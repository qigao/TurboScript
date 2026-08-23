/**
 * @file exprtk.h
 * @brief exprtk-like Parser Public API
 */

#ifndef exprtk_H
#define exprtk_H

#include "exprtk_export.h"
#include "platform.h"
#include "exprtk_module.h"
#include "exprtk_types.h"
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif


// Parse the input string into an AST (simplified API, creates internal arena if needed)
EXPRTK_C_API exprtk_node_t *exprtk_parse(const char *input, size_t length);

// Extended parse API allowing custom arena and error reporting
EXPRTK_C_API exprtk_node_t *exprtk_parse_ext(const char *input, size_t length, mem_pool_t *arena, int *error,
                                          char *error_msg, size_t error_msg_len);

// Validate AST (undefined variables, duplicate params, simple type checks)
EXPRTK_C_API int exprtk_validate(exprtk_node_t *root, exprtk_env_t *env, char *error_msg, size_t msg_len);

// Clean up (frees the arena)
EXPRTK_C_API void exprtk_free(exprtk_node_t *node);

// Evaluate AST
EXPRTK_C_API exprtk_value_t exprtk_eval(const exprtk_node_t *node, exprtk_env_t *env);

// Complexity checks
EXPRTK_C_API size_t exprtk_node_count(const exprtk_node_t *node);
EXPRTK_C_API size_t exprtk_node_depth(const exprtk_node_t *node);

// Node allocation (internal/parser use)
EXPRTK_C_API exprtk_node_t *exprtk_node_create(mem_pool_t *arena, exprtk_node_type_t type);

// Environment management
EXPRTK_C_API void exprtk_env_init(exprtk_env_t *env);
EXPRTK_C_API void exprtk_env_free(exprtk_env_t *env);

/** Retain a captured snapshot environment (no-op for non-snapshot scopes). */
EXPRTK_C_API void exprtk_env_retain(exprtk_env_t *env);

/**
 * Create a captured snapshot that copies only the named variables instead of
 * the whole environment. Closures capture their free variables this way so
 * repeated closure assignment does not chain-capture the previous closure.
 * Pass NULL/0 for names to copy everything (class closures, module imports).
 */
EXPRTK_C_API exprtk_env_t *exprtk_env_snapshot_names(exprtk_env_t *env,
                                                  const char *const *names,
                                                  size_t name_count);

/**
 * Collect variable names referenced by @p body (recursively, including nested
 * function bodies) excluding @p arg_params. Callers use the result to build a
 * capturing snapshot. Returns a NULL-terminated string array or NULL on OOM;
 * the array and strings are allocated in @p arena.
 */
EXPRTK_C_API char **exprtk_collect_closure_free_vars(const exprtk_node_t *body,
                                                  exprtk_node_t *const *arg_params,
                                                  size_t arg_count,
                                                  mem_pool_t *arena);

/** Release a captured snapshot environment; freed when the last reference drops. */
EXPRTK_C_API void exprtk_env_release(exprtk_env_t *env);

/**
 * Reclaim snapshot environments on @p root closure chain that were created by
 * the calling thread and are no longer referenced by any function value or
 * method. Call after a script execution boundary (or a timer/task callback)
 * so short-lived closures do not accumulate. Thread-local ownership makes
 * concurrent sweeps from the host and the event-loop thread safe.
 */
EXPRTK_C_API void exprtk_env_sweep_closures(exprtk_env_t *root);
/**
 * Store a value in an environment owner domain. Borrowed and cross-domain
 * values are copied. An owned mem_buffer payload already belonging to this
 * environment may be adopted; callers must then treat the argument as moved.
 */
EXPRTK_C_API void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value);
EXPRTK_C_API void exprtk_env_set_local(exprtk_env_t *env, const char *name, exprtk_value_t value);
/** Return a borrowed value invalidated by replacement or environment release. */
EXPRTK_C_API exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name);
/** Return an independently owned value; destroy it unless ownership is moved. */
EXPRTK_C_API exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value,
                                                    exprtk_env_t *dst_env);
EXPRTK_C_API int exprtk_env_has(exprtk_env_t *env, const char *name);
EXPRTK_C_API void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn,
                                        void *user_data);
EXPRTK_C_API int exprtk_env_has_func(exprtk_env_t *env, const char *name);
EXPRTK_C_API void exprtk_env_set_constant(exprtk_env_t *env, const char *name, exprtk_value_t value);
EXPRTK_C_API void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod);

// OOP - Class registration
EXPRTK_C_API void exprtk_env_register_class(exprtk_env_t *env, const char *name, struct exprtk_class_s *klass);
EXPRTK_C_API struct exprtk_class_s *exprtk_env_lookup_class(exprtk_env_t *env, const char *name);

EXPRTK_C_API int exprtk_env_last_line(const exprtk_env_t *env);
EXPRTK_C_API int exprtk_env_last_column(const exprtk_env_t *env);

// Global Registry
EXPRTK_C_API void exprtk_registry_init(void);
EXPRTK_C_API void exprtk_registry_add_module(const exprtk_module_t *mod);
EXPRTK_C_API exprtk_builtin_fn exprtk_registry_find(const char *name);

/**
 * @brief Full internal call dispatch (user functions + module registry).
 * Called by the evaluator for NODE_FUNCTION_CALL.
 */
EXPRTK_C_API exprtk_value_t exprtk_call_internal(const char *name, size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env);

/**
 * Invoke a builtin with an ephemeral scratch pool and promote its result into
 * @p env. Builtins must not retain scratch pointers after returning.
 */
EXPRTK_C_API exprtk_value_t exprtk_call_builtin(exprtk_builtin_fn fn, size_t argc,
                                             exprtk_value_t *args, exprtk_env_t *env);

EXPRTK_C_API exprtk_builtin_fn exprtk_find_builtin(const char *name, exprtk_env_t *env);

/* OOP runtime entry points used by JIT lowering. These operate on already-lowered
 * names and numeric arguments instead of evaluating arbitrary AST expressions. */
EXPRTK_C_API exprtk_value_t exprtk_oop_define_class(const exprtk_node_t *node, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_alias_class(const char *target_name, const char *source_name,
                                                exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_instantiate_numeric(const char *class_name, size_t argc,
                                                        const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_instantiate_class_value(exprtk_value_t class_value,
                                                            const char *class_name, size_t argc,
                                                            exprtk_value_t *args,
                                                            exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_numeric(const char *object_name,
                                                        const char *method_name, size_t argc,
                                                        const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_checked_numeric(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_checked_values(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, exprtk_value_t *args, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_checked_value_nodes(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    const exprtk_node_t *call_node, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_member_call_checked_values(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, exprtk_value_t *args, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_member_call_checked_numeric(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_member_call_checked_value_nodes(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    const exprtk_node_t *call_node, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_mono_numeric(const char *object_name,
                                                             const char *expected_class_name,
                                                             const char *method_name, size_t argc,
                                                             const double *argv,
                                                             exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_mono_checked_numeric(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const exprtk_node_t *object_node, size_t argc, const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_mono_checked_value_nodes(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const exprtk_node_t *object_node, const exprtk_node_t *call_node, exprtk_env_t *env);
typedef struct exprtk_oop_method_cache_s {
  struct exprtk_class_s *last_class;
  exprtk_func_t *method;
  uint64_t arg_signature;
  size_t argc;
  int is_static;
} exprtk_oop_method_cache_t;
typedef struct exprtk_oop_field_cache_s {
  void *owner;
  struct exprtk_class_s *field_owner;
  uint64_t version;
  exprtk_value_t *slot;
  int access_level;
  int is_static;
} exprtk_oop_field_cache_t;
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_cached_numeric(
    const char *object_name, const char *method_name, exprtk_oop_method_cache_t *cache,
    size_t argc, const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_method_cached_checked_numeric(
    const char *object_name, const char *method_name, exprtk_oop_method_cache_t *cache,
    const exprtk_node_t *object_node, size_t argc, const double *argv, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_call_bound_method(exprtk_value_t bound_method, size_t argc,
                                                      exprtk_value_t *args, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_call_function_value(exprtk_value_t function_value, size_t argc,
                                                    exprtk_value_t *args, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_get_member(const char *object_name, const char *member_name,
                                               exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_get_member_checked(
    const char *object_name, const char *member_name, const exprtk_node_t *object_node,
    exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_get_member_cached_checked(
    const char *object_name, const char *member_name, exprtk_oop_field_cache_t *cache,
    const exprtk_node_t *object_node, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_set_member_numeric(const char *object_name,
                                                       const char *member_name, double value,
                                                       exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_set_member_checked_numeric(
    const char *object_name, const char *member_name, double value,
    const exprtk_node_t *object_node, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_set_member_cached_checked_numeric(
    const char *object_name, const char *member_name, double value,
    exprtk_oop_field_cache_t *cache, const exprtk_node_t *object_node, exprtk_env_t *env);
EXPRTK_C_API exprtk_value_t exprtk_oop_instanceof_name(const char *object_name,
                                                    const char *class_name, exprtk_env_t *env);

EXPRTK_C_API exprtk_node_t *exprtk_node_copy(const exprtk_node_t *src, mem_pool_t *dest_arena);
EXPRTK_C_API exprtk_node_t *exprtk_node_create(mem_pool_t *arena, exprtk_node_type_t type);
EXPRTK_C_API void eval_destructure(exprtk_node_t *target, exprtk_value_t rhs, exprtk_env_t *env,
                                int is_constant);
EXPRTK_C_API void exprtk_env_init_child(exprtk_env_t *env, exprtk_env_t *parent);

/* =========================================================================
 * Coroutine API
 * ========================================================================= */

/**
 * @brief 创建协程实例
 * @param env 父环境
 * @param generator_node 生成器函数节点（EXPRTK_NODE_GENERATOR_FUNCTION）
 * @param args 初始化参数
 * @param arg_count 参数数量
 * @return 协程实例，或 NULL
 */
EXPRTK_C_API exprtk_coroutine_t *exprtk_coroutine_create(
    exprtk_env_t *env,
    exprtk_node_t *generator_node,
    exprtk_value_t *args,
    size_t arg_count
);

/**
 * @brief 恢复协程执行（next）
 * @param coro 协程实例
 * @return 1 如果有值，0 如果完成，-1 如果错误
 */
EXPRTK_C_API int exprtk_coroutine_next(exprtk_coroutine_t *coro);

/**
 * @brief 向协程发送值（send）
 * @param coro 协程实例
 * @param value 发送的值
 * @return 1 如果有值，0 如果完成，-1 如果错误
 */
EXPRTK_C_API int exprtk_coroutine_send(exprtk_coroutine_t *coro, exprtk_value_t value);

/**
 * @brief 获取协程当前值
 * @param coro 协程实例
 * @return 当前 yield 的值
 */
EXPRTK_C_API exprtk_value_t exprtk_coroutine_value(const exprtk_coroutine_t *coro);

/**
 * @brief 检查协程是否完成
 * @param coro 协程实例
 * @return 1 如果完成，0 如果未完成
 */
EXPRTK_C_API int exprtk_coroutine_done(const exprtk_coroutine_t *coro);

/**
 * @brief 增加协程引用计数
 * @param coro 协程实例
 */
EXPRTK_C_API void exprtk_coroutine_retain(exprtk_coroutine_t *coro);

/**
 * @brief 减少协程引用计数，为 0 时释放
 * @param coro 协程实例
 */
EXPRTK_C_API void exprtk_coroutine_release(exprtk_coroutine_t *coro);

#ifdef __cplusplus
}
#endif

#endif // exprtk_H
