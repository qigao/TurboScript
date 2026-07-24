#include "exprtk.h"
#include "exprtk_class.h"
#include "exprtk_module.h"
#include "turbo_script.h"
#include "turbo_script_internal.h"
#include "turbo_script_closure_analysis.h"
#include "mir/turbo_script_mir_internal.h"

#include "exprtk_grammar.h"
#include <math.h>
#include <mir-gen.h>
#include <mir.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

exprtk_env_t *exprtk_env_snapshot(exprtk_env_t *env);
exprtk_value_t throw_error(exprtk_env_t *env, const exprtk_node_t *node, const char *fmt, ...);
exprtk_value_t throw_method_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                         const char *method_name, exprtk_func_t *method);
exprtk_value_t throw_field_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                        const char *field_name, int access_level);
const char *type_name(int type);
int values_match(exprtk_value_t lhs, exprtk_value_t rhs);
int exprtk_datetime_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_date_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_time_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_duration_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_decimal_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
exprtk_class_t *eval_current_class(exprtk_env_t *env);
exprtk_func_t *eval_find_constructor_typed(exprtk_class_t *klass, size_t argc,
                                           exprtk_value_t *args);
int can_access_method(exprtk_env_t *env, exprtk_func_t *method, const exprtk_node_t *object_node);
int can_access_declared_field(exprtk_env_t *env, exprtk_class_t *owner_class, int access_level,
                              const exprtk_node_t *object_node, const char *field_name);
exprtk_value_t eval_script_function(exprtk_func_t *func, size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *parent_env, exprtk_env_t *caller_env);
exprtk_value_t eval_class_def_node(const exprtk_node_t *node, exprtk_env_t *env);
exprtk_value_t eval_class_instantiation(const char *class_name, exprtk_node_t **arg_nodes,
                                        size_t arg_count, exprtk_env_t *env);

/* =========================================================================
 * Phase 1-5: Extended MIR JIT Compiler for TurboScript
 * NOTE: Type definitions moved to mir/turbo_script_mir_internal.h
 * ========================================================================= */

/* Forward declarations */
void ts_mir_promote_env_error(turbo_script_ctx_t *ctx);

MIR_reg_t ts_emit_zero_reg(ts_mir_compiler_t *c) {
  MIR_reg_t r = ts_mir_new_temp_reg(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r), MIR_new_double_op(c->ctx, 0.0)));
  return r;
}

/* Forward declarations */
MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node);
void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node);
static void ts_compile_branch_false(ts_mir_compiler_t *c, exprtk_node_t *node,
                                    MIR_label_t false_label);
static void ts_compile_branch_true(ts_mir_compiler_t *c, exprtk_node_t *node,
                                   MIR_label_t true_label);
MIR_reg_t ts_emit_member_access_assign(ts_mir_compiler_t *c, const char *target_name,
                                       const char *obj_name, const char *member,
                                       exprtk_node_t *object_node);
void ts_emit_sync_var_to_env(ts_mir_compiler_t *c, const char *name);
void ts_emit_sync_to_env(ts_mir_compiler_t *c);
void ts_emit_reload_from_env(ts_mir_compiler_t *c);
MIR_reg_t ts_emit_unsupported_node(ts_mir_compiler_t *c, exprtk_node_t *node);
MIR_reg_t ts_emit_runtime_value_node(ts_mir_compiler_t *c, exprtk_node_t *node);

typedef struct {
  const char *name;
  size_t proto_offset;
  size_t import_offset;
} ts_math_dispatch_entry_t;

static int ts_math_dispatch_cmp(const void *key, const void *entry) {
  return strcmp((const char *)key, ((const ts_math_dispatch_entry_t *)entry)->name);
}

int ts_find_math_dispatch(ts_mir_compiler_t *c, const char *name, size_t argc,
                                 MIR_item_t *proto, MIR_item_t *import) {
  static const ts_math_dispatch_entry_t unary_table[] = {
      {"abs", offsetof(ts_mir_externals_t, fabs_proto), offsetof(ts_mir_externals_t, fabs_import)},
      {"acos", offsetof(ts_mir_externals_t, acos_proto),
       offsetof(ts_mir_externals_t, acos_import)},
      {"asin", offsetof(ts_mir_externals_t, asin_proto),
       offsetof(ts_mir_externals_t, asin_import)},
      {"atan", offsetof(ts_mir_externals_t, atan_proto),
       offsetof(ts_mir_externals_t, atan_import)},
      {"ceil", offsetof(ts_mir_externals_t, ceil_proto),
       offsetof(ts_mir_externals_t, ceil_import)},
      {"cos", offsetof(ts_mir_externals_t, cos_proto), offsetof(ts_mir_externals_t, cos_import)},
      {"exp", offsetof(ts_mir_externals_t, exp_proto), offsetof(ts_mir_externals_t, exp_import)},
      {"floor", offsetof(ts_mir_externals_t, floor_proto),
       offsetof(ts_mir_externals_t, floor_import)},
      {"log", offsetof(ts_mir_externals_t, log_proto), offsetof(ts_mir_externals_t, log_import)},
      {"round", offsetof(ts_mir_externals_t, round_proto),
       offsetof(ts_mir_externals_t, round_import)},
      {"sin", offsetof(ts_mir_externals_t, sin_proto), offsetof(ts_mir_externals_t, sin_import)},
      {"sqrt", offsetof(ts_mir_externals_t, sqrt_proto),
       offsetof(ts_mir_externals_t, sqrt_import)},
      {"tan", offsetof(ts_mir_externals_t, tan_proto), offsetof(ts_mir_externals_t, tan_import)},
  };
  static const ts_math_dispatch_entry_t binary_table[] = {
      {"atan2", offsetof(ts_mir_externals_t, atan2_proto),
       offsetof(ts_mir_externals_t, atan2_import)},
      {"max", offsetof(ts_mir_externals_t, fmax_proto),
       offsetof(ts_mir_externals_t, fmax_import)},
      {"min", offsetof(ts_mir_externals_t, fmin_proto),
       offsetof(ts_mir_externals_t, fmin_import)},
  };

  const ts_math_dispatch_entry_t *table = NULL;
  size_t table_count = 0;
  const ts_math_dispatch_entry_t *entry = NULL;
  char *ext_base = NULL;

  if (!c || !name || !proto || !import)
    return 0;

  if (argc == 1) {
    table = unary_table;
    table_count = sizeof(unary_table) / sizeof(unary_table[0]);
  } else if (argc == 2) {
    table = binary_table;
    table_count = sizeof(binary_table) / sizeof(binary_table[0]);
  } else {
    return 0;
  }

  entry = (const ts_math_dispatch_entry_t *)bsearch(name, table, table_count, sizeof(table[0]),
                                                    ts_math_dispatch_cmp);
  if (!entry)
    return 0;

  ext_base = (char *)&c->ext;
  *proto = *(MIR_item_t *)(ext_base + entry->proto_offset);
  *import = *(MIR_item_t *)(ext_base + entry->import_offset);
  return 1;
}
static ts_compiled_func_t *ts_find_hof_specialized_call(ts_mir_compiler_t *c,
                                                        exprtk_node_t *call_node,
                                                        int numeric_arg_indices[16],
                                                        size_t *numeric_arg_count);
static MIR_reg_t ts_emit_assignment(ts_mir_compiler_t *c, exprtk_node_t *node);
static int ts_compile_data_access_and_assignment(ts_mir_compiler_t *c, exprtk_node_t *node,
                                                 MIR_reg_t *out);
int ts_index_assignment_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node);

/* --- Dispatch helpers: function calls (Phase 13/17 + explicit runtime calls) --- */
MIR_reg_t ts_emit_packed_args(ts_mir_compiler_t *c, size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t arr_reg = ts_mir_new_temp_preg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_ALLOCA, MIR_new_reg_op(c->ctx, arr_reg),
                               MIR_new_int_op(c->ctx, (int64_t)(argc * sizeof(double)))));
  for (size_t i = 0; i < argc; i++) {
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV,
                     MIR_new_mem_op(c->ctx, MIR_T_D, (int64_t)(i * sizeof(double)), arr_reg, 0, 1),
                     MIR_new_reg_op(c->ctx, arg_regs[i])));
  }
  return arr_reg;
}

int ts_emit_direct_math_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                    const MIR_reg_t *arg_regs, MIR_reg_t res) {
  MIR_item_t proto = NULL, import = NULL;

  if (argc == 1) {
    if (!ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
    if (!proto) return 0;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 4, MIR_new_ref_op(c->ctx, proto),
                                      MIR_new_ref_op(c->ctx, import), MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, arg_regs[0])));
    return 1;
  }

  if (argc == 2) {
    if (!ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
    if (!proto) return 0;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, proto),
                                      MIR_new_ref_op(c->ctx, import), MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, arg_regs[0]),
                                      MIR_new_reg_op(c->ctx, arg_regs[1])));
    return 1;
  }

  return 0;
}

int ts_emit_direct_resolved_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                        const MIR_reg_t *arg_regs, MIR_reg_t res) {
  /* 1) env-registered native functions */
  exprtk_func_t *f = c->ts_ctx->env.funcs;
  while (f) {
    if (f->name && strcmp(f->name, name) == 0 && !f->is_script) {
      void *fn_ptr = (void *)f->data.native.fn;
      void *ud = f->data.native.user_data;
      MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(
                          c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call_native_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call_native_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)fn_ptr),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ud),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
      return 1;
    }
    f = f->next;
  }

  /* 2) module/registry builtins */
  exprtk_builtin_fn bfn = exprtk_find_builtin(name, &c->ts_ctx->env);
  if (bfn) {
    MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.call_builtin_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call_builtin_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)bfn),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
    return 1;
  }

  return 0;
}

void ts_emit_runtime_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                 const MIR_reg_t *arg_regs, MIR_reg_t res) {
  // 保守策略：函数调用可能修改任何变量，标记所有变量为脏
  ts_mir_mark_all_vars_dirty(c);
  
  if (argc == 0) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.call0_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.call0_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name)));
  } else if (argc == 1) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.call1_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.call1_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                      MIR_new_reg_op(c->ctx, arg_regs[0])));
  } else if (argc == 2) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(
                        c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.call2_proto),
                        MIR_new_ref_op(c->ctx, c->ext.call2_import), MIR_new_reg_op(c->ctx, res),
                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_reg_op(c->ctx, arg_regs[0]), MIR_new_reg_op(c->ctx, arg_regs[1])));
  } else if (argc == 3) {
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call3_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call3_import), MIR_new_reg_op(c->ctx, res),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                          MIR_new_reg_op(c->ctx, arg_regs[0]), MIR_new_reg_op(c->ctx, arg_regs[1]),
                          MIR_new_reg_op(c->ctx, arg_regs[2])));
  } else {
    MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.calln_proto),
                          MIR_new_ref_op(c->ctx, c->ext.calln_import), MIR_new_reg_op(c->ctx, res),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  }
}

size_t ts_compile_call_args(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args,
                                   MIR_reg_t out_regs[16]) {
  if (argc > 16) argc = 16;
  for (size_t i = 0; i < argc; i++) {
    out_regs[i] = ts_compile_expr(c, args[i]);
  }
  return argc;
}

int ts_is_non_numeric_node(exprtk_node_t *node) {
  if (!node) return 0;
  return node->type == EXPRTK_NODE_VECTOR || node->type == EXPRTK_NODE_MAP_LITERAL ||
         node->type == EXPRTK_NODE_STRING || node->type == EXPRTK_NODE_TEMPLATE_STRING ||
         node->type == EXPRTK_NODE_SLICE || node->type == EXPRTK_NODE_FUNCTION_EXPRESSION;
}

int ts_expr_contains_value_call(ts_mir_compiler_t *c, exprtk_node_t *node);

static int ts_member_call_is_stream_chain(exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_MEMBER_CALL) return 0;
  if (node->data.member_call.method &&
      strcmp(node->data.member_call.method, "stream") == 0) {
    return 1;
  }
  if (node->data.member_call.object &&
      node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
      node->data.member_call.object->data.variable.name &&
      strcmp(node->data.member_call.object->data.variable.name, "stream") == 0 &&
      node->data.member_call.method &&
      (strcmp(node->data.member_call.method, "of") == 0 ||
       strcmp(node->data.member_call.method, "file") == 0 ||
       strcmp(node->data.member_call.method, "lines") == 0 ||
       strcmp(node->data.member_call.method, "text") == 0 ||
       strcmp(node->data.member_call.method, "json") == 0 ||
       strcmp(node->data.member_call.method, "xml") == 0 ||
       strcmp(node->data.member_call.method, "csv") == 0)) {
    return 1;
  }
  return ts_member_call_is_stream_chain(node->data.member_call.object);
}

static int ts_call_has_spread_args(size_t argc, exprtk_node_t **args) {
  if (!args) return 0;
  for (size_t i = 0; i < argc; ++i) {
    if (args[i] && args[i]->type == EXPRTK_NODE_SPREAD) return 1;
  }
  return 0;
}

int ts_call_args_need_ast_eval(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args) {
  if (!args) return 0;
  for (size_t i = 0; i < argc; ++i) {
    exprtk_node_t *arg = args[i];
    if (!arg) continue;
    if (arg->type == EXPRTK_NODE_SPREAD || ts_is_non_numeric_node(arg) ||
        ts_oop_arg_needs_value_bridge(c, arg) || ts_expr_contains_value_call(c, arg)) {
      return 1;
    }
  }
  return 0;
}

static int ts_name_starts_with(const char *name, const char *prefix) {
  size_t prefix_len = 0;
  if (!name || !prefix) return 0;
  prefix_len = strlen(prefix);
  return strncmp(name, prefix, prefix_len) == 0;
}

static int ts_call_name_needs_value_eval(const char *name) {
  if (!name) return 0;
  return ts_name_starts_with(name, "mat_") ||
         ts_name_starts_with(name, "matrix.") ||
         ts_name_starts_with(name, "linalg.") ||
         ts_name_starts_with(name, "table.") ||
         ts_name_starts_with(name, "http.") ||
         ts_name_starts_with(name, "json.") ||
         ts_name_starts_with(name, "xml.") ||
         ts_name_starts_with(name, "csv.") ||
         ts_name_starts_with(name, "os.") ||
         ts_name_starts_with(name, "map.") ||
         ts_name_starts_with(name, "parser.json_") ||
         ts_name_starts_with(name, "parser.xml_") ||
         ts_name_starts_with(name, "parser.csv_") ||
         ts_name_starts_with(name, "vec.") ||
         ts_name_starts_with(name, "vector_") ||
         ts_name_starts_with(name, "str_") ||
         ts_name_starts_with(name, "string.") ||
         strcmp(name, "stats.t_test_1samp") == 0;
}

int ts_env_has_func(exprtk_env_t *env, const char *name) {
  for (exprtk_env_t *curr_env = env; curr_env; curr_env = curr_env->parent) {
    for (exprtk_func_t *f = curr_env->funcs; f; f = f->next) {
      if (f->name && strcmp(f->name, name) == 0) return 1;
    }
  }
  return 0;
}

static int ts_is_core_compat_namespace(const char *name, size_t len) {
  static const char *const names[] = {"math", "stats", "string", "io", "core", "regex", NULL};
  for (size_t i = 0; names[i]; ++i) {
    if (strlen(names[i]) == len && strncmp(names[i], name, len) == 0) return 1;
  }
  return 0;
}

int ts_resolves_core_compat_func(const char *name) {
  const char *dot = name ? strchr(name, '.') : NULL;
  if (!dot || !ts_is_core_compat_namespace(name, (size_t)(dot - name))) return 0;
  return exprtk_registry_find(dot + 1) != NULL;
}

static int ts_resolves_runtime_func(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  return ts_env_has_func(&c->ts_ctx->env, name) ||
         exprtk_find_builtin(name, &c->ts_ctx->env) != NULL ||
         ts_resolves_core_compat_func(name);
}

int ts_runtime_call_needs_value_eval(ts_mir_compiler_t *c, const char *name,
                                            size_t argc) {
  MIR_item_t proto = NULL;
  MIR_item_t import = NULL;
  if (!c || !name) return 0;
  if (ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
  return ts_call_name_needs_value_eval(name) || ts_resolves_runtime_func(c, name);
}

int ts_expr_list_contains_value_call(ts_mir_compiler_t *c, size_t count,
                                            exprtk_node_t **nodes);

static int ts_index_constant(exprtk_node_t *node, int *out) {
  if (!node || !out) return 0;
  if (node->type == EXPRTK_NODE_INTEGER) {
    *out = (int)node->data.integer;
    return 1;
  }
  if (node->type == EXPRTK_NODE_NUMBER) {
    *out = (int)node->data.number;
    return node->data.number == (double)*out;
  }
  return 0;
}

int ts_expr_contains_value_call(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    return ts_mir_var_is_dynamic(c, node->data.variable.name);

  case EXPRTK_NODE_FUNCTION_CALL:
    if (node->data.function.name && strcmp(node->data.function.name, "await") == 0) {
      return 0;
    }
    if (ts_runtime_call_needs_value_eval(c, node->data.function.name,
                                         node->data.function.arg_count)) {
      return 1;
    }
    if (ts_call_name_needs_value_eval(node->data.function.name)) return 1;
    return ts_call_args_need_ast_eval(c, node->data.function.arg_count,
                                      node->data.function.args) ||
           ts_expr_list_contains_value_call(c, node->data.function.arg_count,
                                            node->data.function.args);

  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_member_call_is_stream_chain(node)) return 1;
    if (node->data.member_call.object &&
        node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        ts_mir_var_is_dynamic(c, node->data.member_call.object->data.variable.name)) {
      return 1;
    }
    if (node->data.member_call.object &&
        node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_call.object->data.variable.name &&
        node->data.member_call.method) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               node->data.member_call.object->data.variable.name,
               node->data.member_call.method);
      if (ts_resolves_runtime_func(c, full_name)) return 1;
      if (ts_call_name_needs_value_eval(full_name)) return 1;
    }
    return ts_expr_contains_value_call(c, node->data.member_call.object) ||
           ts_call_args_need_ast_eval(c, node->data.member_call.arg_count,
                                      node->data.member_call.args) ||
           ts_expr_list_contains_value_call(c, node->data.member_call.arg_count,
                                            node->data.member_call.args);

  case EXPRTK_NODE_BINARY_OP:
    if (ts_binary_is_null_eq_compare(node)) return 1;
    return ts_expr_contains_value_call(c, node->data.binary.left) ||
           ts_expr_contains_value_call(c, node->data.binary.right);

  case EXPRTK_NODE_INDEX:
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VARIABLE) {
      if (ts_mir_var_is_dynamic(c, node->data.index_access.array->data.variable.name)) {
        return 1;
      }
      return ts_expr_contains_value_call(c, node->data.index_access.index);
    }
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VECTOR) {
      int idx = 0;
      if (!ts_index_constant(node->data.index_access.index, &idx)) return 1;
      if (idx < 0 || (size_t)idx >= node->data.index_access.array->data.vector.count) return 1;
      return ts_expr_contains_value_call(
          c, node->data.index_access.array->data.vector.elements[idx]);
    }
    return 1;

  case EXPRTK_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object &&
        node->data.member_access.object->type == EXPRTK_NODE_VARIABLE &&
        ts_mir_var_is_dynamic(c, node->data.member_access.object->data.variable.name)) {
      return 1;
    }
    return ts_expr_contains_value_call(c, node->data.member_access.object);

  case EXPRTK_NODE_IF:
    return ts_expr_contains_value_call(c, node->data.if_stmt.condition) ||
           ts_expr_contains_value_call(c, node->data.if_stmt.if_branch) ||
           ts_expr_contains_value_call(c, node->data.if_stmt.else_branch);

  default:
    return 0;
  }
}

int ts_expr_list_contains_value_call(ts_mir_compiler_t *c, size_t count,
                                            exprtk_node_t **nodes) {
  if (!nodes) return 0;
  for (size_t i = 0; i < count; ++i) {
    if (ts_expr_contains_value_call(c, nodes[i])) return 1;
  }
  return 0;
}

int ts_index_assignment_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_value_t existing;
  if (!c || !node || node->type != EXPRTK_NODE_INDEX) return 0;

  if (node->data.index_access.array &&
      node->data.index_access.array->type == EXPRTK_NODE_VECTOR) {
    int idx = 0;
    if (!ts_index_constant(node->data.index_access.index, &idx)) return 1;
    if (idx < 0 || (size_t)idx >= node->data.index_access.array->data.vector.count) return 1;
    return ts_expr_contains_value_call(c,
        node->data.index_access.array->data.vector.elements[idx]);
  }

  if (!node->data.index_access.array ||
      node->data.index_access.array->type != EXPRTK_NODE_VARIABLE) {
    return 1;
  }
  if (ts_mir_var_is_dynamic(c, node->data.index_access.array->data.variable.name)) {
    return 1;
  }

  existing = exprtk_env_get(&c->ts_ctx->env,
                            node->data.index_access.array->data.variable.name);
  return existing.type != EXPRTK_VAL_VECTOR;
}

static exprtk_node_t *ts_find_map_literal_member(exprtk_node_t *map_node, const char *member) {
  if (!map_node || map_node->type != EXPRTK_NODE_MAP_LITERAL || !member) return NULL;

  for (size_t i = 0; i < map_node->data.map_literal.count; ++i) {
    if (map_node->data.map_literal.keys[i] &&
        strcmp(map_node->data.map_literal.keys[i], member) == 0) {
      return map_node->data.map_literal.values[i];
    }
  }
  return NULL;
}

static void ts_register_destructure_targets(ts_mir_compiler_t *c, exprtk_node_t *target) {
  if (!c || !target) return;

  switch (target->type) {
  case EXPRTK_NODE_VARIABLE:
    (void)ts_mir_get_or_create_reg(c, target->data.variable.name);
    break;

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < target->data.vector.count; ++i) {
      ts_register_destructure_targets(c, target->data.vector.elements[i]);
    }
    break;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < target->data.map_literal.count; ++i) {
      ts_register_destructure_targets(c, target->data.map_literal.values[i]);
    }
    break;

  case EXPRTK_NODE_SPREAD:
    ts_register_destructure_targets(c, target->data.spread.child);
    break;

  case EXPRTK_NODE_ASSIGNMENT:
    (void)ts_mir_get_or_create_reg(c, target->data.assignment.name);
    break;

  default:
    break;
  }
}

static void ts_mark_destructure_targets_dynamic(ts_mir_compiler_t *c, exprtk_node_t *target) {
  if (!c || !target) return;

  switch (target->type) {
  case EXPRTK_NODE_VARIABLE:
    ts_mir_mark_var_dynamic(c, target->data.variable.name);
    ts_mir_mark_var_clean(c, target->data.variable.name);
    break;

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < target->data.vector.count; ++i) {
      ts_mark_destructure_targets_dynamic(c, target->data.vector.elements[i]);
    }
    break;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < target->data.map_literal.count; ++i) {
      ts_mark_destructure_targets_dynamic(c, target->data.map_literal.values[i]);
    }
    break;

  case EXPRTK_NODE_SPREAD:
    ts_mark_destructure_targets_dynamic(c, target->data.spread.child);
    break;

  case EXPRTK_NODE_ASSIGNMENT:
    ts_mir_mark_var_dynamic(c, target->data.assignment.name);
    ts_mir_mark_var_clean(c, target->data.assignment.name);
    break;

  default:
    break;
  }
}

static MIR_reg_t ts_emit_destructure_assignment(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_node_t *value;
  MIR_reg_t res;

  if (!c || !node || node->type != EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT ||
      !node->data.destructuring.targets || !node->data.destructuring.value) {
    return 0;
  }

  value = node->data.destructuring.value;
  ts_register_destructure_targets(c, node->data.destructuring.targets);
  res = ts_mir_new_temp_reg(c);

  if (value->type == EXPRTK_NODE_VARIABLE && value->data.variable.name) {
    ts_emit_sync_to_env(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.destruct_var_proto),
                          MIR_new_ref_op(c->ctx, c->ext.destruct_var_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node->data.destructuring.targets),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)value->data.variable.name),
                          MIR_new_int_op(c->ctx, (int64_t)node->data.destructuring.is_constant)));
    ts_emit_reload_from_env(c);
    ts_mark_destructure_targets_dynamic(c, node->data.destructuring.targets);
    return res;
  }

  if (value->type == EXPRTK_NODE_VECTOR && value->data.vector.count <= 16 &&
      !ts_vector_literal_needs_value_eval(c, value)) {
    MIR_reg_t element_regs[16];
    MIR_reg_t arr_reg;
    size_t count = value->data.vector.count;

    for (size_t i = 0; i < count; ++i) {
      element_regs[i] = ts_compile_expr(c, value->data.vector.elements[i]);
    }
    arr_reg = ts_emit_packed_args(c, count, element_regs);

    ts_emit_sync_to_env(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.destruct_vector_proto),
                          MIR_new_ref_op(c->ctx, c->ext.destruct_vector_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node->data.destructuring.targets),
                          MIR_new_int_op(c->ctx, (int64_t)node->data.destructuring.is_constant),
                          MIR_new_int_op(c->ctx, (int64_t)count), MIR_new_reg_op(c->ctx, arr_reg)));
    ts_emit_reload_from_env(c);
    ts_mark_destructure_targets_dynamic(c, node->data.destructuring.targets);
    return res;
  }

  return 0;
}

static void ts_register_helper_written_vars(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    ts_register_helper_written_vars(c, node->data.assignment.value);
    break;

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    ts_register_destructure_targets(c, node->data.destructuring.targets);
    ts_register_helper_written_vars(c, node->data.destructuring.value);
    break;

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      ts_register_helper_written_vars(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_IF:
    ts_register_helper_written_vars(c, node->data.if_stmt.if_branch);
    ts_register_helper_written_vars(c, node->data.if_stmt.else_branch);
    break;

  case EXPRTK_NODE_TRY_CATCH:
    ts_register_helper_written_vars(c, node->data.try_catch.try_body);
    ts_register_helper_written_vars(c, node->data.try_catch.catch_body);
    if (node->data.try_catch.catch_var) {
      (void)ts_mir_get_or_create_reg(c, node->data.try_catch.catch_var);
    }
    break;

  default:
    break;
  }
}

void ts_prescan_variables(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    if (node->data.variable.name) (void)ts_mir_get_or_create_reg(c, node->data.variable.name);
    break;

  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    if (node->data.assignment.name) (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    ts_prescan_variables(c, node->data.assignment.value);
    break;

  case EXPRTK_NODE_BINARY_OP:
    ts_prescan_variables(c, node->data.binary.left);
    ts_prescan_variables(c, node->data.binary.right);
    break;

  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      ts_prescan_variables(c, node->data.function.args[i]);
    }
    break;

  case EXPRTK_NODE_MEMBER_CALL:
    ts_prescan_variables(c, node->data.member_call.object);
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      ts_prescan_variables(c, node->data.member_call.args[i]);
    }
    break;

  case EXPRTK_NODE_MEMBER_ACCESS:
    ts_prescan_variables(c, node->data.member_access.object);
    break;

  case EXPRTK_NODE_MEMBER_SET:
    ts_prescan_variables(c, node->data.member_set.object);
    ts_prescan_variables(c, node->data.member_set.value);
    break;

  case EXPRTK_NODE_INDEX:
    ts_prescan_variables(c, node->data.index_access.array);
    ts_prescan_variables(c, node->data.index_access.index);
    break;

  case EXPRTK_NODE_SLICE:
    ts_prescan_variables(c, node->data.slice.array);
    ts_prescan_variables(c, node->data.slice.start);
    ts_prescan_variables(c, node->data.slice.end);
    break;

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i) {
      ts_prescan_variables(c, node->data.vector.elements[i]);
    }
    break;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      ts_prescan_variables(c, node->data.map_literal.values[i]);
    }
    break;

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      ts_prescan_variables(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_IF:
    ts_prescan_variables(c, node->data.if_stmt.condition);
    ts_prescan_variables(c, node->data.if_stmt.if_branch);
    ts_prescan_variables(c, node->data.if_stmt.else_branch);
    break;

  case EXPRTK_NODE_WHILE:
    ts_prescan_variables(c, node->data.while_loop.condition);
    ts_prescan_variables(c, node->data.while_loop.body);
    break;

  case EXPRTK_NODE_FOR:
    ts_prescan_variables(c, node->data.for_loop.init);
    ts_prescan_variables(c, node->data.for_loop.condition);
    ts_prescan_variables(c, node->data.for_loop.post);
    ts_prescan_variables(c, node->data.for_loop.body);
    break;

  case EXPRTK_NODE_DO_WHILE:
    ts_prescan_variables(c, node->data.do_while.body);
    ts_prescan_variables(c, node->data.do_while.condition);
    break;

  case EXPRTK_NODE_FOR_IN:
    if (node->data.for_in.var_name) (void)ts_mir_get_or_create_reg(c, node->data.for_in.var_name);
    ts_prescan_variables(c, node->data.for_in.collection);
    ts_prescan_variables(c, node->data.for_in.body);
    break;

  case EXPRTK_NODE_FLOW:
    ts_prescan_variables(c, node->data.flow.value);
    break;

  case EXPRTK_NODE_SWITCH:
    ts_prescan_variables(c, node->data.switch_stmt.value);
    for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
      ts_prescan_variables(c, node->data.switch_stmt.cases[i]);
    }
    ts_prescan_variables(c, node->data.switch_stmt.default_case);
    break;

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    ts_register_destructure_targets(c, node->data.destructuring.targets);
    ts_prescan_variables(c, node->data.destructuring.value);
    break;

  case EXPRTK_NODE_TRY_CATCH:
    ts_prescan_variables(c, node->data.try_catch.try_body);
    if (node->data.try_catch.catch_var) (void)ts_mir_get_or_create_reg(c, node->data.try_catch.catch_var);
    ts_prescan_variables(c, node->data.try_catch.catch_body);
    break;

  case EXPRTK_NODE_THROW:
    ts_prescan_variables(c, node->data.throw_stmt.value);
    break;

  case EXPRTK_NODE_SPREAD:
    ts_prescan_variables(c, node->data.spread.child);
    break;

  case EXPRTK_NODE_NEW:
    for (size_t i = 0; i < node->data.new_expr.arg_count; ++i) {
      ts_prescan_variables(c, node->data.new_expr.args[i]);
    }
    break;

  case EXPRTK_NODE_INSTANCEOF:
    ts_prescan_variables(c, node->data.instanceof_expr.object);
    ts_prescan_variables(c, node->data.instanceof_expr.class_expr);
    break;

  case EXPRTK_NODE_FUNCTION_DEFINITION:
  case EXPRTK_NODE_FUNCTION_EXPRESSION:
  case EXPRTK_NODE_GENERATOR_FUNCTION:
  case EXPRTK_NODE_CLASS_DEF:
  case EXPRTK_NODE_METHOD:
    break;

  default:
    break;
  }
}

static int ts_oop_receiver_can_assign_temp(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    return 1;

  case EXPRTK_NODE_NEW:
    return node->data.new_expr.class_name != NULL;

  case EXPRTK_NODE_FUNCTION_CALL:
    return node->data.function.name &&
           !ts_call_has_spread_args(node->data.function.arg_count, node->data.function.args) &&
           (ts_mir_is_known_class_name(c, node->data.function.name) ||
            ts_call_assignment_needs_value_bridge(c, node->data.function.name,
                                                  node->data.function.arg_count));

  case EXPRTK_NODE_MEMBER_CALL:
    return node->data.member_call.object && node->data.member_call.method &&
           !ts_expr_contains_value_call(c, node->data.member_call.object) &&
           !ts_expr_list_contains_value_call(c, node->data.member_call.arg_count,
                                             node->data.member_call.args) &&
           ts_oop_receiver_can_assign_temp(c, node->data.member_call.object);

  case EXPRTK_NODE_MEMBER_ACCESS:
    return node->data.member_access.object && node->data.member_access.member &&
           !ts_expr_contains_value_call(c, node->data.member_access.object) &&
           ts_oop_receiver_can_assign_temp(c, node->data.member_access.object);

  default:
    return 0;
  }
}

static MIR_reg_t ts_emit_assignment(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_node_t *rhs = node->data.assignment.value;
  if (rhs && rhs->type == EXPRTK_NODE_NEW && rhs->data.new_expr.class_name) {
    if (ts_oop_call_args_need_value_bridge(c, rhs->data.new_expr.arg_count,
                                           rhs->data.new_expr.args)) {
      MIR_reg_t res = ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
      ts_mir_set_var_class(c, node->data.assignment.name, rhs->data.new_expr.class_name);
      return res;
    }
    MIR_reg_t res = ts_emit_oop_new_assign(c, node->data.assignment.name, rhs);
    ts_mir_set_var_class(c, node->data.assignment.name, rhs->data.new_expr.class_name);
    return res;
  }
  if (rhs && rhs->type == EXPRTK_NODE_FUNCTION_CALL && rhs->data.function.name &&
      ts_mir_is_known_class_name(c, rhs->data.function.name)) {
    if (ts_oop_call_args_need_value_bridge(c, rhs->data.function.arg_count,
                                           rhs->data.function.args)) {
      MIR_reg_t res = ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
      ts_mir_set_var_class(c, node->data.assignment.name, rhs->data.function.name);
      return res;
    }
    MIR_reg_t res = ts_emit_oop_class_call_assign(c, node->data.assignment.name, rhs);
    ts_mir_set_var_class(c, node->data.assignment.name, rhs->data.function.name);
    return res;
  }
  if (rhs && rhs->type == EXPRTK_NODE_FUNCTION_CALL && rhs->data.function.name) {
    MIR_reg_t hof_res = ts_try_emit_hof_specialized_call(c, rhs);
    if (hof_res) {
      MIR_reg_t target = ts_mir_get_or_create_reg(c, node->data.assignment.name);
      ts_mir_clear_var_class(c, node->data.assignment.name);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, hof_res)));
      ts_mir_mark_var_dirty(c, node->data.assignment.name);
      ts_mir_mark_var_numeric(c, node->data.assignment.name);
      return target;
    }
    if (strcmp(rhs->data.function.name, "await") == 0) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_await_assign(c, node->data.assignment.name,
                                  rhs->data.function.arg_count > 0 ? rhs->data.function.args[0]
                                                                   : NULL);
    }
    if (ts_call_args_need_ast_eval(c, rhs->data.function.arg_count, rhs->data.function.args)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_call_value_assign(c, node->data.assignment.name, rhs->data.function.name, rhs);
    }
    if (ts_runtime_call_needs_value_eval(c, rhs->data.function.name,
                                         rhs->data.function.arg_count)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    if (ts_call_assignment_needs_value_bridge(c, rhs->data.function.name,
                                              rhs->data.function.arg_count)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_runtime_call_assign(c, node->data.assignment.name, rhs->data.function.name,
                                         rhs->data.function.arg_count, rhs->data.function.args);
    }
  }
  if (rhs && rhs->type == EXPRTK_NODE_MEMBER_CALL && rhs->data.member_call.object &&
      rhs->data.member_call.method) {
    if (ts_member_call_is_stream_chain(rhs)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
    }
    if (rhs->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        ts_mir_var_is_dynamic(c, rhs->data.member_call.object->data.variable.name)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_oop_member_call_assign_value(
          c, node->data.assignment.name, rhs->data.member_call.object->data.variable.name,
          rhs->data.member_call.method, rhs->data.member_call.object, rhs);
    }
    if (ts_expr_contains_value_call(c, rhs->data.member_call.object)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    if (rhs->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        rhs->data.member_call.object->data.variable.name) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               rhs->data.member_call.object->data.variable.name, rhs->data.member_call.method);
      if (ts_resolves_runtime_func(c, full_name) || ts_call_name_needs_value_eval(full_name)) {
        char *full_name_copy = strdup(full_name);
        if (!full_name_copy) {
          ts_mir_fail(c, "JIT compile error: out of memory duplicating function name '%s'",
                      full_name);
          return ts_emit_zero_reg(c);
        }
        ts_mir_clear_var_class(c, node->data.assignment.name);
        return ts_emit_call_value_assign(c, node->data.assignment.name, full_name_copy, rhs);
      }
    }
    if (ts_call_args_need_ast_eval(c, rhs->data.member_call.arg_count,
                                   rhs->data.member_call.args) &&
        !ts_oop_call_args_need_value_bridge(c, rhs->data.member_call.arg_count,
                                            rhs->data.member_call.args)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, rhs->data.member_call.object);
    if (!obj_name) return ts_emit_unsupported_node(c, rhs);

    if (ts_oop_call_args_need_value_bridge(c, rhs->data.member_call.arg_count,
                                           rhs->data.member_call.args)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_oop_member_call_assign_value(c, node->data.assignment.name, obj_name,
                                                  rhs->data.member_call.method,
                                                  rhs->data.member_call.object, rhs);
    }

    MIR_reg_t arg_regs[16];
    size_t argc =
        ts_compile_call_args(c, rhs->data.member_call.arg_count, rhs->data.member_call.args,
                             arg_regs);
    const char *exact_class = ts_mir_find_var_class(c, obj_name);
    if (exact_class) {
      MIR_reg_t inline_res =
          ts_try_emit_oop_inline_method_call(c, obj_name, exact_class,
                                             rhs->data.member_call.method, argc, arg_regs);
      if (inline_res) {
        MIR_reg_t target = ts_mir_get_or_create_reg(c, node->data.assignment.name);
        ts_mir_clear_var_class(c, node->data.assignment.name);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                     MIR_new_reg_op(c->ctx, inline_res)));
        ts_mir_mark_var_dirty(c, node->data.assignment.name);
        ts_mir_mark_var_numeric(c, node->data.assignment.name);
        return target;
      }
    }
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_oop_member_call_assign(c, node->data.assignment.name, obj_name,
                                          rhs->data.member_call.method,
                                          rhs->data.member_call.object, argc, arg_regs);
  }
  if (rhs && rhs->type == EXPRTK_NODE_VARIABLE && rhs->data.variable.name &&
      ts_mir_is_known_class_name(c, rhs->data.variable.name)) {
    MIR_reg_t res =
        ts_emit_oop_class_alias(c, node->data.assignment.name, rhs->data.variable.name);
    ts_mir_add_class_name(c, node->data.assignment.name);
    return res;
  }
  if (rhs && rhs->type == EXPRTK_NODE_VARIABLE && rhs->data.variable.name &&
      ts_mir_var_is_dynamic(c, rhs->data.variable.name)) {
    ts_mir_clear_var_class(c, node->data.assignment.name);
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_dynamic_var_assign(c, node->data.assignment.name, rhs->data.variable.name);
  }
  if (rhs && rhs->type == EXPRTK_NODE_MEMBER_ACCESS && rhs->data.member_access.object &&
      rhs->data.member_access.member) {
    if (rhs->data.member_access.object->type == EXPRTK_NODE_MAP_LITERAL) {
      exprtk_node_t *value =
          ts_find_map_literal_member(rhs->data.member_access.object, rhs->data.member_access.member);
      if (!value) return ts_emit_unsupported_node(c, rhs);
      MIR_reg_t val = ts_compile_expr(c, value);
      MIR_reg_t target = ts_mir_get_or_create_reg(c, node->data.assignment.name);
      ts_mir_clear_var_class(c, node->data.assignment.name);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, val)));
      ts_mir_mark_var_dirty(c, node->data.assignment.name);
      ts_mir_mark_var_numeric(c, node->data.assignment.name);
      return target;
    }
    if (ts_expr_contains_value_call(c, rhs->data.member_access.object)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
    }
    if (!ts_oop_receiver_can_assign_temp(c, rhs->data.member_access.object)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, rhs->data.member_access.object);
    if (!obj_name) return ts_emit_unsupported_node(c, rhs);
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_member_access_assign(c, node->data.assignment.name, obj_name,
                                        rhs->data.member_access.member,
                                        rhs->data.member_access.object);
  }

  ts_mir_clear_var_class(c, node->data.assignment.name);

  if (rhs && rhs->type == EXPRTK_NODE_STRING) {
    MIR_reg_t res = ts_emit_string_literal_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_TEMPLATE_STRING) {
    MIR_reg_t res = ts_emit_template_string_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_FUNCTION_EXPRESSION) {
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_function_expr_assign(c, node->data.assignment.name, rhs);
  }

  if (rhs && rhs->type == EXPRTK_NODE_TRY_CATCH) {
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_try_catch_assign(c, node->data.assignment.name, rhs);
  }

  if (rhs && rhs->type == EXPRTK_NODE_NULL) {
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_VECTOR) {
    MIR_reg_t res = ts_emit_vector_literal_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_MAP_LITERAL) {
    MIR_reg_t res = ts_emit_map_literal_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    return ts_emit_map_value_literal_assign(c, node->data.assignment.name, rhs);
  }

  if (ts_is_non_numeric_node(rhs)) {
    (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (ts_expr_contains_value_call(c, rhs)) {
    return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
  }

  if (ts_index_assignment_needs_value_eval(c, rhs)) {
    return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
  }

  MIR_reg_t val = ts_compile_expr(c, rhs);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, node->data.assignment.name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, val)));
  ts_mir_mark_var_dirty(c, node->data.assignment.name);  // 标记为脏
  ts_mir_mark_var_numeric(c, node->data.assignment.name);
  return target;
}

static int ts_compile_data_access_and_assignment(ts_mir_compiler_t *c, exprtk_node_t *node,
                                                 MIR_reg_t *out) {
  if (!node || !out) return 0;

  switch (node->type) {
  case EXPRTK_NODE_ASSIGNMENT:
    *out = ts_emit_assignment(c, node);
    return 1;

  case EXPRTK_NODE_CONSTANT_DECL: {
    exprtk_node_t *value = node->data.assignment.value;
    if (value && value->type == EXPRTK_NODE_NULL) {
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      *out = ts_emit_runtime_value_node(c, node);
      return 1;
    }
    if (value && value->type == EXPRTK_NODE_STRING) {
      MIR_reg_t res = ts_emit_string_literal_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (value && value->type == EXPRTK_NODE_TEMPLATE_STRING) {
      MIR_reg_t res = ts_emit_template_string_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (value && value->type == EXPRTK_NODE_VARIABLE && value->data.variable.name &&
        ts_mir_var_is_dynamic(c, value->data.variable.name)) {
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      *out = ts_emit_dynamic_var_assign(c, node->data.assignment.name, value->data.variable.name);
      return 1;
    }
    if (value && value->type == EXPRTK_NODE_VECTOR) {
      MIR_reg_t res = ts_emit_vector_literal_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (value && value->type == EXPRTK_NODE_MAP_LITERAL) {
      MIR_reg_t res = ts_emit_map_literal_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (ts_is_non_numeric_node(value) || ts_expr_contains_value_call(c, value) ||
        ts_index_assignment_needs_value_eval(c, value)) {
      (void)ts_mir_get_or_create_reg(c, node->data.assignment.name);
      *out = ts_emit_runtime_value_node(c, node);
      return 1;
    }
    MIR_reg_t val = ts_compile_expr(c, value);
    MIR_reg_t target = ts_mir_get_or_create_reg(c, node->data.assignment.name);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                 MIR_new_reg_op(c->ctx, val)));
    ts_mir_mark_var_dirty(c, node->data.assignment.name);  // 标记为脏
    ts_mir_mark_var_numeric(c, node->data.assignment.name);
    *out = target;
    return 1;
  }

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    *out = ts_emit_destructure_assignment(c, node);
    if (!*out) {
      ts_register_destructure_targets(c, node->data.destructuring.targets);
      *out = ts_emit_runtime_value_node(c, node);
    }
    return 1;

  case EXPRTK_NODE_NULL: {
    MIR_reg_t r = ts_mir_new_temp_reg(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r), MIR_new_double_op(c->ctx, 0.0)));
    *out = r;
    return 1;
  }

  case EXPRTK_NODE_INDEX:
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VARIABLE && c->ctx_reg) {
      const char *name = node->data.index_access.array->data.variable.name;
      *out = ts_emit_index_access(c, name, node->data.index_access.index);
    } else if (node->data.index_access.array &&
               node->data.index_access.array->type == EXPRTK_NODE_VECTOR) {
      int idx = 0;
      if (!ts_index_constant(node->data.index_access.index, &idx) || idx < 0 ||
          (size_t)idx >= node->data.index_access.array->data.vector.count) {
        *out = ts_emit_unsupported_node(c, node);
      } else {
        *out = ts_compile_expr(c, node->data.index_access.array->data.vector.elements[idx]);
      }
    } else {
      *out = ts_emit_runtime_value_node(c, node);
    }
    return 1;

  case EXPRTK_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object && node->data.member_access.member) {
      if (node->data.member_access.object->type == EXPRTK_NODE_MAP_LITERAL) {
        exprtk_node_t *value =
            ts_find_map_literal_member(node->data.member_access.object,
                                       node->data.member_access.member);
        *out = value ? ts_compile_expr(c, value) : ts_emit_unsupported_node(c, node);
        return 1;
      }
      if (ts_expr_contains_value_call(c, node->data.member_access.object) ||
          !ts_oop_receiver_can_assign_temp(c, node->data.member_access.object)) {
        *out = ts_emit_runtime_value_node(c, node);
        return 1;
      }
      const char *obj_name = ts_emit_oop_receiver_to_temp(c, node->data.member_access.object);
      const char *member = node->data.member_access.member;
      *out = obj_name ? ts_emit_member_access(c, obj_name, member, node->data.member_access.object)
                      : ts_emit_runtime_value_node(c, node);
    } else {
      *out = ts_emit_unsupported_node(c, node);
    }
    return 1;

  default:
    return 0;
  }
}

/* =========================================================================
 *  闭包支持：生成 Prologue 加载捕获变量
 * ========================================================================= */

/**
 * @brief 生成闭包 prologue，从闭包环境加载捕获的变量
 *
 * 在函数入口处为每个捕获变量生成：
 *   var_reg = call load_captured_var(ctx, "var_name", closure_env)
 */
static MIR_reg_t ts_emit_if_expression(ts_mir_compiler_t *c, exprtk_node_t *node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_label_t else_label = MIR_new_label(c->ctx);
  MIR_label_t end_label = MIR_new_label(c->ctx);

  ts_compile_branch_false(c, node->data.if_stmt.condition, else_label);
  if (c->failed) return res;

  if (node->data.if_stmt.if_branch) {
    MIR_reg_t then_reg = ts_compile_expr(c, node->data.if_stmt.if_branch);
    if (c->failed) return res;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_reg_op(c->ctx, then_reg)));
  } else {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 0.0)));
  }

  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));
  MIR_append_insn(c->ctx, c->func, else_label);

  if (node->data.if_stmt.else_branch) {
    MIR_reg_t else_reg = ts_compile_expr(c, node->data.if_stmt.else_branch);
    if (c->failed) return res;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_reg_op(c->ctx, else_reg)));
  } else {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 0.0)));
  }

  MIR_append_insn(c->ctx, c->func, end_label);
  return res;
}

MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return 0;
  if (c->failed) return ts_emit_zero_reg(c);

  /*  Try constant folding before anything else */
  {
    double folded;
    if (ts_try_fold_constant(node, &folded)) {
      MIR_reg_t r = ts_mir_new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                   MIR_new_double_op(c->ctx, folded)));
      return r;
    }
  }

  /* Fast sub-dispatch for data access + assignment nodes. */
  {
    MIR_reg_t specialized = 0;
    if (ts_compile_data_access_and_assignment(c, node, &specialized)) return specialized;
  }

  switch (node->type) {
  case EXPRTK_NODE_NUMBER: {
    MIR_reg_t r = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, node->data.number)));
    return r;
  }

  case EXPRTK_NODE_INTEGER: {
    MIR_reg_t r = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, (double)node->data.integer)));
    return r;
  }

  case EXPRTK_NODE_VARIABLE:
    if (node->data.variable.name && ts_mir_var_is_dynamic(c, node->data.variable.name)) {
      MIR_reg_t r = ts_mir_new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                                        MIR_new_reg_op(c->ctx, r),
                                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                                        MIR_new_uint_op(c->ctx,
                                                        (uint64_t)(uintptr_t)node->data.variable.name)));
      return r;
    }
    return ts_mir_get_or_create_reg(c, node->data.variable.name);

  case EXPRTK_NODE_BINARY_OP: {
    /*  Unary operators (left == NULL) */
    if (node->data.binary.left == NULL) {
      MIR_reg_t operand = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = ts_mir_new_temp_reg(c);

      if (node->data.binary.op == exprtk_TOKEN_NOT) {
        /* !x => (x == 0.0) ? 1.0 : 0.0 */
        MIR_reg_t ireg = ts_mir_new_temp_ireg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DEQ, MIR_new_reg_op(c->ctx, ireg),
                                     MIR_new_reg_op(c->ctx, operand),
                                     MIR_new_double_op(c->ctx, 0.0)));
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, ireg)));
        return res;
      } else if (node->data.binary.op == exprtk_TOKEN_MINUS) {
        /* unary minus: -x => 0.0 - x */
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DSUB, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_double_op(c->ctx, 0.0),
                                     MIR_new_reg_op(c->ctx, operand)));
        return res;
      } else if (node->data.binary.op == exprtk_TOKEN_PLUS) {
        /* unary plus: +x => x (no-op) */
        return operand;
      }
      /* unknown unary: just return the operand */
      return operand;
    }

    if (ts_binary_is_null_eq_compare(node)) {
      return ts_emit_runtime_value_node(c, node);
    }

    /*  Logical AND (short-circuit) */
    if (node->data.binary.op == exprtk_TOKEN_AND) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t res = ts_mir_new_temp_reg(c);
      MIR_label_t false_label = MIR_new_label(c->ctx);
      MIR_label_t end_label = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_double_op(c->ctx, 0.0)));

      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t ireg = ts_mir_new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DNE, MIR_new_reg_op(c->ctx, ireg),
                                   MIR_new_reg_op(c->ctx, right), MIR_new_double_op(c->ctx, 0.0)));
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, ireg)));
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, false_label);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_double_op(c->ctx, 0.0)));

      MIR_append_insn(c->ctx, c->func, end_label);
      return res;
    }

    /*  Logical OR (short-circuit) */
    if (node->data.binary.op == exprtk_TOKEN_OR) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t res = ts_mir_new_temp_reg(c);
      MIR_label_t true_label = MIR_new_label(c->ctx);
      MIR_label_t end_label = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_double_op(c->ctx, 0.0)));

      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t ireg = ts_mir_new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DNE, MIR_new_reg_op(c->ctx, ireg),
                                   MIR_new_reg_op(c->ctx, right), MIR_new_double_op(c->ctx, 0.0)));
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, ireg)));
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, true_label);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_double_op(c->ctx, 1.0)));

      MIR_append_insn(c->ctx, c->func, end_label);
      return res;
    }

    /*  Modulo via fmod() external call */
    if (node->data.binary.op == exprtk_TOKEN_MOD) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = ts_mir_new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.fmod_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.fmod_import),
                                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, left),
                                        MIR_new_reg_op(c->ctx, right)));
      return res;
    }

    /*  Power via pow() external call */
    if (node->data.binary.op == exprtk_TOKEN_POWER) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = ts_mir_new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.pow_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.pow_import),
                                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, left),
                                        MIR_new_reg_op(c->ctx, right)));
      return res;
    }

    if (node->data.binary.op == exprtk_TOKEN_EQ ||
        node->data.binary.op == exprtk_TOKEN_NE) {
      MIR_reg_t typeof_cmp = 0;
      if (ts_try_emit_typeof_oop_compare(c, node->data.binary.left, node->data.binary.right,
                                         node->data.binary.op, &typeof_cmp)) {
        return typeof_cmp;
      }
    }

    /* Compound assignments */
    MIR_reg_t res = 0;
    switch (node->data.binary.op) {
    case exprtk_TOKEN_ASSIGN_ADD:
    case exprtk_TOKEN_ASSIGN_SUB:
    case exprtk_TOKEN_ASSIGN_MUL:
    case exprtk_TOKEN_ASSIGN_DIV: {
      if (node->data.binary.left->type != EXPRTK_NODE_VARIABLE) return 0;
      ts_mir_clear_var_class(c, node->data.binary.left->data.variable.name);
      MIR_reg_t target = ts_mir_get_or_create_reg(c, node->data.binary.left->data.variable.name);
      MIR_reg_t rhs = ts_compile_expr(c, node->data.binary.right);
      MIR_insn_code_t op;
      if (node->data.binary.op == exprtk_TOKEN_ASSIGN_ADD) op = MIR_DADD;
      else if (node->data.binary.op == exprtk_TOKEN_ASSIGN_SUB) op = MIR_DSUB;
      else if (node->data.binary.op == exprtk_TOKEN_ASSIGN_MUL) op = MIR_DMUL;
      else op = MIR_DDIV;
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, rhs)));
      return target;
    }
    default: {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_insn_code_t op;
      int is_cmp = 0;
      switch (node->data.binary.op) {
      case exprtk_TOKEN_PLUS:
        op = MIR_DADD;
        break;
      case exprtk_TOKEN_MINUS:
        op = MIR_DSUB;
        break;
      case exprtk_TOKEN_MULTIPLY:
        op = MIR_DMUL;
        break;
      case exprtk_TOKEN_DIVIDE:
        op = MIR_DDIV;
        break;
      case exprtk_TOKEN_LT:
        op = MIR_DLT;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_GT:
        op = MIR_DGT;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_LE:
        op = MIR_DLE;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_GE:
        op = MIR_DGE;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_EQ:
        op = MIR_DEQ;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_NE:
        op = MIR_DNE;
        is_cmp = 1;
        break;
      default:
        op = MIR_DADD;
        break;
      }
      if (is_cmp) {
        MIR_reg_t ireg = ts_mir_new_temp_ireg(c);
        res = ts_mir_new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, ireg),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, ireg)));
      } else {
        res = ts_mir_new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
      }
    }
    }
    return res;
  }

  case EXPRTK_NODE_IF:
    return ts_emit_if_expression(c, node);

  case EXPRTK_NODE_BLOCK:
    ts_register_helper_written_vars(c, node);
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_INSTANCEOF: {
    if (node->data.instanceof_expr.object &&
        (node->data.instanceof_expr.class_name || node->data.instanceof_expr.class_expr)) {
      const char *obj_name = ts_emit_oop_receiver_to_temp(c, node->data.instanceof_expr.object);
      const char *class_name = node->data.instanceof_expr.class_name;
      if (!class_name && node->data.instanceof_expr.class_expr) {
        class_name = ts_emit_oop_receiver_to_temp(c, node->data.instanceof_expr.class_expr);
      }
      return obj_name && class_name ? ts_emit_oop_instanceof(c, obj_name, class_name)
                                    : ts_emit_unsupported_node(c, node);
    }
    return ts_emit_unsupported_node(c, node);
  }

  /* Phase 4+9: Function call compilation */
  case EXPRTK_NODE_FUNCTION_CALL: {
    size_t argc = node->data.function.arg_count;
    const char *name = node->data.function.name;
    if (!name) return 0;
    MIR_reg_t hof_res = ts_try_emit_hof_specialized_call(c, node);
    if (hof_res) return hof_res;
    const char *alias_name = ts_find_func_alias(c, name);
    if (alias_name) name = alias_name;

    if (strcmp(name, "await") == 0) {
      return ts_emit_await_value(c, argc > 0 ? node->data.function.args[0] : NULL);
    }

    if (ts_runtime_call_needs_value_eval(c, name, argc) ||
        ts_call_args_need_ast_eval(c, argc, node->data.function.args) ||
        ts_expr_list_contains_value_call(c, argc, node->data.function.args)) {
      return ts_emit_runtime_value_node(c, node);
    }

    int oop_predicate = ts_oop_predicate_kind(name);
    if (oop_predicate && argc == 1 &&
        ts_oop_predicate_can_preserve_value(oop_predicate, node->data.function.args[0])) {
      return ts_emit_oop_predicate(c, oop_predicate, node->data.function.args[0]);
    }

    MIR_reg_t arg_regs[16];
    argc = ts_compile_call_args(c, argc, node->data.function.args, arg_regs);

    MIR_reg_t res = ts_mir_new_temp_reg(c);

    /*  Check if function was compiled as native MIR */
    ts_compiled_func_t *cf = ts_find_compiled_func(c, name);
    if (cf && cf->arg_count == argc) {
      MIR_reg_t call_res = ts_emit_compiled_func_call(c, cf, argc, arg_regs);
      if (call_res) return call_res;
    }

    /*  direct MIR call for known math functions */
    if (ts_emit_direct_math_call(c, name, argc, arg_regs, res)) return res;

    /* Direct compile-time dispatch, otherwise use named runtime helper dispatch. */
    if (ts_emit_direct_resolved_call(c, name, argc, arg_regs, res)) return res;
    ts_emit_runtime_call(c, name, argc, arg_regs, res);
    return res;
  }

  /*  Member call compilation (module.method) */
  case EXPRTK_NODE_MEMBER_CALL: {
    if (!node->data.member_call.object || !node->data.member_call.method)
      return ts_emit_unsupported_node(c, node);
    if (ts_member_call_is_stream_chain(node)) {
      return ts_emit_runtime_value_node(c, node);
    }
    if (node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_call.object->data.variable.name) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               node->data.member_call.object->data.variable.name,
               node->data.member_call.method);
      if (ts_resolves_runtime_func(c, full_name) || ts_call_name_needs_value_eval(full_name)) {
        return ts_emit_runtime_value_node(c, node);
      }
    }
    if ((ts_call_args_need_ast_eval(c, node->data.member_call.arg_count,
                                    node->data.member_call.args) &&
         !ts_oop_call_args_need_value_bridge(c, node->data.member_call.arg_count,
                                             node->data.member_call.args)) ||
        ts_expr_list_contains_value_call(c, node->data.member_call.arg_count,
                                         node->data.member_call.args)) {
      return ts_emit_runtime_value_node(c, node);
    }
    int object_is_variable = node->data.member_call.object->type == EXPRTK_NODE_VARIABLE;
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, node->data.member_call.object);
    if (!obj_name) return ts_emit_unsupported_node(c, node);

    /* Keep direct module/builtin dispatch, but let unresolved member calls use
     * the OOP-specific IR helper so receiver semantics are preserved. */
    size_t argc = node->data.member_call.arg_count;
    int needs_value_bridge =
        ts_oop_call_args_need_value_bridge(c, argc, node->data.member_call.args);

    MIR_reg_t res = ts_mir_new_temp_reg(c);
    MIR_reg_t arg_regs[16];
    if (!needs_value_bridge) {
      argc = ts_compile_call_args(c, argc, node->data.member_call.args, arg_regs);
    }
    if (object_is_variable && !needs_value_bridge) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s", obj_name, node->data.member_call.method);
      if (ts_emit_direct_resolved_call(c, full_name, argc, arg_regs, res)) return res;
    }

    const char *exact_class = ts_mir_find_var_class(c, obj_name);
    if (needs_value_bridge) {
      if (exact_class) {
        return ts_emit_oop_member_call_mono_value(c, obj_name, exact_class,
                                                  node->data.member_call.method,
                                                  node->data.member_call.object, node);
      }
      return ts_emit_oop_member_call_value(c, obj_name, node->data.member_call.method,
                                           node->data.member_call.object, node);
    }
    if (exact_class) {
      MIR_reg_t inline_res =
          ts_try_emit_oop_inline_method_call(c, obj_name, exact_class,
                                             node->data.member_call.method, argc, arg_regs);
      if (inline_res) return inline_res;
      int reload_after =
          !ts_oop_member_call_can_skip_reload(c, exact_class, node->data.member_call.method, argc);
      return ts_emit_oop_member_call_cached(c, obj_name, node->data.member_call.method,
                                            node->data.member_call.object, argc, arg_regs,
                                            reload_after);
    }
    return ts_emit_oop_member_call_cached(c, obj_name, node->data.member_call.method,
                                          node->data.member_call.object, argc, arg_regs, 1);
  }

  /* String/vector/map literals need real MIR/helper lowering before JIT can accept them. */
  case EXPRTK_NODE_STRING:
  case EXPRTK_NODE_VECTOR:
  case EXPRTK_NODE_SLICE:
  case EXPRTK_NODE_TEMPLATE_STRING:
  case EXPRTK_NODE_MAP_LITERAL:
  case EXPRTK_NODE_FUNCTION_EXPRESSION:
  case EXPRTK_NODE_SPREAD:
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_TRY_CATCH:
    ts_register_helper_written_vars(c, node);
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_THROW:
    return ts_emit_runtime_value_node(c, node);

  /* Complex assignment/control nodes are rejected unless explicitly lowered above. */
  case EXPRTK_NODE_MEMBER_SET:
    if (node->data.member_set.object &&
        node->data.member_set.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_set.member && node->data.member_set.value) {
      if (!ts_oop_member_can_use_num_slot(
              c, node->data.member_set.object->data.variable.name,
              node->data.member_set.member)) {
        return ts_emit_runtime_value_node(c, node);
      }
      return ts_emit_oop_member_set(c, node->data.member_set.object->data.variable.name,
                                    node->data.member_set.member, node->data.member_set.object,
                                    node->data.member_set.value);
    }
    return ts_emit_unsupported_node(c, node);

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    {
      MIR_reg_t res = ts_emit_destructure_assignment(c, node);
      if (res) return res;
    }
    ts_register_destructure_targets(c, node->data.destructuring.targets);
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_REST_PARAMETER:
    return ts_emit_unsupported_node(c, node);

  default: {
    return ts_emit_unsupported_node(c, node);
  }
  }
}

/* =========================================================================
 *  Optimized condition branching — emit direct DBXX for comparisons,
 * avoiding the I2D + DBEQ 0.0 round-trip in hot loops.
 * Jumps to false_label when the condition evaluates to false.
 * ========================================================================= */

static void ts_compile_branch_false(ts_mir_compiler_t *c, exprtk_node_t *node,
                                    MIR_label_t false_label) {
  if (!c || c->failed || !node) return;

  /* Binary comparison → single DBXX branch instruction */
  if (node->type == EXPRTK_NODE_BINARY_OP) {
    /* NOT: unary, left is NULL — branch false when operand is true (non-zero) */
    if (node->data.binary.op == exprtk_TOKEN_NOT && node->data.binary.left == NULL) {
      MIR_reg_t val = ts_compile_expr(c, node->data.binary.right);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, val), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    if (ts_binary_is_null_eq_compare(node)) {
      MIR_reg_t cond = ts_emit_runtime_value_node(c, node);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    if (node->data.binary.left != NULL) {
      MIR_insn_code_t bop = 0;
      /* Map "condition is false" to the negated branch:
       * a < b  false → a >= b → DBGE
       * a > b  false → a <= b → DBLE
       * a <= b false → a > b  → DBGT
       * a >= b false → a < b  → DBLT
       * a == b false → a != b → DBNE
       * a != b false → a == b → DBEQ */
      switch (node->data.binary.op) {
      case exprtk_TOKEN_LT:
        bop = MIR_DBGE;
        break;
      case exprtk_TOKEN_GT:
        bop = MIR_DBLE;
        break;
      case exprtk_TOKEN_LE:
        bop = MIR_DBGT;
        break;
      case exprtk_TOKEN_GE:
        bop = MIR_DBLT;
        break;
      case exprtk_TOKEN_EQ:
        bop = MIR_DBNE;
        break;
      case exprtk_TOKEN_NE:
        bop = MIR_DBEQ;
        break;
      default:
        break;
      }
      if (bop) {
        MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
        MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, bop, MIR_new_label_op(c->ctx, false_label),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
        return;
      }

      /* AND short-circuit: if left is false, jump to false_label;
       * then if right is false, jump to false_label */
      if (node->data.binary.op == exprtk_TOKEN_AND) {
        ts_compile_branch_false(c, node->data.binary.left, false_label);
        ts_compile_branch_false(c, node->data.binary.right, false_label);
        return;
      }

      /* OR short-circuit: if left is true, skip to end (success);
       * then if right is false, jump to false_label */
      if (node->data.binary.op == exprtk_TOKEN_OR) {
        MIR_label_t true_label = MIR_new_label(c->ctx);
        ts_compile_branch_true(c, node->data.binary.left, true_label);
        ts_compile_branch_false(c, node->data.binary.right, false_label);
        MIR_append_insn(c->ctx, c->func, true_label);
        return;
      }
    }
  }

  /* Generic condition: compile expression, branch if == 0.0 */
  MIR_reg_t cond = ts_compile_expr(c, node);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                               MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
}

/*  Branch to true_label when condition is true (helper for OR) */
static void ts_compile_branch_true(ts_mir_compiler_t *c, exprtk_node_t *node,
                                   MIR_label_t true_label) {
  if (!c || c->failed || !node) return;

  if (node->type == EXPRTK_NODE_BINARY_OP && node->data.binary.left != NULL) {
    if (ts_binary_is_null_eq_compare(node)) {
      MIR_reg_t cond = ts_emit_runtime_value_node(c, node);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    MIR_insn_code_t bop = 0;
    /* Direct branch when condition IS true */
    switch (node->data.binary.op) {
    case exprtk_TOKEN_LT:
      bop = MIR_DBLT;
      break;
    case exprtk_TOKEN_GT:
      bop = MIR_DBGT;
      break;
    case exprtk_TOKEN_LE:
      bop = MIR_DBLE;
      break;
    case exprtk_TOKEN_GE:
      bop = MIR_DBGE;
      break;
    case exprtk_TOKEN_EQ:
      bop = MIR_DBEQ;
      break;
    case exprtk_TOKEN_NE:
      bop = MIR_DBNE;
      break;
    default:
      break;
    }
    if (bop) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, bop, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
      return;
    }
  }

  /* Generic condition: compile expression, branch if != 0.0 */
  MIR_reg_t cond = ts_compile_expr(c, node);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                               MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
}

/* =========================================================================
 * Statement Compiler (Phase 1-5)
 * ========================================================================= */

void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || c->failed || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; i++) {
      ts_compile_stmt(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_CLASS_DEF:
    ts_mir_add_class_name(c, node->data.class_def.name);
    ts_emit_oop_define_class(c, node);
    break;

  case EXPRTK_NODE_IF: {
    MIR_label_t else_label = node->data.if_stmt.else_branch ? MIR_new_label(c->ctx) : NULL;
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /*  direct branch — no I2D + DBEQ round-trip */
    ts_compile_branch_false(c, node->data.if_stmt.condition, else_label ? else_label : end_label);

    if (node->data.if_stmt.if_branch) ts_compile_stmt(c, node->data.if_stmt.if_branch);

    if (else_label) {
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));
      MIR_append_insn(c->ctx, c->func, else_label);
      ts_compile_stmt(c, node->data.if_stmt.else_branch);
    }

    MIR_append_insn(c->ctx, c->func, end_label);
    break;
  }

  case EXPRTK_NODE_WHILE: {
    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* Push loop frame for break/continue */
    if (c->loop_depth < TS_MIR_MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = loop_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    /*  direct branch */
    ts_compile_branch_false(c, node->data.while_loop.condition, end_label);

    if (node->data.while_loop.body) ts_compile_stmt(c, node->data.while_loop.body);

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  case EXPRTK_NODE_FOR: {
    if (node->data.for_loop.init) ts_compile_stmt(c, node->data.for_loop.init);

    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t continue_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* Push loop frame: continue jumps to post, break jumps to end */
    if (c->loop_depth < TS_MIR_MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = continue_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    if (node->data.for_loop.condition) {
      /*  direct branch */
      ts_compile_branch_false(c, node->data.for_loop.condition, end_label);
    }

    if (node->data.for_loop.body) ts_compile_stmt(c, node->data.for_loop.body);

    MIR_append_insn(c->ctx, c->func, continue_label);
    if (node->data.for_loop.post) ts_compile_stmt(c, node->data.for_loop.post);

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  /*  do-while loop */
  case EXPRTK_NODE_DO_WHILE: {
    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    if (c->loop_depth < TS_MIR_MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = loop_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    if (node->data.do_while.body) ts_compile_stmt(c, node->data.do_while.body);

    /*  direct branch back when condition is true */
    ts_compile_branch_true(c, node->data.do_while.condition, loop_label);

    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  /*  break/continue/return */
  case EXPRTK_NODE_FLOW: {
    if (node->data.flow.type == exprtk_TOKEN_BREAK && c->loop_depth > 0) {
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_JMP,
                       MIR_new_label_op(c->ctx, c->loop_stack[c->loop_depth - 1].break_label)));
    } else if (node->data.flow.type == exprtk_TOKEN_CONTINUE && c->loop_depth > 0) {
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_JMP,
                       MIR_new_label_op(c->ctx, c->loop_stack[c->loop_depth - 1].continue_label)));
    } else if (node->data.flow.type == exprtk_TOKEN_RETURN) {
      /* Emit epilogue (store vars back) then return */
      MIR_reg_t ret_val;
      if (node->data.flow.value) {
        ret_val = ts_compile_expr(c, node->data.flow.value);
      } else {
        ret_val = ts_mir_new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, ret_val),
                                     MIR_new_double_op(c->ctx, 0.0)));
      }
      /* Store all variables back to env before returning (skip in compiled functions) */
      if (c->ctx_reg) {
        for (int i = 0; i < c->var_count; i++) {
          if (c->vars[i]->dynamic_value) continue;
          if (!c->vars[i]->dirty) continue;
          MIR_append_insn(
              c->ctx, c->func,
                                MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                MIR_new_reg_op(c->ctx, c->ctx_reg),
                                MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
        }
      }
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_val)));
    }
    break;
  }

  /* Phase 6+9: Function definition — already compiled in pre-scan, just register in env */
  case EXPRTK_NODE_FUNCTION_DEFINITION: {
    /* The MIR function was already compiled during ts_prescan_functions.
     * Register it in env so runtime helper dispatch and reflection see it. */
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 4, MIR_new_ref_op(c->ctx, c->ext.define_func_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.define_func_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
    break;
  }

  /* For-in: native compilation for pre-bound vectors; other collections are unsupported. */
  case EXPRTK_NODE_FOR_IN: {
    const char *iter_name = node->data.for_in.var_name;
    exprtk_node_t *collection = node->data.for_in.collection;
    const char *vec_name = NULL;
    int native = 0;

    if (collection && collection->type == EXPRTK_NODE_VARIABLE)
      vec_name = collection->data.variable.name;

    if (vec_name && c->ctx_reg) {
      exprtk_value_t existing = exprtk_env_get(&c->ts_ctx->env, vec_name);
      if (existing.type == EXPRTK_VAL_VECTOR && existing.data.vector.data) {
        native = 1;
        int64_t len = (int64_t)existing.data.vector.size;

        /* Reuse Phase 10 vec_data pointer cache */
        MIR_reg_t ptr_reg = ts_mir_get_or_add_vec_ptr(c, vec_name);

        if (ptr_reg) {
          MIR_reg_t iter_reg = ts_mir_get_or_create_reg(c, iter_name);
          MIR_reg_t i_reg = ts_mir_new_temp_ireg(c);

          /* i = 0 */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, i_reg),
                                       MIR_new_int_op(c->ctx, 0)));

          MIR_label_t loop_label = MIR_new_label(c->ctx);
          MIR_label_t continue_label = MIR_new_label(c->ctx);
          MIR_label_t end_label = MIR_new_label(c->ctx);

          if (c->loop_depth < TS_MIR_MAX_LOOP_DEPTH) {
            c->loop_stack[c->loop_depth].break_label = end_label;
            c->loop_stack[c->loop_depth].continue_label = continue_label;
            c->loop_depth++;
          }

          MIR_append_insn(c->ctx, c->func, loop_label);

          /* if (i >= len) goto end */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_BGE, MIR_new_label_op(c->ctx, end_label),
                                       MIR_new_reg_op(c->ctx, i_reg), MIR_new_int_op(c->ctx, len)));

          /* iter_var = data[i] — native memory load */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, iter_reg),
                                       MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, i_reg, 8)));

          /* compile body */
          if (node->data.for_in.body) ts_compile_stmt(c, node->data.for_in.body);

          /* continue target: i++ */
          MIR_append_insn(c->ctx, c->func, continue_label);
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, i_reg),
                                       MIR_new_reg_op(c->ctx, i_reg), MIR_new_int_op(c->ctx, 1)));

          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
          MIR_append_insn(c->ctx, c->func, end_label);

          if (c->loop_depth > 0) c->loop_depth--;
        } else {
          native = 0;
        }
      }
    }

    if (!native) {
      if (iter_name) (void)ts_mir_get_or_create_reg(c, iter_name);
      ts_register_helper_written_vars(c, node->data.for_in.body);
      (void)ts_emit_runtime_value_node(c, node);
    }
    break;
  }

  /*  Switch statement — native MIR (chain of compare-and-branch) */
  case EXPRTK_NODE_SWITCH: {
    MIR_reg_t val = ts_compile_expr(c, node->data.switch_stmt.value);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* cases array: [case_val0, body0, case_val1, body1, ...], length = case_count * 2 */
    size_t num_cases = node->data.switch_stmt.case_count;
    for (size_t i = 0; i < num_cases; i++) {
      exprtk_node_t *case_val = node->data.switch_stmt.cases[i * 2];
      exprtk_node_t *case_body = node->data.switch_stmt.cases[i * 2 + 1];
      MIR_label_t next_case = MIR_new_label(c->ctx);

      MIR_reg_t cv = ts_compile_expr(c, case_val);
      /* If val != cv, skip to next case */
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, next_case),
                                   MIR_new_reg_op(c->ctx, val), MIR_new_reg_op(c->ctx, cv)));

      ts_compile_stmt(c, case_body);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, next_case);
    }

    /* Default case */
    if (node->data.switch_stmt.default_case) {
      ts_compile_stmt(c, node->data.switch_stmt.default_case);
    }

    MIR_append_insn(c->ctx, c->func, end_label);
    break;
  }

  default:
    ts_compile_expr(c, node);
    break;
  }
}
