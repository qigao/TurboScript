# Timer 与 Cron 调度

TurboScript 原生提供一次性定时、固定延迟 interval 和 5 字段 cron 调度。调度器只决定“何时触发”；脚本回调统一投递到宿主提供的串行 executor，绝不在 TurboUtils 原生 timer 线程中直接进入 ExprTk。

## 脚本 API

| API | 返回值 | 错误条件 |
| --- | --- | --- |
| `timer.after(delay_ms, callback)` | 正整数 job ID | 延迟不是非负整数、超过跨平台原生 timer 上限（约 49.7 天）、callback 不是函数、未配置 executor 或容量耗尽 |
| `timer.every(interval_ms, callback)` | 正整数 job ID | interval 不是正整数、超过相同上限，或同上 |
| `timer.cron(expression, callback)` | 正整数 job ID | 不是有效的 TurboUtils 5 字段 cron 表达式，或同上 |
| `timer.cancel(job_id)` | `true`/`false` | ID 类型非法时终止当前脚本；未知或已结束的 ID 返回 `false` |
| `timer.status(job_id)` | 状态字符串 | ID 类型非法时终止当前脚本；未知 ID 返回 `"invalid"` |
| `timer.error(job_id)` | 错误字符串 | ID 类型非法时终止当前脚本；没有错误或未知 ID 返回空字符串 |

状态值为 `scheduled`、`pending`、`running`、`completed`、`cancelled`、`failed` 或 `invalid`。终态记录保留到该容量槽被后续 job 复用；调度器的默认容量是 64，可由宿主在没有活动 job 时通过 `turbo_script_set_timer_capacity()` 调整。

```javascript
import("timer");

var heartbeat = timer.every(30000, () => {
    print("heartbeat");
});

// 在另一个已取得 heartbeat 的作用域中可显式取消：
// timer.cancel(heartbeat);
```

用于周期检查网站时，HTTP 调用保持同步脚本语义：回调要等 `http.get()` 返回后才继续；内置 CoroNet executor 会把每次 callback invocation 提交为 managed task，因此网络等待会让出 event loop，而不是阻塞 timer 线程或嵌套驱动同一个 loop：

```javascript
import("net");
import("timer");

var check_site = () => {
    var response = http.get(
        "https://example.com/",
        map { timeout: 5000 }
    );
    print(response);
};

var watch_id = timer.every(30000, check_site);
```

仓库内可运行版本见 [timer_website_watch.tbs](../../examples/timer_website_watch.tbs)。命令行 `--eval`、`--file` 和交互式 REPL 都会创建并驱动 CoroNet loop；REPL 等待下一行输入时 timer 仍会触发。

timer callback 彼此仍严格串行。若一次触发需要同时等待多个网站，可在 callback 中调用 `task.spawn()`，由独立的 [调度型 Task](./tasks.md) 承担并发 I/O，再让 timer callback 尽快返回。

## 调度语义

- `after` 成功执行一次后进入 `completed`。
- `every` 使用固定延迟且不重叠：本次回调结束后才开始计算下一段 interval。
- cron 使用本地墙钟；每次成功回调后从当前时间计算下一次，不补放错过的历史触发。
- 使用内置 CoroNet executor 时，`timer.cancel()` 会请求取消正在执行 callback 的 managed task，可中断 `task.sleep()`、`task.join()` 和 HTTP 等待；纯 CPU 代码仍需到协作点才会停止。自定义 executor 保持原有“禁止再次调度、不强拆运行栈”语义。
- 未捕获错误使当前 job 进入 `failed`，可用 `timer.error(id)` 查询；它不会继续重试。
- job 表有硬容量上限，不会在长期运行中无界增长。

## 宿主集成

首选 CoroNet 适配器：

```c
coro_context_t *coro = coro_context_create(NULL);
turbo_script_ctx_t *script = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

turbo_script_set_coro_context(script, coro);
if (turbo_script_run_file(script, "watch.tbs") == 0 &&
    turbo_script_timer_active_count(script) > 0) {
  coro_context_run(coro, TURBO_RUN_DEFAULT);
}

turbo_script_free(script);
coro_context_destroy(coro);
```

其他宿主可注入 `turbo_script_executor_t`。executor 必须串行执行同一 context 的任务；`post()` 返回成功后必须恰好执行一次任务。executor 与其 data 为借用关系，而且在已接受任务全部运行前必须保持有效。自定义 executor 若要在 callback 内使用 TurboHTTP，还必须让任务运行于可驱动该 HTTP client 的 coroutine/event-loop 上下文。

宿主可用 `turbo_script_timer_active_count()` 判断是否需要继续驱动事件循环，并用 `turbo_script_timer_failed_count()` 判断是否存在仍保留在 job 表中的失败记录。命令行模式检测到 timer callback 失败时会返回非零退出码。

## 架构决策

### 背景与状态归属

TurboUtils `turbo_timer_t` 的回调运行在 OS 线程池或专用线程，而 `turbo_script_ctx_t` 的 ExprTk/MIR 状态是单线程可变状态。直接从 timer 线程调用脚本会让环境、AST、插件和错误状态发生数据竞争。

调度器是 job 生命周期的唯一事实源，拥有 job 状态、原生 timer、cron 表达式、回调引用和错误摘要。executor 只拥有任务队列与执行线程，不复制 job 状态。

### 候选方案

1. timer 线程直接调用脚本：改动最小，但破坏现有单线程环境约束，排除。
2. 每个 job 使用 `turbo_cron_runner` 或独立 worker：线程数和关闭路径随 job 数增长，且仍需跨线程进入脚本，排除。
3. 调度器加窄串行 lane：原生 timer/cron 只负责唤醒；CoroNet 适配器按队列逐个提交 managed task，前一 invocation 终态后才启动下一项，脚本状态只在 owner thread 改变，采用。
4. 让 timer scheduler 同时承担通用 task：会把 timer 的串行、不重叠语义与 task 的多 coroutine 调度混为一体，排除；通用调度另由独立 `task.*` 层复用同一个 CoroNet context。

### 权衡、迁移与回滚

- 性能：每次触发增加一次跨线程 post 和一次 managed-task 调度；内置适配器复用 CoroNet context，不建立 per-job 线程。换取统一取消传播、单线程脚本状态与确定的非重叠语义。
- 依赖：`turbo_script` 新增 `Rocida::Cron` 与 `TurboNet::CoroNet` 私有链接依赖。
- 兼容性：既有同步脚本行为不变；只有使用 timer 的宿主必须提供 executor。原来只有声明而无实现的 `turbo_script_set_coro_context()` 现在成为兼容适配入口。
- 关闭顺序：先标记 context closing，再停止/销毁原生 timers；已接受的 executor task 持有 context 引用，执行或跳过后才允许最终释放 ExprTk/MIR 状态。
- 回滚：移除 timer 函数注册与 CLI loop 驱动即可恢复旧行为；调度代码独立在 `turbo_script_timer.c`，不需要迁移脚本数据格式。

验证范围至少包括原生触发到 CoroNet 投递、取消竞态、回调错误、cron 校验、容量耗尽、context 关闭，以及既有 TurboScript basics/MIR 测试。
