# 调度型 Task

TurboScript 的 `task.*` 是基于 CoroNet scheduler 的托管任务层。每次 `task.spawn()` 创建一条由 CoroNet object pool 管理的 coroutine；脚本仍以同步方式编写，但 `task.yield()`、`task.sleep()`、`task.join()` 以及 coroutine-aware HTTP I/O 会把执行权交还 event loop，让其他 task 继续运行。

`task.*` 是 runtime 内建 API，不需要 `import()`。宿主必须先通过 `turbo_script_set_coro_context()` 绑定一个 CoroNet context，并负责驱动该 context。

## 脚本 API

| API | 返回值 | 错误条件 |
| --- | --- | --- |
| `task.spawn(callback)` | 正整数 task ID | callback 不是函数、未配置 CoroNet context、context 正在关闭或容量耗尽 |
| `task.yield()` | `0` | 只能在 `task.spawn` callback 内调用 |
| `task.sleep(delay_ms)` | `0` | 只能在 task 内调用；delay 必须是 `0..4294967294` 的整数 |
| `task.join(task_id)` | 目标 task 的结果 | 只能在 task 内调用；不能 join 自身；目标失败时当前 task 也失败 |
| `task.status(task_id)` | 状态字符串 | ID 类型非法时终止当前执行；未知或已 release 的 ID 返回 `"invalid"` |
| `task.result(task_id)` | 已完成 task 的结果 | 目标尚未完成、失败、取消或 ID 无效 |
| `task.error(task_id)` | 错误字符串 | ID 类型非法时终止当前执行；无错误或未知 ID 返回空字符串 |
| `task.cancel(task_id)` | 首次接受取消返回 `true` | 未知、已进入终态或已在取消的 task 返回 `false` |
| `task.shutdown()` | 本次请求取消的 task 数 | 先停止 timer，再关闭 registry 并取消全部活动 task；后续 timer/task 调度失败 |
| `task.release(task_id)` | `true`/`false` | 活动中或仍被 join 的 task 不可释放；未知 ID 返回 `false` |
| `task.active_count()` | 活动 task 数 | 不接受参数 |

状态值为 `scheduled`、`running`、`waiting`、`cancelling`、`completed`、`failed`、`cancelled` 或 `invalid`。`waiting` 由 `task.sleep()` 和 `task.join()` 显式记录；通用网络 I/O 的 waiting 标志由 CoroNet 内部管理，task registry 在该阶段仍可能显示 `running`。

`join` 不是轮询：joiner 被登记到目标的 wait-list，并标记为 I/O waiting；目标进入终态时一次唤醒所有 joiner。登记是 O(1)，完成时唤醒是 O(w)，其中 w 是该目标的等待者数。

## 并行 HTTP 示例

```javascript
import("net");

var first = task.spawn(() => {
    return http.get("https://example.com/", map { timeout: 5000, transport: "auto" });
});

var second = task.spawn(() => {
    return http.get("https://www.iana.org/", map { timeout: 5000, transport: "auto" });
});

var report = task.spawn(() => {
    var first_response = task.join(first);
    var second_response = task.join(second);
    print(first_response);
    print(second_response);
    task.release(first);
    task.release(second);
    return 0;
});
```

仓库内版本见 [task_parallel_http.tbs](../../examples/task_parallel_http.tbs)。`http.get()` 对当前脚本 task 仍是同步调用：函数返回后下一行才执行；底层连接、发送和接收等待会挂起当前 coroutine，因此其他 task 可以继续运行。这是协作式并发，不是多线程并行，CPU 密集代码必须显式 `task.yield()` 才会让出执行权。

`http.get()` 与 `http.post()` 通过 `CHttp::Client` facade 发起请求。options 中的 `transport` 可取 `"auto"`、`"h1"` 或 `"h2"`；默认 `"auto"` 会先尝试 HTTP/2，只在连接阶段、尚未发送请求数据时回退到 HTTP/1。显式 `"h2"` 不执行 H1 fallback。`ws.*` 继续使用 CoroNet 的 `ws://` / `wss://` 客户端，因为当前 Salts facade 未公开 WebSocket 客户端句柄；现有 task 连接隔离和取消语义不变。

每个 task 拥有一个 CoroNet cancellation source。`task.cancel()` 会唤醒 `task.sleep()`、`task.join()` 以及通过 `turbo_http_request_ex()` 执行的 HTTP 等待；被取消的 HTTP transport 会从连接池丢弃且不进入 retry。取消是协作式的：纯 CPU callback 只能在下一次 `task.yield()`、task API、可取消 I/O 或 callback 返回时观察请求，runtime 不会强制销毁正在运行的栈。

## 结果、容量与错误

- registry 是 task 状态的唯一事实源，拥有 ID、状态、独立 caller env、结果、错误摘要和 wait-list。
- 默认容量是 64，宿主可在没有活动 task 时调用 `turbo_script_set_task_capacity()` 调整，合法范围是 `1..65536`。
- completed/failed 记录不会自动复用。脚本读取结果或错误后应调用 `task.release(id)`，避免长期服务耗尽 registry。
- 每个已使用槽位会初始化一个 64 KiB ExprTK arena；64 个槽位全部使用时，初始 arena 预算约为 `64 × 64 KiB = 4 MiB`。活动 coroutine 的 stack 由 CoroNet pool 另行管理。
- 每个 task 使用独立 ExprTK caller env，所以 `flow`、错误、递归/循环计数和返回值不会与其他 task 共享；闭包捕获值及根环境中的 native/module 函数仍按既有语言规则解析。
- 未捕获异常只把当前 task 标记为 `failed`，不会污染根脚本环境。CLI 在 event loop 排空后发现保留的 failed task 会返回非零退出码。

## 取消与关闭协议

`task.cancel(id)` 只提交请求，终态由 task 自身在 owner loop 提交。取消 joiner 时会先从目标 wait-list 摘除，避免目标 slot 被永久保留；取消 source 的 pending dispatch 还会持有 context 引用，直到 callback 分派结束，防止关闭期间释放 source。`turbo_script_free()` 与 `task.shutdown()` 都禁止新任务并请求取消活动 task，但不会强制展开或销毁 coroutine 栈。

## 宿主集成

```c
coro_context_t *coro = coro_context_create(NULL);
turbo_script_ctx_t *script = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

turbo_script_set_coro_context(script, coro);
if (turbo_script_run_file(script, "parallel.tbs") == 0 &&
    turbo_script_task_active_count(script) > 0) {
  coro_context_run(coro, TURBO_RUN_DEFAULT);
}

if (turbo_script_task_failed_count(script) != 0) {
  /* Script may query task.error(id) before releasing retained records. */
}

turbo_script_free(script);
coro_context_destroy(coro);
```

借用的 CoroNet context 必须覆盖 TurboScript context 及所有已接受 task 的生命周期。不能在 task 活动期间替换或清除它。命令行 `--eval`、`--file` 和交互式 REPL 都会自动创建并驱动 CoroNet loop；REPL 等待 stdin 时由独立 owner thread 继续推进 timer、task 与 HTTP。

## 架构决策

### 背景

`task.*` 是 TurboScript 唯一的脚本级并发模型；它负责调度、任务可见状态与结果归属。

### 候选方案

1. 用线程池运行脚本 callback：ExprTK/MIR、插件实例及根环境是单 owner lane 的可变状态，需要为大量模块重新定义线程安全，排除。
2. 让 timer 的串行 executor 同时执行 task：可保证安全，但一个 task 等待 HTTP 时会阻止其他 task 开始，不能提供调度型并发，排除。
3. 采用 `task.*` 适配层：每个 task 独立 caller env，调度器管理 ready/I/O 状态，registry 管理脚本可见状态与结果。

### 影响、迁移与回滚

- 删除未使用的 `coro.*` generator 插件；`task.*` 是唯一的脚本并发接口。
- `turbo_script_set_coro_context()` 从 timer-only 适配入口扩展为 timer + task 的共同 CoroNet 入口；自定义 timer executor 不能替代 task 所需的 CoroNet scheduler。
- timer 保持串行、不重叠语义；需要并发 I/O 的 timer callback 可快速 `task.spawn()` 后返回。
- `ws.*` 在 managed task 内按 task 隔离连接；多个 task 可并行调用同名的
  `ws.connect/send/consume/close`，不会覆盖彼此的 socket。root 环境中的 `ws.*` 仍保持
  原有单连接语义。task WebSocket 注册表最多同时持有 256 条连接，满时连接失败；
  `task.cancel()` 可中断等待中的 WebSocket connect/send/consume。
- `ws.consume(timeout, callback)` 将帧限制在 callback 生命周期内；只有 callback
  明确返回的值才逃逸到 task 环境并计入 task 配额。仓库中的
  [Polymarket long-run 示例](../../examples/polymarket_multi_channel_long_run.tbs)
  展示了有界接收、有限重连、同步 Observer 背压与采样输出。
- 迁移成本集中在需要后台任务的宿主：绑定并驱动 CoroNet context，并在消费结果后 release。
- 回滚时可移除 `task.*` 注册、task scheduler 生命周期和 CLI task 计数检查；不涉及脚本数据格式迁移，timer 与手动 coro 模块仍可独立工作。

验证范围包括独立调度与 waiting 状态、yield/sleep、join 结果与唤醒、sleep/join/HTTP 取消、shutdown、失败隔离、显式 release、容量耗尽、未配置 CoroNet context，以及 timer/coro 相邻回归。
