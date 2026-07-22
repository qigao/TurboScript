# Task runtime 与协作式取消

## 背景

TurboScript 的 timer、managed task 与网络模块都运行在同一个 CoroNet context 上。
脚本调用采用同步书写方式，网络和定时等待则通过 stackful coroutine 挂起。现有
`coro_task_cancel()` 只能取消未启动的 lazy task，不能安全销毁已经运行的协程栈。

本决策覆盖以下边界：

- TurboScript task 的状态、结果、join 与关闭；
- CoroNet 等待操作的协作式中断；
- TurboHTTP 请求的取消传播；
- timer callback 与交互式 REPL 的事件循环驱动。

## 候选方案

### 强制销毁运行中的 coroutine

拒绝。强制销毁会跳过脚本环境、HTTP transport、连接池和 native callback 的正常
清理路径，无法提供可验证的资源所有权语义。

### TurboScript 私有取消标志

拒绝作为完整方案。私有标志可覆盖 `task.yield()`，但无法唤醒 CoroNet 内部的
DNS、connect、send、recv 或 TLS 等等待，也会迫使 TurboHTTP 依赖 TurboScript。

### CoroNet cancellation source/token

采用。取消能力由实际拥有 event loop 和 I/O wait 的 CoroNet 提供，TurboScript
拥有 source，TurboHTTP 只借用 token。现有无 token API 保持原行为。

## 状态与事实源

Task registry 是脚本 task 状态的唯一事实源。每个 task slot 拥有：

- task ID、生命周期状态与 active 计数；
- 独立 ExprTK caller environment；
- callback、result、error；
- join waiter 链；
- 一个 CoroNet cancellation source。

取消状态转换为：

```text
scheduled/running/waiting -> cancelling -> cancelled
completed/failed/cancelled -> terminal
```

取消请求和正常完成竞争时，以 event-loop owner 线程首先提交的终态为准。终态只提交
一次，并由同一提交路径唤醒全部 join waiter。`task.release()` 只允许释放终态且没有
waiter、活动 cancellation registration 的 slot。

## 所有权与生命周期

| 对象 | 创建者/owner | 借用者 | 失效点 |
|---|---|---|---|
| task slot | TurboScript task registry | status/join/result 调用 | 成功 `task.release()` |
| cancel source | task slot | 无 | slot reset；此前必须无 registration |
| cancel token | cancel source | sleep/join/TurboHTTP request | source 销毁 |
| cancellation registration | 当前等待操作 | cancel source 的 owner-loop 列表 | unregister 完成 |
| HTTP request control | HTTP 调用栈 | `http_request_ex()` | 请求返回 |
| transport | TurboHTTP request/connection pool | cancellation callback | unregister 后 release/discard |

Token 是 borrowed handle，不延长 task slot 生命周期。注册成功后，等待操作必须在每条
返回路径恰好 unregister 一次。取消 callback 只负责中断等待，不释放业务对象。

## 线程与分派协议

- task 状态迁移、registration 增删和取消 callback 都在 CoroNet owner loop 执行；
- 跨线程取消只原子设置 requested，并通过 `coro_post()` 投递一次分派；
- 不在 task registry mutex 或 cancellation list 操作中执行脚本、I/O 或用户 callback；
- HTTP、ExprTK 和 timer callback 只在 owner loop 线程执行；
- REPL stdin 可在调用线程阻塞，但脚本命令必须投递到 runtime owner loop。

取消 source 的 registration 数量受 task capacity 和每个 task 单一活动等待约束。REPL
命令路径默认只允许一个 outstanding command；`coro_post()` 拒绝时命令所有权仍由
提交方持有并立即返回错误。

## 错误语义

- CoroNet 等待返回 `TURBO_ECANCELED`；
- TurboHTTP 映射为追加的 `HTTP_ERROR_CANCELLED`，不得进入 retry；
- TurboScript 映射为追加的 `TURBO_SCRIPT_ERROR_CANCELLED`；
- `task.join()` 目标取消时传播取消错误；
- 重复 cancel 返回 already-terminal/already-requested 结果，不重复唤醒；
- shutdown deadline 到期返回超时，不能销毁仍有活动 registration 的对象。

## Timer 与 REPL

Timer 仍拥有触发计划；callback execution 交给 managed task。周期 timer 在本次 callback
终态后再重排，保持当前 fixed-delay 行为。为保持现有串行语义，timer invocation 使用
串行 lane；普通 `task.spawn()` 不进入该 lane。

交互式 REPL 使用 runtime driver：owner thread 创建 TurboScript/CoroNet context 并持续
运行 event loop，stdin 线程通过 `coro_post()` 提交命令。关闭顺序为停止接收命令、请求
取消、drain、停止 loop、join owner thread、释放 context。

## 兼容性与迁移

- 现有 `http_request()` 委托给无 token 的 `http_request_ex()`；
- 现有 `turbo_script_set_coro_context()` 和 executor API 保留；
- 新错误枚举只追加，不重排已有值；
- 新 task API 是增加项；原有脚本继续运行；
- timer 的 fixed-delay、串行 callback 与错误查询行为保持不变。

迁移顺序：先统一 MIR/解释器调用语义，再实现 CoroNet token，随后接入 TurboHTTP、
TurboScript task/timer，最后启用 REPL runtime driver。每一阶段都可独立回滚到上一层的
无 token 调用，不能保留只在部分 I/O 阶段生效的公开取消 API。

## 验证范围

- cancel-before-start、sleep、join、DNS/connect/send/recv、timeout 与 completion race；
- cancellation callback 恰好一次、registration 销毁次序与 context shutdown drain；
- 取消后的 pooled transport 不复用，HTTP 不重试；
- concise/block arrow 在解释器与 MIR 路径下一致；
- timer callback 的 HTTP/yield/cancel 和 fixed-delay；
- REPL 等待输入时 timer/task 仍推进，EOF/exit 能完整 drain；
- Debug、Release、ASan，并运行 CoroNet shutdown regressions。
