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
typedef struct exprtk_value_s exprtk_value_t;

#define TURBO_SCRIPT_VERSION_MAJOR 1
#define TURBO_SCRIPT_VERSION_MINOR 0
#define TURBO_SCRIPT_VERSION_PATCH 0
#define TURBO_SCRIPT_VERSION_STRING "1.0.0"

typedef enum {
  TURBO_SCRIPT_INIT_DEFAULT = 0,
  TURBO_SCRIPT_INIT_BARE = 1,
} turbo_script_init_flags_t;

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
} turbo_script_error_code_t;

/**
 * @brief Initialize a new Turbo Script context.
 * @param flags TURBO_SCRIPT_INIT_DEFAULT for full init, TURBO_SCRIPT_INIT_BARE for plugin-based
 * init.
 */
CXX_C_API turbo_script_ctx_t *turbo_script_init(turbo_script_init_flags_t flags);

/**
 * @brief Get Turbo Script ABI/API version string.
 */
CXX_C_API const char *turbo_script_version(void);

/**
 * @brief Set the coroutine context for networking/async operations.
 */
CXX_C_API void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, coro_context_t *coro_ctx);

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
 */
typedef exprtk_value_t (*turbo_script_func_t)(size_t arg_count, exprtk_value_t *args,
                                              void *user_data);

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
