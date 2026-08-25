/**
 * @file turbo_script_internal.h
 * @brief Internal TurboScript definitions
 */

#ifndef TURBO_SCRIPT_INTERNAL_H
#define TURBO_SCRIPT_INTERNAL_H

#include "exprtk_module.h"
#include "turbo_buffer.h"
#include <mir.h>
#include "ts_plugin_loader.h"
#include "turbo_script.h"
#include <turbostl/vec.h>
#include <stdatomic.h>

typedef struct ts_timer_scheduler_s ts_timer_scheduler_t;
typedef struct ts_task_scheduler_s ts_task_scheduler_t;
typedef struct coro_cancel_token_s coro_cancel_token_t;

typedef struct imported_module_s {
  char *name;
  exprtk_node_t *expr;
  exprtk_value_t exports;
  int has_exports;
  int isolated;
  struct imported_module_s *next;
} imported_module_t;

#define TS_MAX_PLUGINS 16
#define TS_JIT_CACHE_SIZE 128  // 从 64 扩展到 128，提升缓存命中率

/**
 * @brief JIT 统计数据结构
 */
typedef struct {
  uint64_t compile_count;         // 编译次数
  uint64_t exec_count;            // 执行次数
  uint64_t cache_hit_count;       // 缓存命中次数
  uint64_t cache_miss_count;      // 缓存未命中次数
  uint64_t total_compile_time_us; // 总编译时间（微秒）
  uint64_t total_exec_time_us;    // 总执行时间（微秒）
  uint64_t var_sync_count;        // 变量同步次数
} ts_jit_stats_t;

/* Verify that the internal ts_jit_stats_t and the public turbo_script_jit_stats_t
 * have identical layout.  Both structs are identical by definition; this assert
 * catches any accidental divergence before the pointer cast in
 * turbo_script_get_jit_stats() can produce undefined behaviour. */
#ifdef __cplusplus
static_assert(sizeof(ts_jit_stats_t) == sizeof(turbo_script_jit_stats_t),
              "ts_jit_stats_t and turbo_script_jit_stats_t must have the same size");
#else
_Static_assert(sizeof(ts_jit_stats_t) == sizeof(turbo_script_jit_stats_t),
               "ts_jit_stats_t and turbo_script_jit_stats_t must have the same size");
#endif


struct turbo_script_ctx_s {
  atomic_uint ref_count;
  atomic_int closing;
  /* Immutable TLS address captured by the creating thread. Host ABI entry
   * points compare against this single context-owner fact source. */
  const void *owner_thread_token;
  vec_t host_functions;
  size_t active_host_modules;
  size_t host_callback_depth;
  uint64_t next_instance_generation;
  exprtk_env_t env;
  exprtk_node_t *expr;
  char *expr_source;
  /* 1 when ctx->expr is already tracked in compiled_asts[] (owned by that array).
   * In that case turbo_script_free and ts_prepare_expr must NOT call exprtk_free(ctx->expr)
   * directly — compiled_asts[] cleanup handles it. */
  int expr_in_compiled_asts;
  imported_module_t *imports;
  mem_pool_t scratch_arena;
  char *current_script_dir;
  const char *current_import_name;
  exprtk_value_t current_import_exports;
  int current_import_has_exports;
  exprtk_env_t *current_import_env;
  char error_msg[1024];
  turbo_script_error_code_t error_code;
  turbo_script_memory_policy_t memory_policy;
  size_t peak_context_bytes;
  int memory_exhausted;

  /* Timer callbacks are posted onto this borrowed serialized executor. */
  turbo_script_executor_t executor;
  coro_context_t *coro_ctx;
  ts_timer_scheduler_t *timer_scheduler;
  ts_task_scheduler_t *task_scheduler;

  /* JIT compiling tracking to prevent memory leak and data race */
  int mir_mod_idx;
  exprtk_node_t **compiled_asts;
  size_t compiled_ast_count;
  size_t compiled_ast_capacity;

  /* Plugin handles */
  turbo_script_plugin_authorizer_fn plugin_authorizer;
  void *plugin_authorizer_data;
  ts_plugin_handle_t *plugins[TS_MAX_PLUGINS];
  char *loaded_names[TS_MAX_PLUGINS];
  size_t plugin_count;

  /* MIR JIT compiler context */
  MIR_context_t mir_ctx;
  void *mir_last_fn;      /* Phase 15: cached JIT function pointer */
  int mir_gen_initialized; /* Phase 15: gen_init called once */
  MIR_context_t mir_interp_ctx;
  MIR_item_t mir_interp_last_func;
  int mir_interp_externals_loaded;

  /* Isolated context for script-level mir.load/mir.call to prevent interface clash and duplicate linking */
  MIR_context_t script_mir_ctx;
  int script_mir_linked;

  struct {
    uint64_t hash;
    void *fn_ptr;
    uint32_t access_count;  // LRU 访问计数
    char *script;           // Copy of script string to prevent hash collisions
  } jit_cache[TS_JIT_CACHE_SIZE];

  /* JIT 统计信息 */
  ts_jit_stats_t jit_stats;
  int jit_stats_enabled;  // 是否启用统计
};

struct turbo_script_compiled_s {
  char *source;
};

exprtk_node_t *turbo_script_parse_with_error(turbo_script_ctx_t *ctx, const char *script);
/* Internal deterministic seam: fails the parser arena allocation. */
exprtk_node_t *turbo_script_parse_with_error_test_oom(turbo_script_ctx_t *ctx,
                                                      const char *script);

/* Built-in module accessors */
void turbo_script_register_modules(void);
void turbo_script_register_mir(struct turbo_script_ctx_s *ctx);
exprtk_value_t turbo_script_mir_eval_node(const exprtk_node_t *node, exprtk_env_t *env);
int turbo_script_mir_exec_script_body(exprtk_func_t *func, exprtk_env_t *local_env,
                                      exprtk_env_t *caller_env, exprtk_value_t *out);

/* Internal JIT API - for testing and advanced use */
TURBO_SCRIPT_C_API int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script);
TURBO_SCRIPT_C_API int turbo_script_compile_mir_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast,
                                           const char *script);
TURBO_SCRIPT_C_API int turbo_script_exec_jit(turbo_script_ctx_t *ctx);
TURBO_SCRIPT_C_API int turbo_script_compile_mir_interp(turbo_script_ctx_t *ctx, const char *script);
TURBO_SCRIPT_C_API int turbo_script_compile_mir_interp_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast,
                                                  const char *script);
TURBO_SCRIPT_C_API int turbo_script_exec_mir_interp(turbo_script_ctx_t *ctx);
TURBO_SCRIPT_C_API int turbo_script_exec_mir_interp_result(turbo_script_ctx_t *ctx, double *result_out);
TURBO_SCRIPT_C_API int turbo_script_run_mir_interp(turbo_script_ctx_t *ctx, const char *script);

/* Internal REPL helper */
int turbo_script_repl_run(turbo_script_ctx_t *ctx, const char *script);

/* Context references protect accepted executor tasks during deferred shutdown. */
void ts_context_retain(turbo_script_ctx_t *ctx);
void ts_context_release(turbo_script_ctx_t *ctx);
int ts_context_is_owner_thread(const turbo_script_ctx_t *ctx);

/* Returns the caller environment owned by the currently running managed task,
 * or the root environment when execution is not inside one. */
exprtk_env_t *ts_task_execution_env(turbo_script_ctx_t *ctx);

/* Returns the cooperative cancellation token owned by the current managed task. */
const coro_cancel_token_t *ts_task_cancel_token(turbo_script_ctx_t *ctx);

/* Point-in-time bytes retained by managed task environments. */
size_t ts_task_memory_used(turbo_script_ctx_t *ctx);
size_t ts_task_memory_max_used(turbo_script_ctx_t *ctx);

/* Apply scratch reclamation and hard context quota checks after execution. */
int ts_memory_finish_run(turbo_script_ctx_t *ctx, int result);

#endif /* TURBO_SCRIPT_INTERNAL_H */
