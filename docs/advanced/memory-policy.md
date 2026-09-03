# 运行时内存策略

TurboScript 以一个 `turbo_script_ctx_t` 作为状态事实源。内存策略在 context
级别配置，并同时约束根环境、managed task、网络输入和 scratch pool；没有全局
共享池，也没有“无限制”或旧式 `ws.recv` 兼容模式。脚本值采用 Native ABI v3
的显式所有权元数据，不再依赖“裸指针恰好活得足够久”的隐含约定。

## 所有权协议

| 存储类别 | 所有者 | 生命周期 | 典型用途 |
| --- | --- | --- | --- |
| definition | 根 `exprtk_env_t::arena` | context | AST、函数、类和模块元数据 |
| managed payload | owner env 的 `mem_pool_t` + `mem_buffer_t` | 最后一个 owner 释放 | string、bytes、bigint、enum、vector、typed-array |
| container | map/list 本身 | replace/delete/destroy | map hash/key pool、list `vec_t`/value pool |
| task | `ts_task_job_t` | `task.release` 或 shutdown | callback 局部状态与终态结果 |
| callback frame | script callback local env | callback 返回 | WebSocket 帧、临时解析结果 |
| builtin scratch | 单次 builtin 调用 | builtin 返回后立即 destroy | 字符串构造、编码和算法临时结果 |
| context scratch | `turbo_script_ctx_t` | run 边界可 reset/trim | 路径、插件装载和宿主临时数据 |
| external | 网络/宿主 | 明确回调期间 | CoroNet 接收缓冲区 |

跨边界只允许四种显式语义：borrow、copy、move、retain：

- `exprtk_env_get()`、`exprtk_map_get()`、iterator 与 `exprtk_list_get()` 返回 borrow；
  replace、delete 或 owner 销毁后立即失效。
- `exprtk_value_copy_to_pool()` / `exprtk_value_copy_to_env()` 建立目标 owner 的副本；
  container 跨 owner 时递归复制，class/function/instance 保持引用身份。
- 同一 env pool 内的 owned managed payload 可由 `exprtk_env_set*()` move/adopt；调用方
  此后不得再 destroy 原参数。解释器的赋值表达式会重新读取槽位并返回 borrow，避免
  同一个 owner 被两个按值副本误释放。
- `exprtk_value_retain()` 仅用于 `mem_buffer_t` 支撑的 payload。map/list 不做伪引用计数，
  必须 copy；`exprtk_value_destroy()` 是 owned 临时值的统一释放入口。

网络缓冲区只在 `ws.consume` callback 期间 borrow。callback 参数绑定会复制到
callback local env；只有 callback 明确返回的值才会复制到 task 环境并逃逸。

`vstr` 只表达 borrowed view，不能成为所有权事实源；`tstr` 用于局部字符串
构建；`mem_pool_t` 用于有明确 reset/destroy 边界的存储；跨 owner 共享的 payload
使用 Salts `mem_buffer_t`。`mem_slice_t` 只适合受控的零拷贝子视图，slice 不得
长于其 buffer owner。vector 与 typed-array 也由 `mem_buffer_t` 支撑，变量替换会把
旧 buffer 归还 owner pool，而不是让环境 arena 单调增长。

Native function ABI v3 的签名是
`(argc, args, exprtk_env_t *env, user_data)`。`env` 是实际调用环境，也是返回值
分配的 owner domain；旧三参数 callback 不会被 wrapper 或 function-pointer cast
适配。静态 `exprtk_builtin_fn` 使用 `(argc, args, env, scratch)`；scratch 只在本次
调用期间有效。`exprtk_call_builtin()` 在销毁 scratch 前把返回值提升到 env owner，
因此 builtin 不得缓存 scratch 指针，也不得绕过该调度入口直接制造逃逸值。

## Profiles 与配额

`turbo_script_memory_policy_init()` 提供 batch、interactive、service、streaming 和
sandbox 五组有界默认值。`turbo_script_init()` 使用 service profile。宿主可复制
profile 后收紧阈值，再通过 `turbo_script_set_memory_policy()` 应用。

四个限制都是 fail-fast 的边界配额：

- `max_context_bytes`：每次 run 返回时检查根环境和 retained task 的总预算。
- `max_task_bytes`：task callback 完成时检查单个 task 环境与终态结果预算。
- `max_external_value_bytes`：单个网络/宿主输入值预算。
- `scratch_trim_threshold_bytes`：run 结束时触发 scratch reset + trim 的阈值。

外部值在复制前检查，因此不会为超额 frame 分配 script storage。task/context 配额
在所有权稳定的完成边界检查；一次尚未返回的 native/字符串操作可能产生瞬态峰值，
宿主仍需用进程级内存限制处理 allocator/OS OOM。无效配置、活动 task/timer 期间的
策略切换、超额外部帧与完成时超额 task 都立即失败。
context 超过总预算后进入 exhausted 状态，后续执行被拒绝；宿主必须读取错误、销毁
context 并按业务恢复，运行时不会静默降级。算法实现中不逃逸的局部工作数组仍可使用
栈、Salts pool 或与第三方 API 对称的 allocator；它们不属于脚本值 owner graph，
也不计作 managed payload 的兼容旁路。

```c
turbo_script_memory_policy_t policy;
turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

if (!ctx || turbo_script_memory_policy_init(TURBO_SCRIPT_MEMORY_STREAMING, &policy) != 0)
  return -1;
policy.max_external_value_bytes = 512U * 1024U;
if (turbo_script_set_memory_policy(ctx, &policy) != 0) {
  turbo_script_free(ctx);
  return -1;
}
```

## Streaming API

`ws.consume(timeout_ms, callback)` 是唯一的 WebSocket 接收入口。transport frame 在
callback 返回后立即释放；旧 `ws.recv()` 已删除，并且不会注册 alias 或复制适配器。

```ts
var ok = ws.consume(5000, (message) => {
    process_message(message);
    return 1;
});
if (ok == 0) throw "receive failed";
```

callback 返回字符串、bytes、map 或 list 时，该返回值会逃逸并计入 task 配额；纯
流处理 callback 应返回数字、布尔或 null。背压是同步的：下一帧只在 callback
完成后接收。

## 方案选择与影响

WebSocket 数据路径的候选方案：

1. context arena 持有每一帧：实现简单，但长连接占用单调增长，排除。
2. 为旧 `ws.recv` 增加隐式 ring/自动失效：会让已保存字符串悬空或偷偷复制，排除。
3. 所有 value（包括容器和对象图）统一引用计数：循环图回收和对象身份成本过高，
   排除；仅对连续 payload 使用 `mem_buffer_t` 引用计数。
4. external borrow + callback-local copy + escape copy：边界明确、每帧可回收，并与
   当前 evaluator 的环境所有权一致，采用。

公开行为变化是 `ws.recv` 被 `ws.consume` 取代；脚本需要把帧处理移入 callback。
HTTP、timer 与 task 的同步脚本语义不变。迁移成本集中在 WebSocket 消费者；构建时
对旧名称的查找失败，运行时调用旧名称也会明确报错。

managed `exprtk_value_t` 元数据改变了 Native ABI，项目版本升为 3.0.0；Unix
shared-library SOVERSION 也随 major version 改为 3。所有宿主 callback 和二进制
插件必须重新编译，不能把 1.x/2.x 插件装入 3.x runtime，也不提供旧布局 adapter。

回滚只能整体回退本次 API/脚本迁移，不提供运行时 fallback。验证范围包括函数
注册、并行 task 连接隔离、等待取消、帧配额、profile 校验、内存统计和 Polymarket
长运行示例。
