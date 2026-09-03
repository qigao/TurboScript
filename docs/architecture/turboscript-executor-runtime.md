# TurboScript Executor Runtime

## Decision

TurboScript no longer accepts or stores a CoroNet context, cancellation token,
or callback-post executor. Each `turbo_script_ctx_t` owns one Salts coroutine
executor with exactly one shard. Script tasks and timers run only on that
shard, so an ExprTk environment is never concurrently evaluated by two
callbacks.

## Data and ownership protocol

| Item | Owner | Transfer and terminal rule |
| --- | --- | --- |
| executor | `turbo_script_ctx_t` | Created during context initialization; shutdown and destroy occur only after task and timer registries stop admitting work. |
| task/timer job slot | registry | A successful Executor submission borrows the slot until exactly one run or cancel callback and its finalize callback finish. Terminal results remain in the slot until `release` or registry shutdown. |
| script callback and child environment | job slot | Retained before submission; released exactly once by the executor finalizer after execution or admission cancellation. |
| await handle | executor coroutine | Borrowed generation-checked handle. It is consumed by `await`, `await_for`, or `await_abort`; it is never retained after a suspension completes. |
| cancellation | job state under registry mutex | A cancel request wins only from a nonterminal state. It completes a stored join await, and running jobs observe the request at yield/sleep/join and execution boundaries. |

The producer topology is MPSC: host threads may request cancellation or create
jobs, while the sole executor shard evaluates callbacks. The registry mutex is
the authoritative state store. The executor queue is bounded by the configured
task capacity; full or closed admission fails without transferring callback
ownership.

## State transitions

`RESERVED -> SCHEDULED -> RUNNING -> {COMPLETED, FAILED, CANCELLED}`.

`WAITING` is a running job suspended in a Salts Executor await slot. A cancel
request moves a nonterminal job to `CANCELLING`, publishes a cancelled status to
its active await when present, and the shard performs the only terminal cleanup.
No host thread resumes a coroutine directly.

Timers are executor coroutines: `after` and `cron` wait with `await_for`; an
`every` timer repeats that wait while its job remains scheduled. Timer callbacks
therefore share the same serialized evaluator shard as tasks. There is no
external timer wheel or CoroNet event loop to outlive context teardown.

## Failure and shutdown

- Invalid capacity, executor creation failure, queue saturation, stale awaits,
  or a closed executor fail fast and leave callback ownership with the caller.
- `turbo_script_free` first closes both registries, requests cancellation for
  their accepted jobs, waits for the executor drain, then releases registry
  storage and destroys the executor. It is not callable from a script callback.
- A task that never reaches an await/yield boundary cannot be pre-empted; this
  is explicit cooperative cancellation, not a hidden fallback.

## Verification

Cover admission limits, callback completion, timer firing, cancellation before
and during await, join completion-before-suspend, executor shutdown drain, and
interpreter/JIT equivalence. Run the focused TurboScript task, timer, basics,
and JIT suites with the `win-dev-user` preset.
