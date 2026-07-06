/* Data, literal, member, and index MIR emission helpers. */

#include "turbo_script_mir_internal.h"
#include <stdlib.h>

int ts_vector_literal_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_VECTOR) return 1;

  for (size_t i = 0; i < node->data.vector.count; ++i) {
    exprtk_node_t *element = node->data.vector.elements[i];
    if (!element || element->type == EXPRTK_NODE_SPREAD || ts_is_non_numeric_node(element) ||
        ts_expr_contains_value_call(c, element) || ts_index_assignment_needs_value_eval(c, element)) {
      return 1;
    }
  }

  return 0;
}

MIR_reg_t ts_emit_vector_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                               exprtk_node_t *node) {
  MIR_reg_t element_regs[16];
  MIR_reg_t arr_reg;
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  size_t count;

  if (!target_name || !node || node->type != EXPRTK_NODE_VECTOR ||
      node->data.vector.count > 16 || ts_vector_literal_needs_value_eval(c, node)) {
    return 0;
  }

  count = node->data.vector.count;
  for (size_t i = 0; i < count; ++i) {
    element_regs[i] = ts_compile_expr(c, node->data.vector.elements[i]);
  }
  arr_reg = ts_emit_packed_args(c, count, element_regs);

  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.vec_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.vec_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_int_op(c->ctx, (int64_t)count), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

int ts_map_literal_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_MAP_LITERAL) return 1;

  for (size_t i = 0; i < node->data.map_literal.count; ++i) {
    exprtk_node_t *value = node->data.map_literal.values[i];
    if (!node->data.map_literal.keys[i] || !value || value->type == EXPRTK_NODE_SPREAD ||
        ts_is_non_numeric_node(value) || ts_expr_contains_value_call(c, value) ||
        ts_index_assignment_needs_value_eval(c, value)) {
      return 1;
    }
  }

  return 0;
}

MIR_reg_t ts_emit_map_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                            exprtk_node_t *node) {
  MIR_reg_t value_regs[16];
  MIR_reg_t arr_reg;
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  size_t count;

  if (!target_name || !node || node->type != EXPRTK_NODE_MAP_LITERAL ||
      node->data.map_literal.count > 16 || ts_map_literal_needs_value_eval(c, node)) {
    return 0;
  }

  count = node->data.map_literal.count;
  for (size_t i = 0; i < count; ++i) {
    value_regs[i] = ts_compile_expr(c, node->data.map_literal.values[i]);
  }
  arr_reg = ts_emit_packed_args(c, count, value_regs);

  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.map_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.map_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node),
                        MIR_new_int_op(c->ctx, (int64_t)count), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

MIR_reg_t ts_emit_map_value_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                                  exprtk_node_t *node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);

  if (!target_name || !node || node->type != EXPRTK_NODE_MAP_LITERAL) return 0;

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.map_value_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.map_value_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  ts_emit_reload_from_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

MIR_reg_t ts_emit_string_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                               exprtk_node_t *node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  if (!target_name || !node || node->type != EXPRTK_NODE_STRING ||
      !node->data.string.value.data) {
    return 0;
  }

  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.string_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.string_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node->data.string.value.data),
                        MIR_new_int_op(c->ctx, (int64_t)node->data.string.value.len)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

MIR_reg_t ts_emit_template_string_assign(ts_mir_compiler_t *c, const char *target_name,
                                                exprtk_node_t *node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);

  if (!target_name || !node || node->type != EXPRTK_NODE_TEMPLATE_STRING) {
    return 0;
  }

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.template_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.template_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

/* --- Access helpers: member/index fast path + runtime helpers --- */
MIR_reg_t ts_emit_member_access(ts_mir_compiler_t *c, const char *obj_name,
                                       const char *member, exprtk_node_t *object_node) {
  exprtk_value_t obj = exprtk_env_get(&c->ts_ctx->env, obj_name);
  MIR_reg_t oop_slot = ts_try_emit_oop_member_get_slot(c, obj_name, member, object_node);
  if (oop_slot) return oop_slot;

  if (exprtk_value_is_object_like(&obj) && exprtk_map_has(&obj, member)) {
    MIR_reg_t ptr_reg = ts_mir_get_or_add_map_ptr(c, obj_name, member);

    if (ptr_reg) {
      MIR_reg_t res = ts_mir_new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, 0, 1)));
      return res;
    }

    MIR_reg_t res = ts_mir_new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.map_get_key_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.map_get_key_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member)));
    return res;
  }

  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.member_get_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.member_get_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  return res;
}

MIR_reg_t ts_emit_member_access_assign(ts_mir_compiler_t *c, const char *target_name,
                                       const char *obj_name, const char *member,
                                       exprtk_node_t *object_node) {
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_reg_t target = ts_mir_get_or_create_reg(c, target_name);
  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));
  (void)target;

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.member_get_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.member_get_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

void ts_emit_sync_var_to_env(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    if (!c->vars[i]->dirty || strcmp(c->vars[i]->name, name) != 0) continue;

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
    c->vars[i]->dirty = 0;
    return;
  }
}

void ts_emit_sync_to_env(ts_mir_compiler_t *c) {
  // 增量同步：只同步脏变量
  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    if (!c->vars[i]->dirty) continue;  // 跳过未修改的变量

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
    c->vars[i]->dirty = 0;
  }
}

void ts_emit_reload_from_env(ts_mir_compiler_t *c) {
  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(
                        c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                        MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                        MIR_new_reg_op(c->ctx, c->vars[i]->reg),
                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name)));
  }
  // 清除脏标记（重新加载后所有变量都是干净的）
  ts_mir_clear_dirty_flags(c);
}

MIR_reg_t ts_emit_unsupported_node(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (node && node->type == EXPRTK_NODE_MEMBER_CALL) {
    ts_mir_fail(c,
                "MIR lowering error: unsupported member call '%s' on syntax object type %d",
                node->data.member_call.method ? node->data.member_call.method : "<null>",
                node->data.member_call.object ? (int)node->data.member_call.object->type : -1);
    return ts_emit_zero_reg(c);
  }
  if (node && (node->type == EXPRTK_NODE_ASSIGNMENT ||
               node->type == EXPRTK_NODE_CONSTANT_DECL)) {
    exprtk_node_t *value = node->data.assignment.value;
    if (value && value->type == EXPRTK_NODE_FUNCTION_CALL) {
      ts_mir_fail(c,
                  "MIR lowering error: unsupported assignment '%s' with RHS function call "
                  "'%s' argc %zu",
                  node->data.assignment.name ? node->data.assignment.name : "<null>",
                  value->data.function.name ? value->data.function.name : "<null>",
                  value->data.function.arg_count);
      return ts_emit_zero_reg(c);
    }
    ts_mir_fail(c,
                "MIR lowering error: unsupported assignment '%s' with RHS syntax node type %d",
                node->data.assignment.name ? node->data.assignment.name : "<null>",
                value ? (int)value->type : -1);
    return ts_emit_zero_reg(c);
  }
  if (node && node->type == EXPRTK_NODE_INDEX) {
    ts_mir_fail(c,
                "MIR lowering error: unsupported index array syntax node type %d",
                node->data.index_access.array ? (int)node->data.index_access.array->type : -1);
    return ts_emit_zero_reg(c);
  }
  ts_mir_fail(c, "MIR lowering error: unsupported syntax node type %d",
              node ? (int)node->type : -1);
  return ts_emit_zero_reg(c);
}

MIR_reg_t ts_emit_index_access(ts_mir_compiler_t *c, const char *name,
                                      exprtk_node_t *index_node) {
  MIR_reg_t idx_reg = ts_compile_expr(c, index_node);

  /* Native fast path for vectors pre-bound in env at compile time. */
  exprtk_value_t existing = exprtk_env_get(&c->ts_ctx->env, name);
  int is_prebound = (existing.type == EXPRTK_VAL_VECTOR && existing.data.vector.data != NULL);
  if (is_prebound) {
    MIR_reg_t ptr_reg = ts_mir_get_or_add_vec_ptr(c, name);

    if (ptr_reg) {
      MIR_reg_t idx_i = ts_mir_new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_D2I, MIR_new_reg_op(c->ctx, idx_i),
                                   MIR_new_reg_op(c->ctx, idx_reg)));

      MIR_reg_t res = ts_mir_new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, idx_i, 8)));
      return res;
    }
  }

  /* Runtime helper for env-owned vectors. */
  MIR_reg_t res = ts_mir_new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.vec_get_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.vec_get_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                    MIR_new_reg_op(c->ctx, idx_reg)));
  return res;
}

/* --- Assignment helpers --- */
