/* Function compilation and HOF MIR helpers. */

#include "turbo_script_mir_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ts_compiled_func_t *ts_find_hof_specialized_call(ts_mir_compiler_t *c,
                                                        exprtk_node_t *call_node,
                                                        int numeric_arg_indices[16],
                                                        size_t *numeric_arg_count);

MIR_reg_t ts_emit_compiled_func_call(ts_mir_compiler_t *c, ts_compiled_func_t *cf,
                                            size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t res = 0;
  size_t nops = 0;
  MIR_op_t ops[21];

  if (!c || !cf || cf->arg_count != argc) return 0;
  if (cf->has_closure_env && !c->ctx_reg) return 0;

  if (cf->has_closure_env) ts_emit_sync_to_env(c);

  res = ts_mir_new_temp_reg(c);
  ops[0] = MIR_new_ref_op(c->ctx, cf->proto);
  ops[1] = MIR_new_ref_op(c->ctx, cf->mir_func);
  ops[2] = MIR_new_reg_op(c->ctx, res);
  nops = 3;

  if (cf->has_closure_env) {
    ops[nops++] = MIR_new_reg_op(c->ctx, c->ctx_reg);
    if (c->closure_env_reg) {
      ops[nops++] = MIR_new_reg_op(c->ctx, c->closure_env_reg);
    } else {
      ops[nops++] = MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)&c->ts_ctx->env);
    }
  }

  for (size_t i = 0; i < argc; ++i) {
    ops[nops++] = MIR_new_reg_op(c->ctx, arg_regs[i]);
  }
  MIR_append_insn(c->ctx, c->func, MIR_new_insn_arr(c->ctx, MIR_CALL, nops, ops));
  return res;
}

MIR_reg_t ts_try_emit_hof_specialized_call(ts_mir_compiler_t *c,
                                                  exprtk_node_t *call_node) {
  int numeric_arg_indices[16];
  size_t numeric_arg_count = 0;
  ts_compiled_func_t *cf =
      ts_find_hof_specialized_call(c, call_node, numeric_arg_indices, &numeric_arg_count);
  if (!cf || cf->arg_count != numeric_arg_count) return 0;

  MIR_reg_t arg_regs[16];
  for (size_t i = 0; i < numeric_arg_count; ++i) {
    arg_regs[i] = ts_compile_expr(c, call_node->data.function.args[numeric_arg_indices[i]]);
  }

  return ts_emit_compiled_func_call(c, cf, numeric_arg_count, arg_regs);
}

static void ts_emit_closure_prologue(ts_mir_compiler_t *c) {
  if (!c->closure_analysis) return;

  ts_closure_analysis_t *analysis = (ts_closure_analysis_t *)c->closure_analysis;
  if (analysis->captured_count == 0) return;

  /* 为每个捕获变量生成加载代码 */
  for (size_t i = 0; i < analysis->captured_count; i++) {
    const char *var_name = analysis->captured_vars[i];

    /* 创建或获取变量寄存器 */
    MIR_reg_t var_reg = ts_mir_get_or_create_reg(c, var_name);

    /* 生成调用：var_reg = load_captured_var(ctx, var_name, closure_env) */
    MIR_append_insn(c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6,
        MIR_new_ref_op(c->ctx, c->ext.load_captured_proto),
        MIR_new_ref_op(c->ctx, c->ext.load_captured_import),
        MIR_new_reg_op(c->ctx, var_reg),                          // 返回值
        MIR_new_reg_op(c->ctx, c->ctx_reg),                       // ctx
        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)var_name),  // 变量名
        MIR_new_reg_op(c->ctx, c->closure_env_reg)));            // closure_env
  }
}

int ts_function_call_is_known_class(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  if (ts_mir_is_known_class_name(c, name)) return 1;
  exprtk_value_t value = exprtk_env_get(&c->ts_ctx->env, name);
  return value.type == EXPRTK_VAL_CLASS;
}

static int ts_name_matches_variable_param(const char *name, exprtk_node_t **params,
                                          size_t param_count) {
  if (!name || !params) return 0;
  for (size_t i = 0; i < param_count; ++i) {
    exprtk_node_t *param = params[i];
    if (!param) continue;
    if (param->type == EXPRTK_NODE_VARIABLE && param->data.variable.name &&
        strcmp(name, param->data.variable.name) == 0) {
      return 1;
    }
    if (param->type == EXPRTK_NODE_ASSIGNMENT && param->data.assignment.name &&
        strcmp(name, param->data.assignment.name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ts_node_calls_param_function(exprtk_node_t *node, exprtk_node_t **params,
                                        size_t param_count) {
  if (!node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_calls_param_function(node->data.block.statements[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_FLOW:
    return ts_node_calls_param_function(node->data.flow.value, params, param_count);
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    return ts_node_calls_param_function(node->data.assignment.value, params, param_count);
  case EXPRTK_NODE_BINARY_OP:
    return ts_node_calls_param_function(node->data.binary.left, params, param_count) ||
           ts_node_calls_param_function(node->data.binary.right, params, param_count);
  case EXPRTK_NODE_IF:
    return ts_node_calls_param_function(node->data.if_stmt.condition, params, param_count) ||
           ts_node_calls_param_function(node->data.if_stmt.if_branch, params, param_count) ||
           ts_node_calls_param_function(node->data.if_stmt.else_branch, params, param_count);
  case EXPRTK_NODE_WHILE:
    return ts_node_calls_param_function(node->data.while_loop.condition, params, param_count) ||
           ts_node_calls_param_function(node->data.while_loop.body, params, param_count);
  case EXPRTK_NODE_FOR:
    return ts_node_calls_param_function(node->data.for_loop.init, params, param_count) ||
           ts_node_calls_param_function(node->data.for_loop.condition, params, param_count) ||
           ts_node_calls_param_function(node->data.for_loop.post, params, param_count) ||
           ts_node_calls_param_function(node->data.for_loop.body, params, param_count);
  case EXPRTK_NODE_DO_WHILE:
    return ts_node_calls_param_function(node->data.do_while.body, params, param_count) ||
           ts_node_calls_param_function(node->data.do_while.condition, params, param_count);
  case EXPRTK_NODE_FOR_IN:
    return ts_node_calls_param_function(node->data.for_in.collection, params, param_count) ||
           ts_node_calls_param_function(node->data.for_in.body, params, param_count);
  case EXPRTK_NODE_FUNCTION_CALL:
    if (ts_name_matches_variable_param(node->data.function.name, params, param_count)) return 1;
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_calls_param_function(node->data.function.args[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_calls_param_function(node->data.member_call.object, params, param_count))
      return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_calls_param_function(node->data.member_call.args[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_ACCESS:
    return ts_node_calls_param_function(node->data.member_access.object, params, param_count);
  case EXPRTK_NODE_MEMBER_SET:
    return ts_node_calls_param_function(node->data.member_set.object, params, param_count) ||
           ts_node_calls_param_function(node->data.member_set.value, params, param_count);
  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i) {
      if (ts_node_calls_param_function(node->data.vector.elements[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      if (ts_node_calls_param_function(node->data.map_literal.values[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_INDEX:
    return ts_node_calls_param_function(node->data.index_access.array, params, param_count) ||
           ts_node_calls_param_function(node->data.index_access.index, params, param_count);
  case EXPRTK_NODE_SLICE:
    return ts_node_calls_param_function(node->data.slice.array, params, param_count) ||
           ts_node_calls_param_function(node->data.slice.start, params, param_count) ||
           ts_node_calls_param_function(node->data.slice.end, params, param_count);
  default:
    return 0;
  }
}

static int ts_node_uses_name_as_value(exprtk_node_t *node, const char *name) {
  if (!node || !name) return 0;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    return node->data.variable.name && strcmp(node->data.variable.name, name) == 0;

  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    if (node->data.assignment.name && strcmp(node->data.assignment.name, name) == 0) return 1;
    return ts_node_uses_name_as_value(node->data.assignment.value, name);

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_uses_name_as_value(node->data.block.statements[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_FLOW:
    return ts_node_uses_name_as_value(node->data.flow.value, name);

  case EXPRTK_NODE_BINARY_OP:
    return ts_node_uses_name_as_value(node->data.binary.left, name) ||
           ts_node_uses_name_as_value(node->data.binary.right, name);

  case EXPRTK_NODE_IF:
    return ts_node_uses_name_as_value(node->data.if_stmt.condition, name) ||
           ts_node_uses_name_as_value(node->data.if_stmt.if_branch, name) ||
           ts_node_uses_name_as_value(node->data.if_stmt.else_branch, name);

  case EXPRTK_NODE_WHILE:
    return ts_node_uses_name_as_value(node->data.while_loop.condition, name) ||
           ts_node_uses_name_as_value(node->data.while_loop.body, name);

  case EXPRTK_NODE_FOR:
    return ts_node_uses_name_as_value(node->data.for_loop.init, name) ||
           ts_node_uses_name_as_value(node->data.for_loop.condition, name) ||
           ts_node_uses_name_as_value(node->data.for_loop.post, name) ||
           ts_node_uses_name_as_value(node->data.for_loop.body, name);

  case EXPRTK_NODE_DO_WHILE:
    return ts_node_uses_name_as_value(node->data.do_while.body, name) ||
           ts_node_uses_name_as_value(node->data.do_while.condition, name);

  case EXPRTK_NODE_FOR_IN:
    if (node->data.for_in.var_name && strcmp(node->data.for_in.var_name, name) == 0) return 1;
    return ts_node_uses_name_as_value(node->data.for_in.collection, name) ||
           ts_node_uses_name_as_value(node->data.for_in.body, name);

  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_uses_name_as_value(node->data.function.args[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_uses_name_as_value(node->data.member_call.object, name)) return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_uses_name_as_value(node->data.member_call.args[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MEMBER_ACCESS:
    return ts_node_uses_name_as_value(node->data.member_access.object, name);

  case EXPRTK_NODE_MEMBER_SET:
    return ts_node_uses_name_as_value(node->data.member_set.object, name) ||
           ts_node_uses_name_as_value(node->data.member_set.value, name);

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i) {
      if (ts_node_uses_name_as_value(node->data.vector.elements[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      if (ts_node_uses_name_as_value(node->data.map_literal.values[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_INDEX:
    return ts_node_uses_name_as_value(node->data.index_access.array, name) ||
           ts_node_uses_name_as_value(node->data.index_access.index, name);

  case EXPRTK_NODE_SLICE:
    return ts_node_uses_name_as_value(node->data.slice.array, name) ||
           ts_node_uses_name_as_value(node->data.slice.start, name) ||
           ts_node_uses_name_as_value(node->data.slice.end, name);

  default:
    return 0;
  }
}

static int ts_node_needs_value_return_bridge(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return 0;
  if (node->type == EXPRTK_NODE_CLASS_DEF) return 1;
  if (node->type == EXPRTK_NODE_NEW) return 1;
  if (node->type == EXPRTK_NODE_THIS) return 1;
  if (node->type == EXPRTK_NODE_SUPER) return 1;
  if (node->type == EXPRTK_NODE_VARIABLE &&
      ts_function_call_is_known_class(c, node->data.variable.name)) {
    return 1;
  }

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_needs_value_return_bridge(c, node->data.block.statements[i])) return 1;
    }
    return 0;
  case EXPRTK_NODE_IF:
    return ts_node_needs_value_return_bridge(c, node->data.if_stmt.condition) ||
           ts_node_needs_value_return_bridge(c, node->data.if_stmt.if_branch) ||
           ts_node_needs_value_return_bridge(c, node->data.if_stmt.else_branch);
  case EXPRTK_NODE_WHILE:
    return ts_node_needs_value_return_bridge(c, node->data.while_loop.condition) ||
           ts_node_needs_value_return_bridge(c, node->data.while_loop.body);
  case EXPRTK_NODE_FOR:
    return ts_node_needs_value_return_bridge(c, node->data.for_loop.init) ||
           ts_node_needs_value_return_bridge(c, node->data.for_loop.condition) ||
           ts_node_needs_value_return_bridge(c, node->data.for_loop.post) ||
           ts_node_needs_value_return_bridge(c, node->data.for_loop.body);
  case EXPRTK_NODE_DO_WHILE:
    return ts_node_needs_value_return_bridge(c, node->data.do_while.body) ||
           ts_node_needs_value_return_bridge(c, node->data.do_while.condition);
  case EXPRTK_NODE_FOR_IN:
    return 1;
  case EXPRTK_NODE_SWITCH:
    return 1;
  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    return 1;
  case EXPRTK_NODE_TRY_CATCH:
    return 1;
  case EXPRTK_NODE_THROW:
    return 1;
  case EXPRTK_NODE_FLOW:
    return ts_node_needs_value_return_bridge(c, node->data.flow.value);
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    return ts_node_needs_value_return_bridge(c, node->data.assignment.value);
  case EXPRTK_NODE_BINARY_OP:
    return ts_node_needs_value_return_bridge(c, node->data.binary.left) ||
           ts_node_needs_value_return_bridge(c, node->data.binary.right);
  case EXPRTK_NODE_FUNCTION_CALL:
    if (ts_function_call_is_known_class(c, node->data.function.name)) return 1;
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_needs_value_return_bridge(c, node->data.function.args[i])) return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_needs_value_return_bridge(c, node->data.member_call.object)) return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_needs_value_return_bridge(c, node->data.member_call.args[i])) return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_SET:
    return ts_node_needs_value_return_bridge(c, node->data.member_set.object) ||
           ts_node_needs_value_return_bridge(c, node->data.member_set.value);
  case EXPRTK_NODE_MEMBER_ACCESS:
    return 1;
  case EXPRTK_NODE_INDEX:
    return ts_node_needs_value_return_bridge(c, node->data.index_access.array) ||
           ts_node_needs_value_return_bridge(c, node->data.index_access.index);
  case EXPRTK_NODE_INSTANCEOF:
    return ts_node_needs_value_return_bridge(c, node->data.instanceof_expr.object) ||
           ts_node_needs_value_return_bridge(c, node->data.instanceof_expr.class_expr);
  default:
    return 0;
  }
}

static int ts_func_alias_index(ts_mir_func_alias_t *aliases, size_t alias_count,
                               const char *param_name) {
  if (!aliases || !param_name) return -1;
  for (size_t i = 0; i < alias_count; ++i) {
    if (aliases[i].param_name && strcmp(aliases[i].param_name, param_name) == 0)
      return (int)i;
  }
  return -1;
}

static int ts_aliases_need_closure_env(ts_mir_compiler_t *c, ts_mir_func_alias_t *aliases,
                                       size_t alias_count) {
  if (!c || !aliases) return 0;
  for (size_t i = 0; i < alias_count; ++i) {
    ts_compiled_func_t *cf = ts_find_compiled_func(c, aliases[i].target_name);
    if (cf && cf->has_closure_env) return 1;
  }
  return 0;
}

/* Compile a script function as a separate MIR function.
 * Signature: double func_name(double arg0, double arg1, ...)
 * Or with closure: double func_name(void *ctx, void *closure_env, double arg0, double arg1, ...)
 * MUST be called when no other MIR function is open. */
static void ts_compile_script_func_with_aliases(ts_mir_compiler_t *c, const char *name,
                                                exprtk_node_t **arg_params, size_t arg_count,
                                                exprtk_node_t *body,
                                                ts_mir_func_alias_t *aliases,
                                                size_t alias_count) {
  if (ts_find_compiled_func(c, name)) return;
  if (arg_count > 16) return; /* sanity limit */
  if (alias_count == 0 && ts_node_calls_param_function(body, arg_params, arg_count)) return;
  for (size_t i = 0; i < alias_count; ++i) {
    if (ts_node_uses_name_as_value(body, aliases[i].param_name)) return;
  }
  if (ts_node_needs_value_return_bridge(c, body)) return;
  if (!ts_mir_reserve_compiled_func(c)) return;

  /* 闭包分析：检查是否捕获外部变量 */
  ts_closure_analysis_t *analysis =
      ts_analyze_closure(body, arg_params, arg_count, &c->ts_ctx->env,
                         c->ast_root);

  /* 如果不能 JIT（嵌套闭包、太多变量等），跳过编译 */
  if (analysis && !analysis->can_jit) {
    ts_closure_analysis_free(analysis);
    return;
  }
  int needs_closure_env = 1; /* ALWAYS need ctx for native/builtin calls */

  ts_mir_compile_frame_t frame = ts_mir_capture_frame(c);

  /* Reset compiler state for the new function */
  ts_mir_begin_isolated_compile(c);

  /* 保存闭包分析结果到编译器 */
  c->closure_analysis = (void *)analysis;
  c->func_aliases = aliases;
  c->func_alias_count = alias_count;

  /* Create unique function name */
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "%s_ts_%s", c->item_prefix, name);

  /* Build MIR function signature */
  MIR_type_t res_type = MIR_T_D;
  MIR_var_t mir_args[18];  // 最多 1 ctx + 1 closure_env + 16 参数
  size_t numeric_arg_count = 0;
  for (size_t i = 0; i < arg_count; i++) {
    const char *pname = arg_params[i]->data.variable.name;
    if (ts_func_alias_index(aliases, alias_count, pname) < 0) numeric_arg_count++;
  }
  uint32_t total_args = (uint32_t)numeric_arg_count;

  /* 如果需要闭包环境，添加 ctx 和 closure_env 作为前两个参数 */
  if (needs_closure_env) {
    mir_args[0].type = MIR_T_P;
    mir_args[0].name = "ctx";
    mir_args[0].size = 0;

    mir_args[1].type = MIR_T_P;
    mir_args[1].name = "closure_env";
    mir_args[1].size = 0;

    size_t out_i = 0;
    for (size_t i = 0; i < arg_count; i++) {
      const char *pname = arg_params[i]->data.variable.name;
      if (ts_func_alias_index(aliases, alias_count, pname) >= 0) continue;
      mir_args[out_i + 2].type = MIR_T_D;
      mir_args[out_i + 2].name = pname;
      mir_args[out_i + 2].size = 0;
      out_i++;
    }
    total_args = (uint32_t)(numeric_arg_count + 2);
  } else {
    size_t out_i = 0;
    for (size_t i = 0; i < arg_count; i++) {
      const char *pname = arg_params[i]->data.variable.name;
      if (ts_func_alias_index(aliases, alias_count, pname) >= 0) continue;
      mir_args[out_i].type = MIR_T_D;
      mir_args[out_i].name = pname;
      mir_args[out_i].size = 0;
      out_i++;
    }
  }

  MIR_item_t new_func =
      MIR_new_func_arr(c->ctx, func_name, 1, &res_type, total_args, mir_args);
  c->func = new_func;

  /* Create and register the call proto before compiling the body so recursive
   * calls can resolve to a complete MIR call target. */
  char proto_name[128];
  snprintf(proto_name, sizeof(proto_name), "%s_p_ts_%s", c->item_prefix, name);
  MIR_item_t proto =
      MIR_new_proto_arr(c->ctx, proto_name, 1, &res_type, total_args, mir_args);

  int idx = c->compiled_func_count++;
  c->compiled_funcs[idx].name = strdup(name);
  if (!c->compiled_funcs[idx].name) {
    ts_mir_fail(c, "JIT compile error: out of memory duplicating compiled function name '%s'",
                name ? name : "<unnamed>");
  }
  c->compiled_funcs[idx].mir_func = new_func;
  c->compiled_funcs[idx].proto = proto;
  c->compiled_funcs[idx].arg_count = numeric_arg_count;
  c->compiled_funcs[idx].has_closure_env = needs_closure_env;

  /* 如果有闭包环境，获取 ctx 和 closure_env 寄存器 */
  if (needs_closure_env) {
    c->ctx_reg = MIR_reg(c->ctx, "ctx", new_func->u.func);
    c->closure_env_reg = MIR_reg(c->ctx, "closure_env", new_func->u.func);
  }

  /* Map parameter names to their MIR registers */
  for (size_t i = 0; i < arg_count; i++) {
    const char *pname = arg_params[i]->data.variable.name;
    if (ts_func_alias_index(aliases, alias_count, pname) >= 0) continue;
    if (!ts_mir_bind_existing_reg(c, pname, MIR_reg(c->ctx, pname, new_func->u.func))) break;
  }

  /* 生成闭包 prologue：加载捕获的变量 */
  if (analysis && analysis->captured_count > 0) {
    ts_emit_closure_prologue(c);
  }

  /* Compile the function body */
  ts_compile_stmt(c, body);

  /* Default return 0.0 (in case body doesn't return) */
  MIR_reg_t ret_reg = ts_mir_new_temp_reg(c);
  MIR_append_insn(c->ctx, new_func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, ret_reg),
                               MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, new_func, MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_reg)));

  MIR_finish_func(c->ctx);

  /* 清理闭包分析结果 */
  c->closure_analysis = NULL;
  c->func_aliases = NULL;
  c->func_alias_count = 0;
  ts_closure_analysis_free(analysis);

  /* Restore parent compiler state */
  ts_mir_restore_frame(c, &frame);
}

void ts_compile_script_func(ts_mir_compiler_t *c, const char *name,
                                   exprtk_node_t **arg_params, size_t arg_count,
                                   exprtk_node_t *body) {
  ts_compile_script_func_with_aliases(c, name, arg_params, arg_count, body, NULL, 0);
}

void ts_prescan_class_names(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  if (node->type == EXPRTK_NODE_CLASS_DEF && node->data.class_def.name) {
    ts_mir_add_class_name(c, node->data.class_def.name);
  }

  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; ++i) {
      ts_prescan_class_names(c, node->data.block.statements[i]);
    }
  }
}

/* Pre-scan AST for function definitions and compile them as MIR functions.
 * Must be called BEFORE the main function is created. */
void ts_prescan_functions(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return;

  if (node->type == EXPRTK_NODE_FUNCTION_DEFINITION) {
    if (node->data.func_def.name && node->data.func_def.body &&
        node->data.func_def.arg_count <= 16) {
      int all_vars = 1;
      for (size_t i = 0; i < node->data.func_def.arg_count; i++) {
        if (node->data.func_def.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
          all_vars = 0;
          break;
        }
      }
      if (all_vars) {
        ts_compile_script_func(c, node->data.func_def.name, node->data.func_def.arg_params,
                               node->data.func_def.arg_count, node->data.func_def.body);
      }
    }
  }

  /* Recurse into blocks to find nested function definitions */
  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; i++) {
      ts_prescan_functions(c, node->data.block.statements[i]);
    }
  }
}

static exprtk_func_t *ts_find_script_func_in_env(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return NULL;
  for (exprtk_env_t *env = &c->ts_ctx->env; env; env = env->parent) {
    for (exprtk_func_t *f = env->funcs; f; f = f->next) {
      if (f->name && strcmp(f->name, name) == 0 && f->is_script) return f;
    }
  }
  return NULL;
}

static exprtk_node_t *ts_find_function_def_node(exprtk_node_t *node, const char *name) {
  if (!node || !name) return NULL;
  if (node->type == EXPRTK_NODE_FUNCTION_DEFINITION &&
      node->data.func_def.name && strcmp(node->data.func_def.name, name) == 0) {
    return node;
  }
  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; ++i) {
      exprtk_node_t *found = ts_find_function_def_node(node->data.block.statements[i], name);
      if (found) return found;
    }
  }
  return NULL;
}

static exprtk_node_t *ts_find_script_func_node(ts_mir_compiler_t *c, const char *name) {
  return c ? ts_find_function_def_node(c->ast_root, name) : NULL;
}

static void ts_sanitize_name_part(const char *src, char *dst, size_t dst_size) {
  size_t out = 0;
  if (!dst || dst_size == 0) return;
  if (!src) src = "anon";
  for (size_t i = 0; src[i] && out + 1 < dst_size; ++i) {
    char ch = src[i];
    int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') || ch == '_';
    dst[out++] = ok ? ch : '_';
  }
  dst[out] = '\0';
}

static int ts_build_hof_lambda_name(exprtk_node_t *node, char *out, size_t out_size) {
  if (!node || node->type != EXPRTK_NODE_FUNCTION_EXPRESSION || !out || out_size == 0) return 0;
  return snprintf(out, out_size, "__lambda_%zx", (size_t)(uintptr_t)node) > 0;
}

static int ts_function_expr_can_compile_for_hof(ts_mir_compiler_t *c, exprtk_node_t *node) {
  ts_closure_analysis_t *analysis = NULL;
  int ok = 0;

  if (!c || !node || node->type != EXPRTK_NODE_FUNCTION_EXPRESSION ||
      node->data.func_def.arg_count > 16 || !node->data.func_def.body) {
    return 0;
  }
  for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
    exprtk_node_t *param = node->data.func_def.arg_params[i];
    if (!param || param->type != EXPRTK_NODE_VARIABLE || !param->data.variable.name) return 0;
  }

  analysis = ts_analyze_closure(
      node->data.func_def.body, node->data.func_def.arg_params,
      node->data.func_def.arg_count, &c->ts_ctx->env, c->ast_root);
  ok = !analysis || analysis->can_jit;
  ts_closure_analysis_free(analysis);
  return ok;
}

static int ts_hof_resolve_function_arg(ts_mir_compiler_t *c, exprtk_node_t *arg,
                                       char lambda_name[64], const char **target_name) {
  if (!c || !arg || !target_name) return 0;

  if (arg->type == EXPRTK_NODE_VARIABLE && arg->data.variable.name) {
    ts_compiled_func_t *cf = ts_find_compiled_func(c, arg->data.variable.name);
    if (!cf) return 0;
    *target_name = arg->data.variable.name;
    return 1;
  }

  if (arg->type == EXPRTK_NODE_FUNCTION_EXPRESSION &&
      ts_function_expr_can_compile_for_hof(c, arg) &&
      ts_build_hof_lambda_name(arg, lambda_name, 64) &&
      ts_find_compiled_func(c, lambda_name)) {
    *target_name = lambda_name;
    return 1;
  }

  return 0;
}

static int ts_build_hof_specialization_key(const char *base_name,
                                           ts_mir_func_alias_t *aliases,
                                           size_t alias_count,
                                           char *out,
                                           size_t out_size) {
  char part[64];
  size_t used = 0;
  if (!base_name || !out || out_size == 0 || alias_count == 0) return 0;

  ts_sanitize_name_part(base_name, part, sizeof(part));
  used = (size_t)snprintf(out, out_size, "__hof_%s", part);
  if (used >= out_size) return 0;

  for (size_t i = 0; i < alias_count; ++i) {
    ts_sanitize_name_part(aliases[i].target_name, part, sizeof(part));
    used += (size_t)snprintf(out + used, out_size - used, "_%s", part);
    if (used >= out_size) return 0;
  }
  return 1;
}

static ts_compiled_func_t *ts_find_hof_specialized_call(ts_mir_compiler_t *c,
                                                        exprtk_node_t *call_node,
                                                        int numeric_arg_indices[16],
                                                        size_t *numeric_arg_count) {
  exprtk_func_t *callee = NULL;
  exprtk_node_t *callee_node = NULL;
  ts_mir_func_alias_t aliases[16];
  char lambda_names[16][64];
  size_t alias_count = 0;
  size_t numeric_count = 0;
  char key[256];

  if (!c || !call_node || call_node->type != EXPRTK_NODE_FUNCTION_CALL ||
      !call_node->data.function.name || !numeric_arg_count) {
    return NULL;
  }

  callee_node = ts_find_script_func_node(c, call_node->data.function.name);
  callee = callee_node ? NULL : ts_find_script_func_in_env(c, call_node->data.function.name);
  if (!callee_node && !callee) return NULL;

  size_t callee_arg_count =
      callee_node ? callee_node->data.func_def.arg_count : callee->data.script.arg_count;
  exprtk_node_t **callee_arg_params =
      callee_node ? callee_node->data.func_def.arg_params : callee->data.script.arg_params;
  exprtk_node_t *callee_body =
      callee_node ? callee_node->data.func_def.body : callee->data.script.body;

  if (!callee_body ||
      callee_arg_count != call_node->data.function.arg_count ||
      callee_arg_count > 16) {
    return NULL;
  }

  for (size_t i = 0; i < callee_arg_count; ++i) {
    exprtk_node_t *param = callee_arg_params[i];
    exprtk_node_t *arg = call_node->data.function.args[i];
    if (!param || param->type != EXPRTK_NODE_VARIABLE || !param->data.variable.name ||
        !arg) {
      return NULL;
    }

    if (ts_node_calls_param_function(callee_body, &param, 1)) {
      const char *target_name = NULL;
      if (!ts_hof_resolve_function_arg(c, arg, lambda_names[alias_count], &target_name) ||
          ts_node_uses_name_as_value(callee_body, param->data.variable.name)) {
        return NULL;
      }
      aliases[alias_count].param_name = param->data.variable.name;
      aliases[alias_count].target_name = target_name;
      alias_count++;
    } else {
      numeric_arg_indices[numeric_count++] = (int)i;
    }
  }

  if (alias_count == 0) return NULL;
  if (!ts_build_hof_specialization_key(call_node->data.function.name, aliases, alias_count,
                                       key, sizeof(key))) {
    return NULL;
  }

  *numeric_arg_count = numeric_count;
  return ts_find_compiled_func(c, key);
}

void ts_prescan_hof_specializations(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  if (node->type == EXPRTK_NODE_FUNCTION_CALL && node->data.function.name) {
    exprtk_node_t *callee_node = ts_find_script_func_node(c, node->data.function.name);
    exprtk_func_t *callee =
        callee_node ? NULL : ts_find_script_func_in_env(c, node->data.function.name);
    ts_mir_func_alias_t aliases[16];
    char lambda_names[16][64];
    size_t alias_count = 0;
    char key[256];

    if (callee_node || callee) {
      size_t callee_arg_count =
          callee_node ? callee_node->data.func_def.arg_count : callee->data.script.arg_count;
      exprtk_node_t **callee_arg_params =
          callee_node ? callee_node->data.func_def.arg_params : callee->data.script.arg_params;
      exprtk_node_t *callee_body =
          callee_node ? callee_node->data.func_def.body : callee->data.script.body;
      int all_vars = 1;
      if (!callee_body || callee_arg_count != node->data.function.arg_count ||
          callee_arg_count > 16) {
        all_vars = 0;
      }
      for (size_t i = 0; all_vars && i < callee_arg_count; ++i) {
        exprtk_node_t *param = callee_arg_params[i];
        exprtk_node_t *arg = node->data.function.args[i];
        if (!param || param->type != EXPRTK_NODE_VARIABLE || !param->data.variable.name) {
          all_vars = 0;
          break;
        }
        if (ts_node_calls_param_function(callee_body, &param, 1)) {
          const char *target_name = NULL;
          if (arg && arg->type == EXPRTK_NODE_FUNCTION_EXPRESSION &&
              ts_function_expr_can_compile_for_hof(c, arg) &&
              ts_build_hof_lambda_name(arg, lambda_names[alias_count],
                                       sizeof(lambda_names[alias_count])) &&
              !ts_find_compiled_func(c, lambda_names[alias_count])) {
            ts_compile_script_func(c, lambda_names[alias_count],
                                   arg->data.func_def.arg_params,
                                   arg->data.func_def.arg_count,
                                   arg->data.func_def.body);
          }
          if (!ts_hof_resolve_function_arg(c, arg, lambda_names[alias_count], &target_name) ||
              ts_node_uses_name_as_value(callee_body, param->data.variable.name)) {
            all_vars = 0;
            break;
          }
          aliases[alias_count].param_name = param->data.variable.name;
          aliases[alias_count].target_name = target_name;
          alias_count++;
        }
      }

      if (all_vars && alias_count > 0 &&
          ts_build_hof_specialization_key(node->data.function.name, aliases, alias_count,
                                          key, sizeof(key)) &&
          !ts_find_compiled_func(c, key)) {
        ts_compile_script_func_with_aliases(c, key, callee_arg_params, callee_arg_count,
                                            callee_body,
                                            aliases, alias_count);
      }
    }
  }

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i)
      ts_prescan_hof_specializations(c, node->data.block.statements[i]);
    break;
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    ts_prescan_hof_specializations(c, node->data.assignment.value);
    break;
  case EXPRTK_NODE_FLOW:
    ts_prescan_hof_specializations(c, node->data.flow.value);
    break;
  case EXPRTK_NODE_BINARY_OP:
    ts_prescan_hof_specializations(c, node->data.binary.left);
    ts_prescan_hof_specializations(c, node->data.binary.right);
    break;
  case EXPRTK_NODE_IF:
    ts_prescan_hof_specializations(c, node->data.if_stmt.condition);
    ts_prescan_hof_specializations(c, node->data.if_stmt.if_branch);
    ts_prescan_hof_specializations(c, node->data.if_stmt.else_branch);
    break;
  case EXPRTK_NODE_WHILE:
    ts_prescan_hof_specializations(c, node->data.while_loop.condition);
    ts_prescan_hof_specializations(c, node->data.while_loop.body);
    break;
  case EXPRTK_NODE_FOR:
    ts_prescan_hof_specializations(c, node->data.for_loop.init);
    ts_prescan_hof_specializations(c, node->data.for_loop.condition);
    ts_prescan_hof_specializations(c, node->data.for_loop.post);
    ts_prescan_hof_specializations(c, node->data.for_loop.body);
    break;
  case EXPRTK_NODE_DO_WHILE:
    ts_prescan_hof_specializations(c, node->data.do_while.body);
    ts_prescan_hof_specializations(c, node->data.do_while.condition);
    break;
  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i)
      ts_prescan_hof_specializations(c, node->data.function.args[i]);
    break;
  default:
    break;
  }
}
