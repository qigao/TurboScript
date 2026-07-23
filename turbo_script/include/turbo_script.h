#ifndef TURBO_SCRIPT_H
#define TURBO_SCRIPT_H

#include "platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
 
#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_script_ctx_s turbo_script_ctx_t;
typedef struct turbo_script_compiled_s turbo_script_compiled_t;
typedef struct coro_context_s coro_context_t;
typedef struct coro_cancel_token_s coro_cancel_token_t;
typedef struct exprtk_value_s exprtk_value_t;
typedef struct exprtk_env_s exprtk_env_t;

/** Callback executed by a TurboScript timer executor on the context owner thread. */
typedef void (*turbo_script_executor_task_fn)(void *arg1, void *arg2);

/**
 * @brief Post one timer callback to a serialized TurboScript execution context.
 *
 * A successful post must execute @p task exactly once. The executor must not
 * run two tasks for the same TurboScript context concurrently.
 *
 * @return 0 when accepted, or a negative value on failure.
 */
typedef int (*turbo_script_executor_post_fn)(void *executor_data,
                                             turbo_script_executor_task_fn task,
                                             void *arg1, void *arg2);

/** Borrowed timer-executor descriptor copied by turbo_script_set_executor(). */
typedef struct {
  turbo_script_executor_post_fn post;
  void *data;
} turbo_script_executor_t;

/**
 * @brief Borrowed iterator over a TurboScript map value.
 *
 * The iterator and returned key pointers become invalid when the source map is
 * modified or released. Callers must not retain them across those operations.
 */
typedef struct {
  void *storage;
  size_t position;
  size_t bound;
} turbo_script_value_map_iterator_t;

#define TURBO_SCRIPT_VERSION_MAJOR 3
#define TURBO_SCRIPT_VERSION_MINOR 0
#define TURBO_SCRIPT_VERSION_PATCH 0
#define TURBO_SCRIPT_VERSION_STRING "3.0.0"
#define TURBO_SCRIPT_NATIVE_ABI_VERSION 3

typedef enum {
  TURBO_SCRIPT_INIT_DEFAULT = 0,
  TURBO_SCRIPT_INIT_BARE = 1,
} turbo_script_init_flags_t;

/** Runtime memory profiles. Each profile has bounded, fail-fast defaults. */
typedef enum {
  TURBO_SCRIPT_MEMORY_BATCH = 0,
  TURBO_SCRIPT_MEMORY_INTERACTIVE,
  TURBO_SCRIPT_MEMORY_SERVICE,
  TURBO_SCRIPT_MEMORY_STREAMING,
  TURBO_SCRIPT_MEMORY_SANDBOX,
} turbo_script_memory_profile_t;

/**
 * @brief Memory ownership and quota policy for one TurboScript context.
 *
 * Limits are boundary quotas in bytes. External-value limits are checked before
 * copying; task and context retention limits are checked at completion/run
 * boundaries. A zero limit is invalid and the policy is copied by the context.
 */
typedef struct {
  turbo_script_memory_profile_t profile;
  size_t max_context_bytes;
  size_t max_task_bytes;
  size_t max_external_value_bytes;
  size_t scratch_trim_threshold_bytes;
} turbo_script_memory_policy_t;

typedef struct {
  size_t context_bytes;
  size_t task_bytes;
  size_t scratch_bytes;
  size_t peak_context_bytes;
} turbo_script_memory_stats_t;

typedef enum {
  TURBO_SCRIPT_ERROR_NONE = 0,
  TURBO_SCRIPT_ERROR_ARGUMENT,
  TURBO_SCRIPT_ERROR_PARSE,
  TURBO_SCRIPT_ERROR_VALIDATE,
  TURBO_SCRIPT_ERROR_RUNTIME,
  TURBO_SCRIPT_ERROR_IO,
  TURBO_SCRIPT_ERROR_PLUGIN,
  TURBO_SCRIPT_ERROR_JIT,
  TURBO_SCRIPT_ERROR_OOM,
  TURBO_SCRIPT_ERROR_STATE,
  TURBO_SCRIPT_ERROR_CANCELLED,
} turbo_script_error_code_t;

/**
 * @brief Initialize a new Turbo Script context.
 * @param flags TURBO_SCRIPT_INIT_DEFAULT for full init, TURBO_SCRIPT_INIT_BARE for plugin-based
 * init.
 */
CXX_C_API turbo_script_ctx_t *turbo_script_init(turbo_script_init_flags_t flags);

/** Fill @p policy with the bounded defaults for @p profile. */
CXX_C_API int turbo_script_memory_policy_init(turbo_script_memory_profile_t profile,
                                              turbo_script_memory_policy_t *policy);

/**
 * @brief Replace the context memory policy.
 *
 * This operation fails while managed tasks or timers are active, after a hard
 * memory limit has been exceeded, or when the new limits are already below
 * current usage. There is no compatibility/unbounded mode.
 */
CXX_C_API int turbo_script_set_memory_policy(turbo_script_ctx_t *ctx,
                                             const turbo_script_memory_policy_t *policy);

/** Return a point-in-time memory usage snapshot. */
CXX_C_API int turbo_script_get_memory_stats(turbo_script_ctx_t *ctx,
                                            turbo_script_memory_stats_t *stats);

/**
 * @brief Get Turbo Script ABI/API version string.
 */
CXX_C_API const char *turbo_script_version(void);

/**
 * @brief Set the serialized executor used by timer callbacks.
 *
 * The executor and its data are borrowed and must remain valid until all
 * accepted callbacks have run. Replacing an executor while timer jobs or
 * managed tasks are active fails. Passing NULL clears the executor and the
 * CoroNet task context when both subsystems are idle. A custom timer executor
 * does not provide the scheduler required by task.spawn().
 *
 * @return 0 on success, or -1 on invalid state or arguments.
 */
CXX_C_API int turbo_script_set_executor(turbo_script_ctx_t *ctx,
                                        const turbo_script_executor_t *executor);

/**
 * @brief Use a CoroNet context for timer callbacks and managed script tasks.
 *
 * Timer callbacks run sequentially in one managed coroutine. Each task.spawn
 * callback runs in its own managed coroutine and may cooperatively join, yield,
 * sleep, or perform coroutine-aware I/O.
 * The CoroNet context is borrowed and must outlive the TurboScript context,
 * all accepted callbacks, and all managed tasks. This compatibility API records errors on @p ctx;
 * use turbo_script_set_executor() when the caller needs a return code.
 * Script callbacks with deep class or collection call graphs may need a larger
 * coroutine stack. Context owners can configure this through
 * coro_context_create_ex(). The TurboScript CLI passes stack_size == 0 by
 * default so TurboUtils owns the default; --coro-stack-kib sets an override.
 */
CXX_C_API void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, coro_context_t *coro_ctx);

/**
 * @brief Configure the bounded timer job registry.
 *
 * This may only be changed while no timer jobs are active. Terminal job status
 * is discarded when the capacity changes.
 *
 * @return 0 on success, or -1 on invalid state, zero capacity, or allocation failure.
 */
CXX_C_API int turbo_script_set_timer_capacity(turbo_script_ctx_t *ctx, size_t capacity);

/** @brief Return the number of scheduled, queued, or running timer jobs. */
CXX_C_API size_t turbo_script_timer_active_count(turbo_script_ctx_t *ctx);

/** @brief Return the number of retained timer jobs in the failed state. */
CXX_C_API size_t turbo_script_timer_failed_count(turbo_script_ctx_t *ctx);

/**
 * @brief Configure the bounded managed-task registry.
 *
 * This may only be changed while no task is active. Retained terminal results
 * are discarded. Script code must call task.release(id) before a terminal slot
 * can be reused during normal operation.
 *
 * @return 0 on success, or -1 on invalid state, capacity, or allocation failure.
 */
CXX_C_API int turbo_script_set_task_capacity(turbo_script_ctx_t *ctx, size_t capacity);

/** @brief Return the number of scheduled, running, or waiting managed tasks. */
CXX_C_API size_t turbo_script_task_active_count(turbo_script_ctx_t *ctx);

/** @brief Return the number of retained managed tasks in the failed state. */
CXX_C_API size_t turbo_script_task_failed_count(turbo_script_ctx_t *ctx);

/**
 * @brief Return the borrowed cancellation token for the running managed task.
 *
 * Native modules may pass this token to coroutine-aware operations. The token
 * is NULL outside task.spawn() and must not be retained after the native call
 * returns.
 */
CXX_C_API const coro_cancel_token_t *turbo_script_current_task_cancel_token(void);

/**
 * @brief Free a Turbo Script context.
 */
CXX_C_API void turbo_script_free(turbo_script_ctx_t *ctx);

/**
 * @brief Run a script from a string using the MIR interpreter backend.
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure.
 */
CXX_C_API int turbo_script_run(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script using the MIR JIT backend for maximum performance.
 *
 * TurboScript uses one MIR lowering pipeline for both execution modes:
 * turbo_script_run() executes the generated MIR with MIR's interpreter, while
 * turbo_script_run_jit() lowers the same script to MIR and asks MIR to generate native code.
 *
 * Supported coverage includes numeric expressions, control flow, user functions,
 * strings/templates, maps/vectors/lists, destructuring, try/catch/throw, async/await
 * syntax helpers, parser/data bindings, matrix/statistics helpers, and the current OOP
 * model through MIR runtime helpers and OOP callsite caches.
 *
 * Unsupported syntax or runtime forms must fail with a compile/runtime error from
 * the MIR pipeline so JIT and MIR interpreter semantics stay visible and testable.
 *
 * Performance tips:
 * - Pre-bind vectors with ts_bind_vec() for native pointer access where possible.
 * - Keep hot loops numeric when maximum throughput matters.
 * - Compile once and execute repeatedly when a script is reused.
 *
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure (check turbo_script_get_error_code()).
 */
CXX_C_API int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script and print the result to stdout (for REPL use).
 * @param ctx Context.
 * @param script Script content.
 * @return 0 on success, <0 on failure.
 */
CXX_C_API int turbo_script_run_and_print(turbo_script_ctx_t *ctx, const char *script);

/**
 * @brief Run a script from a file using the MIR interpreter backend.
 * @param ctx Context.
 * @param filename File path.
 * @return 0 on success, <0 on failure.
 */
CXX_C_API int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename);

/**
 * @brief Compile a script into a reusable source-backed object for MIR interpreter execution.
 * @return Compiled object, or NULL on parse error.
 */
CXX_C_API turbo_script_compiled_t *turbo_script_compile(turbo_script_ctx_t *ctx,
                                                        const char *script);

/**
 * @brief Execute a previously compiled script.
 */
CXX_C_API int turbo_script_exec(turbo_script_ctx_t *ctx, turbo_script_compiled_t *compiled);

/**
 * @brief Free a compiled script object.
 */
CXX_C_API void turbo_script_compiled_free(turbo_script_compiled_t *compiled);

/**
 * @brief Get error message of last operation.
 */
CXX_C_API const char *turbo_script_get_error(turbo_script_ctx_t *ctx);

/**
 * @brief Get categorized error code of last operation.
 */
CXX_C_API turbo_script_error_code_t turbo_script_get_error_code(turbo_script_ctx_t *ctx);

/**
 * @brief Bind a number variable into the script environment.
 */
CXX_C_API void ts_bind_num(turbo_script_ctx_t *ctx, const char *name, double value);

/**
 * @brief Bind a string variable into the script environment.
 */
CXX_C_API void ts_bind_str(turbo_script_ctx_t *ctx, const char *name, const char *value);

/**
 * @brief Bind a double[] vector into the script environment.
 */
CXX_C_API int ts_bind_vec(turbo_script_ctx_t *ctx, const char *name, const double *data,
                          size_t len);

/**
 * @brief Native function signature for user-registered functions.
 *
 * @p env is the exact invocation environment and owns returned arena-backed
 * values. Native ABI v3 includes managed exprtk_value_t ownership metadata;
 * callbacks compiled for older layouts are not adapted.
 */
typedef exprtk_value_t (*turbo_script_func_t)(size_t arg_count, exprtk_value_t *args,
                                              exprtk_env_t *env, void *user_data);

/**
 * @brief Register a native C function callable from script.
 */
CXX_C_API void ts_bind_func(turbo_script_ctx_t *ctx, const char *name, turbo_script_func_t fn,
                            void *user_data);

/**
 * @brief Get a number variable from the script environment.
 * Returns 0.0 if not found or not a number.
 */
CXX_C_API double ts_get_num(turbo_script_ctx_t *ctx, const char *name);

/**
 * @brief Get a string variable from the script environment.
 * @return Pointer to null-terminated string, or NULL if not found/not a string.
 */
CXX_C_API const char *ts_get_str(turbo_script_ctx_t *ctx, const char *name);

/**
 * @brief Get a vector variable from the script environment.
 * @return 0 on success, -1 if not found or not a vector.
 */
CXX_C_API int ts_get_vec(turbo_script_ctx_t *ctx, const char *name, const double **data,
                         size_t *len);

/**
 * @brief Load a plugin by name from C code.
 * Equivalent to import("name") in script.
 * @return 0 on success, -1 on failure.
 */
CXX_C_API int turbo_script_load_plugin(turbo_script_ctx_t *ctx, const char *name);

/**
 * @brief Convert an exprtk_value_t to boolean (0.0 or 1.0).
 */
CXX_C_API bool turbo_script_value_as_bool(exprtk_value_t val);

/**
 * @brief Create an empty map value owned by the TurboScript value runtime.
 */
CXX_C_API exprtk_value_t turbo_script_value_map(void);

/**
 * @brief Set a map entry, copying the key and value according to TurboScript value semantics.
 */
CXX_C_API void turbo_script_value_map_set(exprtk_value_t *map, const char *key,
                                          exprtk_value_t value);

/**
 * @brief Start iterating over a map without taking ownership of it.
 */
CXX_C_API turbo_script_value_map_iterator_t
turbo_script_value_map_iter_begin(const exprtk_value_t *map);

/**
 * @brief Read the next map entry.
 * @return 1 when an entry was produced, 0 when iteration is complete or invalid.
 */
CXX_C_API int turbo_script_value_map_iter_next(turbo_script_value_map_iterator_t *iterator,
                                               const char **key, exprtk_value_t *value);

/**
 * @brief Create a list value that borrows the supplied item storage.
 *
 * The returned value does not free @p items. The storage must remain valid for
 * as long as TurboScript can observe the returned list.
 */
CXX_C_API exprtk_value_t turbo_script_value_list_borrowed(exprtk_value_t *items, size_t count);

/* ========================================================================
 * JIT Statistics API
 * ======================================================================== */

/**
 * @brief JIT 统计数据结构（公开版本）
 */
typedef struct {
  uint64_t compile_count;         ///< 编译次数
  uint64_t exec_count;            ///< 执行次数
  uint64_t cache_hit_count;       ///< 缓存命中次数
  uint64_t cache_miss_count;      ///< 缓存未命中次数
  uint64_t total_compile_time_us; ///< 总编译时间（微秒）
  uint64_t total_exec_time_us;    ///< 总执行时间（微秒）
  uint64_t var_sync_count;        ///< 变量同步次数
} turbo_script_jit_stats_t;

/**
 * @brief 启用或禁用 JIT 统计收集
 * 
 * 启用统计会带来极小的性能开销（< 1%）。
 * 
 * @param ctx Context
 * @param enable 1 启用，0 禁用
 */
CXX_C_API void turbo_script_enable_jit_stats(turbo_script_ctx_t *ctx, int enable);

/**
 * @brief 获取 JIT 统计数据
 * 
 * @param ctx Context
 * @return 统计数据指针（只读），如果统计未启用则返回 NULL
 */
CXX_C_API const turbo_script_jit_stats_t *turbo_script_get_jit_stats(turbo_script_ctx_t *ctx);

/**
 * @brief 重置 JIT 统计计数器
 * 
 * @param ctx Context
 */
CXX_C_API void turbo_script_reset_jit_stats(turbo_script_ctx_t *ctx);

/**
 * @brief 打印 JIT 统计信息到文件流（用于调试）
 * 
 * 输出格式化的统计报告，包括：
 * - 编译和执行次数
 * - 缓存命中率
 * - 平均编译/执行时间
 * - 通用回退计数（当前应为 0）和变量同步次数
 * 
 * @param ctx Context
 * @param fp 文件流（如 stdout, stderr 或文件）
 */
CXX_C_API void turbo_script_print_jit_stats(turbo_script_ctx_t *ctx, FILE *fp);

#ifdef __cplusplus
}
#endif

#endif // TURBO_SCRIPT_H
