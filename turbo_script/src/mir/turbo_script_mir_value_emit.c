/* Value bridge MIR emission helpers. */

#include "turbo_script_mir_internal.h"

int ts_call_assignment_needs_value_bridge(ts_mir_compiler_t *c, const char *name,
                                                 size_t argc) {
  MIR_item_t proto = NULL;
  MIR_item_t import = NULL;
  exprtk_func_t *f = NULL;
  const char *alias_name = NULL;

  if (!c || !name) return 0;
  alias_name = ts_find_func_alias(c, name);
  if (alias_name) name = alias_name;
  if (ts_find_compiled_func(c, name)) return 0;
  if (ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
  if (exprtk_find_builtin(name, &c->ts_ctx->env)) return 0;

  for (f = c->ts_ctx->env.funcs; f; f = f->next) {
    if (f->name && strcmp(f->name, name) == 0 && !f->is_script) return 0;
  }

  return 1;
}

MIR_reg_t ts_emit_runtime_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                      const char *name, size_t argc,
                                      exprtk_node_t **arg_nodes) {
  MIR_reg_t arg_regs[16];
  argc = ts_compile_call_args(c, argc, arg_nodes, arg_regs);

  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.call_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  return res;
}

MIR_reg_t ts_emit_call_value_assign(ts_mir_compiler_t *c, const char *target_name,
                                           const char *name, exprtk_node_t *call_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 7,
                                    MIR_new_ref_op(c->ctx, c->ext.call_value_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.call_value_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

MIR_reg_t ts_emit_value_expr_assign(ts_mir_compiler_t *c, const char *target_name,
                                           exprtk_node_t *expr_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)expr_node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static MIR_reg_t ts_emit_value_expr(ts_mir_compiler_t *c, exprtk_node_t *expr_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 5,
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)expr_node)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_await_value(ts_mir_compiler_t *c, exprtk_node_t *arg_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 5,
                                    MIR_new_ref_op(c->ctx, c->ext.await_value_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.await_value_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)arg_node)));
  ts_emit_reload_from_env(c);
  return res;
}

MIR_reg_t ts_emit_await_assign(ts_mir_compiler_t *c, const char *target_name,
                                      exprtk_node_t *arg_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.await_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.await_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)arg_node)));
  ts_emit_reload_from_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

MIR_reg_t ts_emit_function_expr_assign(ts_mir_compiler_t *c, const char *target_name,
                                              exprtk_node_t *node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.function_expr_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.function_expr_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

MIR_reg_t ts_emit_try_catch_assign(ts_mir_compiler_t *c, const char *target_name,
                                          exprtk_node_t *node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.try_catch_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.try_catch_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

MIR_reg_t ts_emit_dynamic_var_assign(ts_mir_compiler_t *c, const char *target_name,
                                            const char *source_name) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  if (!target_name || !source_name) return 0;

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.assign_var_proto),
                        MIR_new_ref_op(c->ctx, c->ext.assign_var_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)source_name)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

MIR_reg_t ts_emit_runtime_value_node(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) {
    MIR_reg_t res = ts_mir_new_temp_reg(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res), MIR_new_double_op(c->ctx, 0.0)));
    return res;
  }

  if ((node->type == EXPRTK_NODE_ASSIGNMENT || node->type == EXPRTK_NODE_CONSTANT_DECL) &&
      node->data.assignment.name) {
    return ts_emit_value_expr_assign(c, node->data.assignment.name, node);
  }

  return ts_emit_value_expr(c, node);
}
