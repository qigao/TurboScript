/**
 * @file turbo_script_mir_internal.h
 * @brief Internal shared definitions for MIR backend modules
 * 
 * This header is shared across all MIR backend modules:
 * - mir_compiler.c: Compiler framework and context management
 * - mir_lowering.c: AST to MIR lowering
 * - mir_optimize.c: Optimization passes
 * - mir_runtime.c: Runtime helper bridges
 * - mir_closure.c: Closure compilation
 * - mir_cache.c: JIT cache management
 */

#ifndef TURBO_SCRIPT_MIR_INTERNAL_H
#define TURBO_SCRIPT_MIR_INTERNAL_H

#include "exprtk.h"
#include "exprtk_class.h"
#include "exprtk_module.h"
#include "turbo_script.h"
#include "../turbo_script_internal.h"
#include "../turbo_script_closure_analysis.h"
#include <mir.h>
#include <mir-gen.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

typedef struct ts_mir_compiler_s ts_mir_compiler_t;
typedef struct ts_mir_externals_s ts_mir_externals_t;
typedef struct ts_mir_var_entry_s ts_mir_var_entry_t;
typedef struct ts_mir_compile_frame_s ts_mir_compile_frame_t;
typedef struct ts_mir_artifact_s ts_mir_artifact_t;
typedef struct ts_host_export_table_s ts_host_export_table_t;

typedef enum ts_mir_host_slot_policy_e {
  TS_MIR_HOST_SLOTS_DISABLED = 0,
  TS_MIR_HOST_SLOTS_FROZEN = 1
} ts_mir_host_slot_policy_t;

typedef struct ts_mir_lowering_policy_s {
  ts_mir_host_slot_policy_t host_slots;
  turbo_script_ctx_t *registry_owner_ctx;
} ts_mir_lowering_policy_t;

/* =========================================================================
 * Constants
 * ========================================================================= */

#define TS_MIR_MAX_LOOP_DEPTH 32
#define TS_MIR_MAX_CAPTURES 10

/* =========================================================================
 * Loop stack for break/continue
 * ========================================================================= */

typedef struct {
  MIR_label_t break_label;
  MIR_label_t continue_label;
} loop_frame_t;

/* =========================================================================
 * External call items cache (Runtime helpers)
 * ========================================================================= */

struct ts_mir_externals_s {
  // Math functions
  MIR_item_t fmod_proto, fmod_import;
  MIR_item_t pow_proto, pow_import;
  MIR_item_t sin_proto, sin_import;
  MIR_item_t cos_proto, cos_import;
  MIR_item_t sqrt_proto, sqrt_import;
  MIR_item_t fabs_proto, fabs_import;
  MIR_item_t floor_proto, floor_import;
  MIR_item_t ceil_proto, ceil_import;
  MIR_item_t log_proto, log_import;
  MIR_item_t exp_proto, exp_import;
  MIR_item_t round_proto, round_import;
  MIR_item_t tan_proto, tan_import;
  MIR_item_t asin_proto, asin_import;
  MIR_item_t acos_proto, acos_import;
  MIR_item_t atan_proto, atan_import;
  MIR_item_t atan2_proto, atan2_import;
  MIR_item_t fmax_proto, fmax_import;
  MIR_item_t fmin_proto, fmin_import;
  
  // Variable bridge
  MIR_item_t load_var_proto, load_var_import;
  MIR_item_t store_var_proto, store_var_import;
  MIR_item_t assign_var_proto, assign_var_import;
  
  // Function call bridges
  MIR_item_t call0_proto, call0_import;
  MIR_item_t call1_proto, call1_import;
  MIR_item_t call2_proto, call2_import;
  MIR_item_t call3_proto, call3_import;
  MIR_item_t calln_proto, calln_import;
  MIR_item_t call_assign_proto, call_assign_import;
  MIR_item_t call_value_assign_proto, call_value_assign_import;
  MIR_item_t value_expr_proto, value_expr_import;
  MIR_item_t value_expr_assign_proto, value_expr_assign_import;
  MIR_item_t await_value_proto, await_value_import;
  MIR_item_t await_assign_proto, await_assign_import;
  MIR_item_t function_expr_assign_proto, function_expr_assign_import;
  MIR_item_t try_catch_assign_proto, try_catch_assign_import;
  
  // Vector indexing
  MIR_item_t vec_get_proto, vec_get_import;
  MIR_item_t vec_assign_proto, vec_assign_import;
  MIR_item_t destruct_var_proto, destruct_var_import;
  MIR_item_t destruct_vector_proto, destruct_vector_import;
  MIR_item_t string_assign_proto, string_assign_import;
  MIR_item_t template_assign_proto, template_assign_import;
  
  // Function definition
  MIR_item_t define_func_proto, define_func_import;
  
  // Member access
  MIR_item_t member_get_proto, member_get_import;
  MIR_item_t member_get_assign_proto, member_get_assign_import;
  
  // Vector data pointer
  MIR_item_t vec_data_proto, vec_data_import;
  
  // Map access by key
  MIR_item_t map_get_key_proto, map_get_key_import;
  MIR_item_t map_assign_proto, map_assign_import;
  MIR_item_t map_value_assign_proto, map_value_assign_import;
  MIR_item_t map_rest_assign_proto, map_rest_assign_import;
  
  // Map field pointer (for native direct load)
  MIR_item_t map_num_ptr_proto, map_num_ptr_import;
  
  // Closure support
  MIR_item_t load_captured_proto, load_captured_import;
  
  // Direct native/builtin function dispatch
  MIR_item_t call_native_proto, call_native_import;
  MIR_item_t call_host_slot_proto, call_host_slot_import;
  MIR_item_t call_builtin_proto, call_builtin_import;
  
  // OOP runtime calls
  MIR_item_t oop_define_class_proto, oop_define_class_import;
  MIR_item_t oop_alias_class_proto, oop_alias_class_import;
  MIR_item_t oop_new_assign_proto, oop_new_assign_import;
  MIR_item_t oop_member_call_proto, oop_member_call_import;
  MIR_item_t oop_member_call_cached_proto, oop_member_call_cached_import;
  MIR_item_t oop_member_call_assign_proto, oop_member_call_assign_import;
  MIR_item_t oop_member_call_value_proto, oop_member_call_value_import;
  MIR_item_t oop_member_call_mono_value_proto, oop_member_call_mono_value_import;
  MIR_item_t oop_member_call_assign_value_proto, oop_member_call_assign_value_import;
  MIR_item_t oop_member_get_proto, oop_member_get_import;
  MIR_item_t oop_member_set_proto, oop_member_set_import;
  MIR_item_t oop_num_ptr_proto, oop_num_ptr_import;
  MIR_item_t oop_instanceof_proto, oop_instanceof_import;
  MIR_item_t oop_predicate_proto, oop_predicate_import;
};

/* =========================================================================
 * Variable entry with dirty tracking
 * ========================================================================= */

struct ts_mir_var_entry_s {
  char *name;           // Variable name
  MIR_reg_t reg;        // MIR register
  int dirty;            // Dirty flag: 1 = needs sync to environment
  int dynamic_value;    // 1 = holds dynamic value (not numeric)
};

/* =========================================================================
 * Function parameter alias for monomorphic higher-order calls
 * ========================================================================= */

typedef struct {
  const char *param_name;
  const char *target_name;
} ts_mir_func_alias_t;

/* =========================================================================
 * Compiled script function
 * ========================================================================= */

typedef struct {
  char *name;
  MIR_item_t mir_func;
  MIR_item_t proto;
  size_t arg_count;
  int has_closure_env;
} ts_compiled_func_t;

/* =========================================================================
 * Cached vector data pointer
 * ========================================================================= */

typedef struct {
  const char *name;      // Vector variable name
  MIR_reg_t ptr_reg;     // Cached pointer register
} ts_mir_vec_ptr_entry_t;

/* =========================================================================
 * Cached map field pointer
 * ========================================================================= */

typedef struct {
  const char *obj_name;  // Map variable name
  const char *key_name;  // Field key
  MIR_reg_t ptr_reg;     // Cached pointer register
} ts_mir_map_ptr_entry_t;

/* =========================================================================
 * OOP slot cache
 * ========================================================================= */

typedef struct {
  exprtk_instance_t *instance;
  size_t index;
} ts_mir_oop_slot_cache_t;

typedef struct {
  const char *obj_name;
  const char *member_name;
  MIR_reg_t ptr_reg;
  void *cache;  // ts_mir_oop_slot_cache_t*
} ts_mir_oop_ptr_entry_t;

/* =========================================================================
 * Class type tracking for monomorphic OOP calls
 * ========================================================================= */

typedef struct {
  char *var_name;
  char *class_name;
} ts_mir_class_type_entry_t;

/* =========================================================================
 * Compile frame (for nested function compilation)
 * ========================================================================= */

struct ts_mir_compile_frame_s {
  MIR_item_t func;
  ts_mir_var_entry_t **vars;
  int var_count;
  int var_capacity;
  int tmp_count;
  int loop_depth;
  MIR_reg_t ctx_reg;
  MIR_reg_t closure_env_reg;
  ts_mir_func_alias_t *func_aliases;
  size_t func_alias_count;
  int vec_ptr_count;
  int map_ptr_count;
  int oop_ptr_count;
  char **class_names;
  int class_name_count;
  int class_name_capacity;
  ts_mir_class_type_entry_t *class_types;
  int class_type_count;
  int class_type_capacity;
};

/* =========================================================================
 * Main compiler context
 * ========================================================================= */

struct ts_mir_compiler_s {
  MIR_context_t ctx;
  MIR_item_t func;
  MIR_module_t module;
  turbo_script_ctx_t *ts_ctx;
  ts_mir_lowering_policy_t lowering_policy;
  exprtk_node_t *ast_root;
  const exprtk_node_t *const *metadata_nodes;
  size_t metadata_node_count;
  char item_prefix[64];

  ts_mir_var_entry_t **vars;
  int var_count;
  int var_capacity;
  int tmp_count;
  int failed;

  // Loop stack for break/continue
  loop_frame_t loop_stack[TS_MIR_MAX_LOOP_DEPTH];
  int loop_depth;

  // External call items
  ts_mir_externals_t ext;

  // ctx_ptr register (first function argument)
  MIR_reg_t ctx_reg;

  // Compile-time function-parameter aliases
  ts_mir_func_alias_t *func_aliases;
  size_t func_alias_count;

  // Compiled script functions
  ts_compiled_func_t *compiled_funcs;
  int compiled_func_count;
  int compiled_func_capacity;

  // Cached vector data pointers
  ts_mir_vec_ptr_entry_t *vec_ptrs;
  int vec_ptr_count;
  int vec_ptr_capacity;

  // Cached map field pointers
  ts_mir_map_ptr_entry_t *map_ptrs;
  int map_ptr_count;
  int map_ptr_capacity;

  // Cached public numeric OOP field pointers
  ts_mir_oop_ptr_entry_t *oop_ptrs;
  int oop_ptr_count;
  int oop_ptr_capacity;

  // Closure support
  void *closure_analysis;     // ts_closure_analysis_t*
  MIR_reg_t closure_env_reg;

  // OOP class names defined in the current script
  char **class_names;
  int class_name_count;
  int class_name_capacity;

  // Exact variable class tracking
  ts_mir_class_type_entry_t *class_types;
  int class_type_count;
  int class_type_capacity;
};

int ts_mir_artifact_compile(turbo_script_ctx_t *compile_ctx, exprtk_node_t *ast,
                            const ts_host_export_table_t *exports,
                            ts_mir_artifact_t **out_artifact);
void ts_mir_artifact_destroy(ts_mir_artifact_t *artifact);
int ts_mir_artifact_execute_numeric(ts_mir_artifact_t *artifact,
                                    turbo_script_ctx_t *runtime_ctx,
                                    size_t export_index, int use_jit,
                                    const double *args, size_t arg_count,
                                    double *out_result);
int ts_mir_artifact_execute_initializer(ts_mir_artifact_t *artifact,
                                        turbo_script_ctx_t *runtime_ctx,
                                        int use_jit);

/* =========================================================================
 * Module: mir_compiler.c - Compiler framework
 * ========================================================================= */

// Initialization and cleanup
void ts_mir_compiler_init(ts_mir_compiler_t *c, turbo_script_ctx_t *ts_ctx, 
                         exprtk_node_t *ast_root);
void ts_mir_compiler_free(ts_mir_compiler_t *c);

// Register allocation
MIR_reg_t ts_mir_new_temp_reg(ts_mir_compiler_t *c);
MIR_reg_t ts_mir_new_temp_ireg(ts_mir_compiler_t *c);
MIR_reg_t ts_mir_new_temp_preg(ts_mir_compiler_t *c);

// Variable management
MIR_reg_t ts_mir_get_or_create_reg(ts_mir_compiler_t *c, const char *name);
void ts_mir_mark_var_dirty(ts_mir_compiler_t *c, const char *name);
void ts_mir_mark_var_clean(ts_mir_compiler_t *c, const char *name);
void ts_mir_mark_all_vars_dirty(ts_mir_compiler_t *c);
void ts_mir_mark_var_dynamic(ts_mir_compiler_t *c, const char *name);
void ts_mir_mark_var_numeric(ts_mir_compiler_t *c, const char *name);
int ts_mir_var_is_dynamic(ts_mir_compiler_t *c, const char *name);
void ts_mir_clear_dirty_flags(ts_mir_compiler_t *c);
void ts_mir_sync_all_dirty_vars(ts_mir_compiler_t *c);
int ts_mir_bind_existing_reg(ts_mir_compiler_t *c, const char *name, MIR_reg_t reg);

// Class tracking
int ts_mir_add_class_name(ts_mir_compiler_t *c, const char *name);
int ts_mir_set_var_class(ts_mir_compiler_t *c, const char *var_name,
                         const char *class_name);
void ts_mir_clear_var_class(ts_mir_compiler_t *c, const char *var_name);
const char *ts_mir_new_hidden_receiver_name(ts_mir_compiler_t *c);
const char *ts_mir_find_var_class(ts_mir_compiler_t *c, const char *var_name);
int ts_mir_is_known_class_name(ts_mir_compiler_t *c, const char *name);

// Frame management
ts_mir_compile_frame_t ts_mir_capture_frame(const ts_mir_compiler_t *c);
void ts_mir_begin_isolated_compile(ts_mir_compiler_t *c);
void ts_mir_restore_frame(ts_mir_compiler_t *c, const ts_mir_compile_frame_t *frame);
void ts_mir_destroy_compiler_storage(ts_mir_compiler_t *c);
void ts_emit_var_prologue(ts_mir_compiler_t *c);
void ts_emit_var_epilogue(ts_mir_compiler_t *c);
void ts_emit_vec_prologue(ts_mir_compiler_t *c);
void ts_emit_map_prologue(ts_mir_compiler_t *c);

// Error reporting
void ts_mir_fail(ts_mir_compiler_t *c, const char *fmt, ...);
double ts_mir_numeric_value(exprtk_value_t val);
void ts_mir_promote_env_error(turbo_script_ctx_t *ctx);
int ts_mir_runtime_define_func_in_env(exprtk_env_t *env, exprtk_node_t *node);

/* =========================================================================
 * Main AST lowering helpers still implemented in turbo_script_mir.c
 * ========================================================================= */

MIR_reg_t ts_emit_packed_args(ts_mir_compiler_t *c, size_t argc, const MIR_reg_t *arg_regs);
MIR_reg_t ts_emit_zero_reg(ts_mir_compiler_t *c);
MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node);

int ts_is_non_numeric_node(exprtk_node_t *node);
int ts_expr_contains_value_call(ts_mir_compiler_t *c, exprtk_node_t *node);
int ts_vector_literal_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node);
int ts_map_literal_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node);
int ts_index_assignment_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node);
MIR_reg_t ts_emit_vector_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                        exprtk_node_t *node);
MIR_reg_t ts_emit_map_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                     exprtk_node_t *node);
MIR_reg_t ts_emit_map_value_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                           exprtk_node_t *node);
MIR_reg_t ts_emit_string_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                        exprtk_node_t *node);
MIR_reg_t ts_emit_template_string_assign(ts_mir_compiler_t *c, const char *target_name,
                                         exprtk_node_t *node);

void ts_mir_clear_dirty_flags(ts_mir_compiler_t *c);
void ts_mir_mark_all_vars_dirty(ts_mir_compiler_t *c);
void ts_emit_sync_var_to_env(ts_mir_compiler_t *c, const char *name);
void ts_emit_sync_to_env(ts_mir_compiler_t *c);
void ts_emit_reload_from_env(ts_mir_compiler_t *c);
MIR_reg_t ts_emit_unsupported_node(ts_mir_compiler_t *c, exprtk_node_t *node);
MIR_reg_t ts_emit_runtime_value_node(ts_mir_compiler_t *c, exprtk_node_t *node);

int ts_find_math_dispatch(ts_mir_compiler_t *c, const char *name, size_t argc,
                          MIR_item_t *proto, MIR_item_t *import);
int ts_call_args_need_ast_eval(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args);
int ts_runtime_call_needs_value_eval(ts_mir_compiler_t *c, const char *name, size_t argc);
int ts_expr_list_contains_value_call(ts_mir_compiler_t *c, size_t count, exprtk_node_t **nodes);
int ts_env_has_func(exprtk_env_t *env, const char *name);
int ts_resolves_core_compat_func(const char *name);
int ts_binary_is_null_eq_compare(exprtk_node_t *node);
int ts_try_fold_constant(exprtk_node_t *node, double *out);
int ts_emit_direct_math_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                              const MIR_reg_t *arg_regs, MIR_reg_t res);
int ts_emit_direct_resolved_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                  const MIR_reg_t *arg_regs, MIR_reg_t res);
void ts_emit_runtime_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                           const MIR_reg_t *arg_regs, MIR_reg_t res);
MIR_reg_t ts_emit_runtime_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                      const char *name, size_t argc,
                                      exprtk_node_t **arg_nodes);

MIR_reg_t ts_mir_get_or_add_vec_ptr(ts_mir_compiler_t *c, const char *name);
MIR_reg_t ts_emit_index_access(ts_mir_compiler_t *c, const char *name, exprtk_node_t *index_node);

MIR_reg_t ts_mir_get_or_add_map_ptr(ts_mir_compiler_t *c, const char *obj_name, const char *key_name);
MIR_reg_t ts_try_emit_oop_member_get_slot(ts_mir_compiler_t *c, const char *object_name,
                                           const char *member_name, exprtk_node_t *object_node);
MIR_reg_t ts_emit_member_access(ts_mir_compiler_t *c, const char *obj_name,
                                 const char *member, exprtk_node_t *object_node);
MIR_reg_t ts_emit_member_access_assign(ts_mir_compiler_t *c, const char *target_name,
                                       const char *obj_name, const char *member,
                                       exprtk_node_t *object_node);

int ts_oop_arg_needs_value_bridge(ts_mir_compiler_t *c, exprtk_node_t *arg);
int ts_oop_call_args_need_value_bridge(ts_mir_compiler_t *c, size_t argc,
                                       exprtk_node_t **args);
void ts_emit_oop_define_class(ts_mir_compiler_t *c, exprtk_node_t *node);
MIR_reg_t ts_emit_oop_class_alias(ts_mir_compiler_t *c, const char *target_name,
                                  const char *source_name);
MIR_reg_t ts_emit_oop_new_assign(ts_mir_compiler_t *c, const char *target_name,
                                 exprtk_node_t *new_node);
MIR_reg_t ts_emit_oop_class_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                        exprtk_node_t *call_node);
const char *ts_emit_oop_receiver_to_temp(ts_mir_compiler_t *c, exprtk_node_t *object_node);
MIR_reg_t ts_emit_oop_predicate(ts_mir_compiler_t *c, int kind, exprtk_node_t *arg);
int ts_try_emit_typeof_oop_compare(ts_mir_compiler_t *c, exprtk_node_t *left,
                                   exprtk_node_t *right, int op, MIR_reg_t *out);
MIR_reg_t ts_emit_oop_member_call_value(ts_mir_compiler_t *c, const char *object_name,
                                        const char *method_name,
                                        exprtk_node_t *object_node,
                                        exprtk_node_t *call_node);
MIR_reg_t ts_emit_oop_member_call_mono_value(ts_mir_compiler_t *c,
                                             const char *object_name,
                                             const char *expected_class_name,
                                             const char *method_name,
                                             exprtk_node_t *object_node,
                                             exprtk_node_t *call_node);
MIR_reg_t ts_emit_oop_member_call_cached(ts_mir_compiler_t *c, const char *object_name,
                                         const char *method_name, exprtk_node_t *object_node,
                                         size_t argc, const MIR_reg_t *arg_regs,
                                         int reload_after);
MIR_reg_t ts_emit_oop_member_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                         const char *object_name, const char *method_name,
                                         exprtk_node_t *object_node, size_t argc,
                                         const MIR_reg_t *arg_regs);
MIR_reg_t ts_emit_oop_member_call_assign_value(ts_mir_compiler_t *c,
                                               const char *target_name,
                                               const char *object_name,
                                               const char *method_name,
                                               exprtk_node_t *object_node,
                                               exprtk_node_t *call_node);
MIR_reg_t ts_emit_oop_member_set(ts_mir_compiler_t *c, const char *object_name,
                                 const char *member_name, exprtk_node_t *object_node,
                                 exprtk_node_t *value_node);
MIR_reg_t ts_try_emit_oop_inline_method_call(ts_mir_compiler_t *c,
                                             const char *object_name,
                                             const char *class_name,
                                             const char *method_name, size_t argc,
                                             const MIR_reg_t *arg_regs);
MIR_reg_t ts_emit_oop_instanceof(ts_mir_compiler_t *c, const char *object_name,
                                 const char *class_name);

// OOP slot pointer cache
MIR_reg_t ts_mir_get_or_add_oop_ptr(ts_mir_compiler_t *c, const char *obj_name,
                                    const char *member_name);

/* =========================================================================
 * Module: mir_closure.c - Closure compilation
 * ========================================================================= */

int ts_mir_compile_closure(ts_mir_compiler_t *c, exprtk_node_t *func_node);

/* =========================================================================
 * Module: mir_runtime.c - Runtime helper bridges
 * ========================================================================= */

// External function setup
void ts_mir_init_externals(ts_mir_compiler_t *c);
void ts_mir_load_externals(MIR_context_t ctx);

// Runtime bridge functions (implemented in turbo_script_mir.c)
// These are the actual C functions that MIR external calls invoke at runtime
double ts_mir_load_var(void *ctx_ptr, const char *name);
void ts_mir_store_var(void *ctx_ptr, const char *name, double value);
double ts_mir_assign_var(void *ctx_ptr, const char *target, const char *source);
double ts_mir_call0(void *ctx_ptr, const char *name);
double ts_mir_call1(void *ctx_ptr, const char *name, double a1);
double ts_mir_call2(void *ctx_ptr, const char *name, double a1, double a2);
double ts_mir_call3(void *ctx_ptr, const char *name, double a1, double a2, double a3);
double ts_mir_calln(void *ctx_ptr, const char *name, int64_t count, double *args);
double ts_mir_call_assign(void *ctx_ptr, const char *target, const char *name,
                         int64_t count, double *args);
double ts_mir_call_value_assign(void *ctx_ptr, const char *target, const char *name,
                                 void *call_node);
double ts_mir_value_expr(void *ctx_ptr, void *node);
double ts_mir_value_expr_assign(void *ctx_ptr, const char *target, void *node);
double ts_mir_await_value(void *ctx_ptr, void *arg_node);
double ts_mir_await_assign(void *ctx_ptr, const char *target, void *arg_node);
double ts_mir_function_expr_assign(void *ctx_ptr, const char *target, void *node);
double ts_mir_try_catch_assign(void *ctx_ptr, const char *target, void *node);
double ts_mir_vec_get(void *ctx_ptr, const char *name, double index);
double ts_mir_vector_assign(void *ctx_ptr, const char *target, int64_t count, double *values);
double ts_mir_destructure_var(void *ctx_ptr, void *target, const char *value_name,
                              int64_t is_constant);
double ts_mir_destructure_vector(void *ctx_ptr, void *target, int64_t is_constant,
                                 int64_t count, double *values);
double ts_mir_string_assign(void *ctx_ptr, const char *target, const char *data, int64_t len);
double ts_mir_template_assign(void *ctx_ptr, const char *target, void *node);
void ts_mir_define_func(void *ctx_ptr, void *node_ptr);
double ts_mir_member_get(void *ctx_ptr, const char *obj_name, const char *member,
                         void *cache_ptr, void *object_node);
double ts_mir_member_get_assign(void *ctx_ptr, const char *target_name, const char *obj_name,
                                const char *member, void *cache_ptr, void *object_node);
void *ts_mir_vec_data(void *ctx_ptr, const char *name);
double ts_mir_map_get_key(void *ctx_ptr, const char *obj_name, const char *key);
double ts_mir_map_assign(void *ctx_ptr, const char *target, void *node, int64_t count,
                         double *values);
double ts_mir_map_value_assign(void *ctx_ptr, const char *target, void *node);
double ts_mir_map_rest_assign(void *ctx_ptr, const char *target, const char *rest_src,
                              int64_t count, const char **keys);
void *ts_mir_map_num_ptr(void *ctx_ptr, const char *obj_name, const char *key);
double ts_mir_load_captured_var(void *ctx_ptr, const char *var_name, void *closure_env_ptr);
double ts_mir_call_native(void *ctx_ptr, void *fn_ptr, void *user_data, int64_t argc, double *argv);
double ts_mir_call_host_slot(void *registry_owner_ptr, void *runtime_ctx_ptr,
                             int64_t slot, int64_t argc, double *argv);
double ts_mir_call_builtin(void *ctx_ptr, void *fn_ptr, int64_t argc, double *argv);
void ts_mir_oop_define_class(void *ctx_ptr, void *node);
double ts_mir_oop_class_alias(void *ctx_ptr, const char *alias, const char *class_name);
double ts_mir_oop_new_assign(void *ctx_ptr, const char *target, const char *class_name,
                             int64_t argc, double *argv);
double ts_mir_oop_member_call(void *ctx_ptr, const char *obj, const char *method,
                              void *object_node, int64_t argc, double *argv);
double ts_mir_oop_member_call_cached(void *ctx_ptr, const char *obj, const char *method,
                                     void *cache, void *object_node, int64_t argc,
                                     double *argv);
double ts_mir_oop_member_call_assign(void *ctx_ptr, const char *target, const char *obj,
                                     const char *method, void *cache_ptr, void *object_node,
                                     int64_t argc, double *argv);
double ts_mir_oop_member_call_value(void *ctx_ptr, const char *obj,
                                    const char *method, void *object_node, void *call_node);
double ts_mir_oop_member_call_mono_value(void *ctx_ptr, const char *obj,
                                         const char *class_name, const char *method,
                                         void *object_node, void *call_node);
double ts_mir_oop_member_call_assign_value(void *ctx_ptr, const char *target,
                                           const char *obj, const char *method,
                                           void *object_node, void *call_node);
double ts_mir_oop_member_get(void *ctx_ptr, const char *obj, const char *field,
                             void *cache_ptr, void *object_node);
double ts_mir_oop_member_set(void *ctx_ptr, const char *obj, const char *field,
                             void *cache_ptr, void *object_node, double value);
void *ts_mir_oop_num_ptr(void *ctx_ptr, const char *obj, const char *field,
                         void *object_node, int64_t create_if_missing, void *slot_cache_ptr);
double ts_mir_oop_instanceof(void *ctx_ptr, const char *obj, const char *class_name);
double ts_mir_oop_predicate(void *ctx_ptr, int64_t kind, const char *name, const char *member,
                            void *object_node);

/* =========================================================================
 * Function compilation helpers
 * ========================================================================= */

size_t ts_compile_call_args(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args,
                            MIR_reg_t out_regs[16]);
void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node);
MIR_reg_t ts_emit_compiled_func_call(ts_mir_compiler_t *c, ts_compiled_func_t *cf,
                                     size_t argc, const MIR_reg_t *arg_regs);
MIR_reg_t ts_try_emit_hof_specialized_call(ts_mir_compiler_t *c,
                                           exprtk_node_t *call_node);
ts_compiled_func_t *ts_find_compiled_func(ts_mir_compiler_t *c, const char *name);
const char *ts_find_func_alias(ts_mir_compiler_t *c, const char *name);
int ts_mir_reserve_compiled_func(ts_mir_compiler_t *c);
int ts_function_call_is_known_class(ts_mir_compiler_t *c, const char *name);
void ts_compile_script_func(ts_mir_compiler_t *c, const char *name,
                            exprtk_node_t **arg_params, size_t arg_count,
                            exprtk_node_t *body);
void ts_prescan_variables(ts_mir_compiler_t *c, exprtk_node_t *node);
void ts_prescan_class_names(ts_mir_compiler_t *c, exprtk_node_t *node);
void ts_prescan_functions(ts_mir_compiler_t *c, exprtk_node_t *node);
void ts_prescan_hof_specializations(ts_mir_compiler_t *c, exprtk_node_t *node);
int ts_call_assignment_needs_value_bridge(ts_mir_compiler_t *c, const char *name, size_t argc);
MIR_reg_t ts_emit_call_value_assign(ts_mir_compiler_t *c, const char *target_name,
                                    const char *name, exprtk_node_t *call_node);
MIR_reg_t ts_emit_value_expr_assign(ts_mir_compiler_t *c, const char *target_name,
                                    exprtk_node_t *expr_node);
MIR_reg_t ts_emit_await_value(ts_mir_compiler_t *c, exprtk_node_t *arg_node);
MIR_reg_t ts_emit_await_assign(ts_mir_compiler_t *c, const char *target_name,
                               exprtk_node_t *arg_node);
MIR_reg_t ts_emit_function_expr_assign(ts_mir_compiler_t *c, const char *target_name,
                                       exprtk_node_t *node);
MIR_reg_t ts_emit_try_catch_assign(ts_mir_compiler_t *c, const char *target_name,
                                   exprtk_node_t *node);
MIR_reg_t ts_emit_dynamic_var_assign(ts_mir_compiler_t *c, const char *target_name,
                                     const char *source_name);

/* =========================================================================
 * Module: mir_cache.c - JIT cache management
 * ========================================================================= */

// Hash computation
uint64_t ts_mir_compute_hash(const char *script);

// Cache operations
void *ts_mir_cache_lookup(turbo_script_ctx_t *ctx, const char *script);
void ts_mir_cache_insert(turbo_script_ctx_t *ctx, const char *script, void *fn_ptr);

// Cache management (optional/future use)
int ts_mir_cache_find_lru_slot(turbo_script_ctx_t *ctx);
void ts_mir_cache_clear(turbo_script_ctx_t *ctx);

/* =========================================================================
 * Module: mir_oop.c - OOP helpers and runtime bridges
 * ========================================================================= */

// Pure utility functions (no dependencies)
int ts_oop_predicate_kind(const char *name);
int ts_oop_predicate_can_preserve_value(int kind, exprtk_node_t *arg);
int ts_typeof_oop_kind(exprtk_node_t *node);
int ts_typeof_arg(exprtk_node_t *node, exprtk_node_t **out_arg);
int ts_oop_member_call_can_skip_reload(ts_mir_compiler_t *c, const char *class_name,
                                       const char *method_name, size_t argc);
exprtk_node_t *ts_find_top_level_class_def(ts_mir_compiler_t *c, const char *class_name);
int ts_top_level_class_name_is_assigned(ts_mir_compiler_t *c, const char *class_name);
int ts_inline_param_index(exprtk_node_t **params, size_t param_count, const char *name,
                          size_t *out_index);
const char *ts_direct_parent_class_name(ts_mir_compiler_t *c, const char *class_name);
int ts_oop_member_can_use_num_slot(ts_mir_compiler_t *c, const char *object_name,
                                   const char *member_name);
exprtk_node_t *ts_find_inlineable_instance_method(ts_mir_compiler_t *c,
                                                  const char *class_name,
                                                  const char *method_name,
                                                  size_t argc);

#ifdef __cplusplus
}
#endif

#endif // TURBO_SCRIPT_MIR_INTERNAL_H
