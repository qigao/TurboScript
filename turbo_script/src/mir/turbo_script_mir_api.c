/* Public MIR compile and execution API. */

#include "../host/turbo_script_host_internal.h"
#include "exprtk_grammar.h"
#include "turbo_script_mir_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/time.h>
#endif

struct ts_mir_artifact_s {
  MIR_context_t ctx;
  MIR_module_t module;
  MIR_item_t initializer;
  void *initializer_address;
  MIR_item_t *export_items;
  MIR_item_t *numeric_export_wrappers;
  MIR_item_t *host_export_wrappers;
  uint32_t *export_arities;
  uint8_t *native_numeric_exports;
  size_t *native_step_costs;
  void **numeric_export_addresses;
  void **host_export_addresses;
  size_t export_count;
  /* Borrowed from the module-retained context for the artifact lifetime. */
  turbo_script_ctx_t *registry_owner_ctx;
  int gen_initialized;
};

enum { TS_MIR_NATIVE_CALL_OVERHEAD_STEPS = 2 };

static ts_compiled_func_t *ts_mir_find_artifact_function(ts_mir_compiler_t *compiler,
                                                         const char *name) {
  for (int i = 0; i < compiler->compiled_func_count; ++i) {
    if (compiler->compiled_funcs[i].name && strcmp(compiler->compiled_funcs[i].name, name) == 0)
      return &compiler->compiled_funcs[i];
  }
  return NULL;
}

static exprtk_node_t *ts_mir_native_numeric_expression(exprtk_node_t *function_node) {
  exprtk_node_t *statement;
  if (!function_node || function_node->type != EXPRTK_NODE_FUNCTION_DEFINITION) return NULL;
  statement = function_node->data.func_def.body;
  if (statement && statement->type == EXPRTK_NODE_BLOCK) {
    if (!statement->data.block.statements || statement->data.block.count != 1) return NULL;
    statement = statement->data.block.statements[0];
  }
  if (!statement || statement->type != EXPRTK_NODE_FLOW ||
      statement->data.flow.type != exprtk_TOKEN_RETURN)
    return NULL;
  return statement->data.flow.value;
}

/* Single AST-to-MIR lowering core. Callers retain ownership of the supplied
 * MIR context/module and decide how long linked code remains alive. */
static int ts_mir_lower_owned_module(MIR_context_t mir_ctx, MIR_module_t module,
                                     turbo_script_ctx_t *compile_ctx, exprtk_node_t *ast,
                                     const char *prefix, const ts_mir_lowering_policy_t *policy,
                                     const ts_host_export_table_t *exports,
                                     MIR_item_t *out_initializer, MIR_item_t *out_export_items,
                                     MIR_item_t *out_numeric_export_wrappers,
                                     MIR_item_t *out_host_export_wrappers) {
  ts_mir_compiler_t compiler = {0};
  const exprtk_node_t **metadata_nodes = NULL;
  size_t export_count = exports ? vec_size(&exports->entries) : 0;
  int success = 0;
  int module_finished = 0;
  if (!mir_ctx || !module || !compile_ctx || !ast || !prefix || !policy || !out_initializer)
    return -1;
  if ((policy->host_slots == TS_MIR_HOST_SLOTS_FROZEN &&
       (!policy->registry_owner_ctx || policy->registry_owner_ctx->active_host_modules == 0)) ||
      (policy->host_slots == TS_MIR_HOST_SLOTS_DISABLED && policy->registry_owner_ctx) ||
      (policy->host_slots != TS_MIR_HOST_SLOTS_FROZEN &&
       policy->host_slots != TS_MIR_HOST_SLOTS_DISABLED))
    return -1;
  if (export_count &&
      (!out_export_items || !out_numeric_export_wrappers || !out_host_export_wrappers))
    return -1;
  *out_initializer = NULL;
  if (export_count) {
    metadata_nodes = (const exprtk_node_t **)calloc(export_count, sizeof(*metadata_nodes));
    if (!metadata_nodes) return -1;
  }
  compiler.ctx = mir_ctx;
  compiler.module = module;
  compiler.ts_ctx = compile_ctx;
  compiler.lowering_policy = *policy;
  compiler.ast_root = ast;
  compiler.metadata_nodes = metadata_nodes;
  compiler.metadata_node_count = export_count;
  snprintf(compiler.item_prefix, sizeof(compiler.item_prefix), "%s", prefix);
  for (size_t i = 0; i < export_count; ++i) {
    const ts_host_export_entry_t *entry =
        (const ts_host_export_entry_t *)vec_at_const(&exports->entries, i);
    if (!entry) goto done;
    metadata_nodes[i] = entry->declaration_node;
  }
  ts_mir_init_externals(&compiler);
  ts_prescan_class_names(&compiler, ast);
  for (exprtk_func_t *f = compile_ctx->env.funcs; f; f = f->next) {
    if (f->is_script && f->data.script.body && f->data.script.arg_count <= 16) {
      int all_vars = 1;
      for (size_t i = 0; i < f->data.script.arg_count; ++i) {
        if (f->data.script.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
          all_vars = 0;
          break;
        }
      }
      if (all_vars)
        ts_compile_script_func(&compiler, f->name, f->data.script.arg_params,
                               f->data.script.arg_count, f->data.script.body);
    }
  }
  ts_prescan_functions(&compiler, ast);
  ts_prescan_hof_specializations(&compiler, ast);

  MIR_type_t result_type = MIR_T_D;
  MIR_var_t initializer_args[1] = {{MIR_T_P, "ctx_ptr", 0}};
  char initializer_name[128];
  snprintf(initializer_name, sizeof(initializer_name), "%s_main", prefix);
  MIR_item_t initializer =
      MIR_new_func_arr(mir_ctx, initializer_name, 1, &result_type, 1, initializer_args);
  compiler.func = initializer;
  compiler.ctx_reg = MIR_reg(mir_ctx, "ctx_ptr", initializer->u.func);
  ts_prescan_variables(&compiler, ast);
  ts_compile_stmt(&compiler, ast);
  ts_emit_var_prologue(&compiler);
  ts_emit_vec_prologue(&compiler);
  ts_emit_map_prologue(&compiler);
  ts_emit_var_epilogue(&compiler);
  char ret_name[32];
  snprintf(ret_name, sizeof(ret_name), "_t%d", compiler.tmp_count++);
  MIR_reg_t ret = MIR_new_func_reg(mir_ctx, initializer->u.func, MIR_T_D, ret_name);
  MIR_append_insn(mir_ctx, initializer,
                  MIR_new_insn(mir_ctx, MIR_DMOV, MIR_new_reg_op(mir_ctx, ret),
                               MIR_new_double_op(mir_ctx, 0.0)));
  MIR_append_insn(mir_ctx, initializer, MIR_new_ret_insn(mir_ctx, 1, MIR_new_reg_op(mir_ctx, ret)));
  MIR_finish_func(mir_ctx);
  if (compiler.failed) goto done;

  /* MIR virtual integer registers are always i64.  The generated wrapper's
   * zero/non-zero result remains ABI-compatible with the public C int view. */
  MIR_type_t host_result_type = MIR_T_I64;
  MIR_var_t host_bridge_args[5] = {{MIR_T_P, "ctx", 0},
                                   {MIR_T_U64, "export_index", 0},
                                   {MIR_T_P, "args", 0},
                                   {MIR_T_U64, "arg_count", 0},
                                   {MIR_T_P, "out_value", 0}};
  MIR_item_t host_bridge_proto = NULL;
  MIR_item_t host_bridge_import = NULL;
  char host_bridge_proto_name[128];
  if (export_count != 0) {
    snprintf(host_bridge_proto_name, sizeof(host_bridge_proto_name), "%s_p_host_export_call",
             prefix);
    host_bridge_proto = MIR_new_proto_arr(mir_ctx, host_bridge_proto_name, 1, &host_result_type, 5,
                                          host_bridge_args);
    host_bridge_import = MIR_new_import(mir_ctx, "ts_mir_host_export_call");
  }

  for (size_t i = 0; i < export_count; ++i) {
    const ts_host_export_entry_t *entry =
        (const ts_host_export_entry_t *)vec_at_const(&exports->entries, i);
    ts_compiled_func_t *compiled =
        entry ? ts_mir_find_artifact_function(&compiler, entry->name) : NULL;
    if (!entry || entry->arity > 16) goto done;
    if (compiled) {
      MIR_var_t numeric_wrapper_args[3] = {
          {MIR_T_P, "ctx", 0}, {MIR_T_P, "closure_env", 0}, {MIR_T_P, "argv", 0}};
      char numeric_wrapper_name[64];
      MIR_item_t numeric_wrapper;
      MIR_reg_t numeric_ctx;
      MIR_reg_t numeric_env;
      MIR_reg_t numeric_argv;
      MIR_reg_t numeric_ret;
      MIR_op_t numeric_call_ops[21];
      if (compiled->arg_count != entry->arity) goto done;
      out_export_items[i] = compiled->mir_func;
      snprintf(numeric_wrapper_name, sizeof(numeric_wrapper_name), "%s_numeric_wrapper_%zu", prefix,
               i);
      numeric_wrapper =
          MIR_new_func_arr(mir_ctx, numeric_wrapper_name, 1, &result_type, 3, numeric_wrapper_args);
      numeric_ctx = MIR_reg(mir_ctx, "ctx", numeric_wrapper->u.func);
      numeric_env = MIR_reg(mir_ctx, "closure_env", numeric_wrapper->u.func);
      numeric_argv = MIR_reg(mir_ctx, "argv", numeric_wrapper->u.func);
      numeric_ret = MIR_new_func_reg(mir_ctx, numeric_wrapper->u.func, MIR_T_D, "result");
      numeric_call_ops[0] = MIR_new_ref_op(mir_ctx, compiled->proto);
      numeric_call_ops[1] = MIR_new_ref_op(mir_ctx, compiled->mir_func);
      numeric_call_ops[2] = MIR_new_reg_op(mir_ctx, numeric_ret);
      numeric_call_ops[3] = MIR_new_reg_op(mir_ctx, numeric_ctx);
      numeric_call_ops[4] = MIR_new_reg_op(mir_ctx, numeric_env);
      for (size_t arg = 0; arg < entry->arity; ++arg) {
        char value_name[24];
        MIR_reg_t value_reg;
        snprintf(value_name, sizeof(value_name), "arg_%zu", arg);
        value_reg = MIR_new_func_reg(mir_ctx, numeric_wrapper->u.func, MIR_T_D, value_name);
        MIR_append_insn(
            mir_ctx, numeric_wrapper,
            MIR_new_insn(mir_ctx, MIR_DMOV, MIR_new_reg_op(mir_ctx, value_reg),
                         MIR_new_mem_op(mir_ctx, MIR_T_D, (MIR_disp_t)(arg * sizeof(double)),
                                        numeric_argv, 0, 1)));
        numeric_call_ops[arg + 5] = MIR_new_reg_op(mir_ctx, value_reg);
      }
      MIR_append_insn(mir_ctx, numeric_wrapper,
                      MIR_new_insn_arr(mir_ctx, MIR_CALL, entry->arity + 5, numeric_call_ops));
      MIR_append_insn(mir_ctx, numeric_wrapper,
                      MIR_new_ret_insn(mir_ctx, 1, MIR_new_reg_op(mir_ctx, numeric_ret)));
      MIR_finish_func(mir_ctx);
      out_numeric_export_wrappers[i] = numeric_wrapper;
    }

    MIR_var_t host_wrapper_args[4] = {{MIR_T_P, "ctx", 0},
                                      {MIR_T_P, "args", 0},
                                      {MIR_T_U64, "arg_count", 0},
                                      {MIR_T_P, "out_value", 0}};
    char host_wrapper_name[64];
    snprintf(host_wrapper_name, sizeof(host_wrapper_name), "%s_host_wrapper_%zu", prefix, i);
    MIR_item_t host_wrapper =
        MIR_new_func_arr(mir_ctx, host_wrapper_name, 1, &host_result_type, 4, host_wrapper_args);
    MIR_reg_t host_ctx = MIR_reg(mir_ctx, "ctx", host_wrapper->u.func);
    MIR_reg_t host_args = MIR_reg(mir_ctx, "args", host_wrapper->u.func);
    MIR_reg_t host_arg_count = MIR_reg(mir_ctx, "arg_count", host_wrapper->u.func);
    MIR_reg_t host_out = MIR_reg(mir_ctx, "out_value", host_wrapper->u.func);
    MIR_reg_t host_ret = MIR_new_func_reg(mir_ctx, host_wrapper->u.func, MIR_T_I64, "result");
    MIR_append_insn(
        mir_ctx, host_wrapper,
        MIR_new_call_insn(mir_ctx, 8, MIR_new_ref_op(mir_ctx, host_bridge_proto),
                          MIR_new_ref_op(mir_ctx, host_bridge_import),
                          MIR_new_reg_op(mir_ctx, host_ret), MIR_new_reg_op(mir_ctx, host_ctx),
                          MIR_new_uint_op(mir_ctx, (uint64_t)i), MIR_new_reg_op(mir_ctx, host_args),
                          MIR_new_reg_op(mir_ctx, host_arg_count),
                          MIR_new_reg_op(mir_ctx, host_out)));
    MIR_append_insn(mir_ctx, host_wrapper,
                    MIR_new_ret_insn(mir_ctx, 1, MIR_new_reg_op(mir_ctx, host_ret)));
    MIR_finish_func(mir_ctx);
    out_host_export_wrappers[i] = host_wrapper;
  }
  MIR_finish_module(mir_ctx);
  module_finished = 1;
  *out_initializer = initializer;
  success = 1;
done:
  if (!module_finished) MIR_finish_module(mir_ctx);
  free(metadata_nodes);
  ts_mir_destroy_compiler_storage(&compiler);
  return success ? 0 : -1;
}

int ts_mir_artifact_compile(turbo_script_ctx_t *compile_ctx, exprtk_node_t *ast,
                            const ts_host_export_table_t *exports,
                            ts_mir_artifact_t **out_artifact) {
  ts_mir_artifact_t *artifact = NULL;
  size_t export_count;
  const ts_mir_lowering_policy_t policy = {TS_MIR_HOST_SLOTS_FROZEN, compile_ctx};
  int success = 0;
  if (!compile_ctx || !ast || !exports || !out_artifact) return -1;
  *out_artifact = NULL;
  export_count = vec_size(&exports->entries);
  artifact = (ts_mir_artifact_t *)calloc(1, sizeof(*artifact));
  if (!artifact) goto done;
  if (export_count) {
    artifact->export_items = (MIR_item_t *)calloc(export_count, sizeof(MIR_item_t));
    artifact->numeric_export_wrappers = (MIR_item_t *)calloc(export_count, sizeof(MIR_item_t));
    artifact->host_export_wrappers = (MIR_item_t *)calloc(export_count, sizeof(MIR_item_t));
    artifact->numeric_export_addresses = (void **)calloc(export_count, sizeof(void *));
    artifact->host_export_addresses = (void **)calloc(export_count, sizeof(void *));
    artifact->export_arities = (uint32_t *)calloc(export_count, sizeof(uint32_t));
    artifact->native_numeric_exports = (uint8_t *)calloc(export_count, sizeof(uint8_t));
    artifact->native_step_costs = (size_t *)calloc(export_count, sizeof(size_t));
    if (!artifact->export_items || !artifact->numeric_export_wrappers ||
        !artifact->host_export_wrappers || !artifact->numeric_export_addresses ||
        !artifact->host_export_addresses || !artifact->export_arities ||
        !artifact->native_numeric_exports || !artifact->native_step_costs)
      goto done;
  }
  artifact->export_count = export_count;
  artifact->registry_owner_ctx = compile_ctx;
  artifact->ctx = MIR_init();
  if (!artifact->ctx) goto done;
  artifact->module = MIR_new_module(artifact->ctx, "ts_host_module");
  if (!artifact->module) goto done;
  if (ts_mir_lower_owned_module(artifact->ctx, artifact->module, compile_ctx, ast, "ts_host",
                                &policy, exports, &artifact->initializer, artifact->export_items,
                                artifact->numeric_export_wrappers,
                                artifact->host_export_wrappers) != 0)
    goto done;
  for (size_t i = 0; i < export_count; ++i) {
    const ts_host_export_entry_t *entry =
        (const ts_host_export_entry_t *)vec_at_const(&exports->entries, i);
    if (!entry) goto done;
    artifact->export_arities[i] = entry->arity;
    artifact->native_numeric_exports[i] =
        (uint8_t)(artifact->numeric_export_wrappers[i] != NULL &&
                  ts_mir_function_is_native_numeric_export(entry->function_node));
    if (artifact->native_numeric_exports[i]) {
      exprtk_node_t *expression = ts_mir_native_numeric_expression(entry->function_node);
      size_t expression_steps;
      if (!expression) goto done;
      expression_steps = exprtk_node_count(expression);
      if (expression_steps > SIZE_MAX - TS_MIR_NATIVE_CALL_OVERHEAD_STEPS) goto done;
      /* Include the generated wrapper's dispatch and return units so native
       * exports consume the same per-call budget in interpreter and JIT mode. */
      artifact->native_step_costs[i] = expression_steps + TS_MIR_NATIVE_CALL_OVERHEAD_STEPS;
    }
  }
  MIR_load_module(artifact->ctx, artifact->module);
  ts_mir_load_externals(artifact->ctx);
  MIR_load_external(artifact->ctx, "ts_mir_host_export_call", (void *)ts_mir_host_export_call);
  MIR_link(artifact->ctx, MIR_set_interp_interface, NULL);
  MIR_gen_init(artifact->ctx);
  artifact->gen_initialized = 1;
  MIR_link(artifact->ctx, MIR_set_gen_interface, NULL);
  artifact->initializer_address = MIR_gen(artifact->ctx, artifact->initializer);
  if (!artifact->initializer_address) goto done;
  for (size_t i = 0; i < export_count; ++i) {
    if (artifact->numeric_export_wrappers[i])
      artifact->numeric_export_addresses[i] =
          MIR_gen(artifact->ctx, artifact->numeric_export_wrappers[i]);
    artifact->host_export_addresses[i] = MIR_gen(artifact->ctx, artifact->host_export_wrappers[i]);
    if ((artifact->numeric_export_wrappers[i] && !artifact->numeric_export_addresses[i]) ||
        !artifact->host_export_addresses[i])
      goto done;
  }
  success = 1;

done:
  if (!success) {
    if (compile_ctx && compile_ctx->error_code == TURBO_SCRIPT_ERROR_NONE) {
      compile_ctx->error_code = artifact ? TURBO_SCRIPT_ERROR_JIT : TURBO_SCRIPT_ERROR_OOM;
      snprintf(compile_ctx->error_msg, sizeof(compile_ctx->error_msg),
               "Host module MIR artifact compilation failed");
    }
    ts_mir_artifact_destroy(artifact);
    return -1;
  }
  *out_artifact = artifact;
  return 0;
}

void ts_mir_artifact_destroy(ts_mir_artifact_t *artifact) {
  if (!artifact) return;
  if (artifact->ctx) {
    if (artifact->gen_initialized) MIR_gen_finish(artifact->ctx);
    MIR_finish(artifact->ctx);
  }
  free(artifact->export_items);
  free(artifact->numeric_export_wrappers);
  free(artifact->host_export_wrappers);
  free(artifact->numeric_export_addresses);
  free(artifact->host_export_addresses);
  free(artifact->export_arities);
  free(artifact->native_numeric_exports);
  free(artifact->native_step_costs);
  free(artifact);
}

int ts_mir_artifact_execute_numeric(ts_mir_artifact_t *artifact, turbo_script_ctx_t *runtime_ctx,
                                    size_t export_index, int use_jit, const double *args,
                                    size_t arg_count, double *out_result) {
  MIR_val_t mir_args[18] = {0};
  MIR_val_t result = {0};
  if (!artifact || !runtime_ctx || export_index >= artifact->export_count ||
      !artifact->registry_owner_ctx || arg_count > 16 ||
      arg_count != artifact->export_arities[export_index] || (!args && arg_count) || !out_result ||
      !artifact->export_items[export_index] || !artifact->numeric_export_wrappers[export_index])
    return -1;
  runtime_ctx->env.aborted = 0;
  runtime_ctx->env.flow = exprtk_FLOW_NORMAL;
  runtime_ctx->env.error_msg[0] = '\0';
  mir_args[0].a = runtime_ctx;
  mir_args[1].a = &runtime_ctx->env;
  for (size_t i = 0; i < arg_count; ++i)
    mir_args[i + 2].d = args[i];
  if (!use_jit) {
    MIR_interp_arr(artifact->ctx, artifact->export_items[export_index], &result, arg_count + 2,
                   mir_args);
    if (runtime_ctx->env.aborted || runtime_ctx->env.flow == exprtk_FLOW_THROW) return -1;
    *out_result = result.d;
    return 0;
  }
  if (!artifact->numeric_export_addresses[export_index]) return -1;
  result.d = ((double (*)(void *, void *,
                          const double *))artifact->numeric_export_addresses[export_index])(
      runtime_ctx, &runtime_ctx->env, args);
  if (runtime_ctx->env.aborted || runtime_ctx->env.flow == exprtk_FLOW_THROW) return -1;
  *out_result = result.d;
  return 0;
}

static int ts_mir_artifact_call_begin(ts_mir_artifact_t *artifact, turbo_script_ctx_t *runtime_ctx,
                                      size_t export_index, const exprtk_value_t *args,
                                      size_t arg_count, exprtk_value_t *out_value) {
  if (out_value) {
    memset(out_value, 0, sizeof(*out_value));
    out_value->type = EXPRTK_VAL_NULL;
  }
  if (!artifact || !runtime_ctx || export_index >= artifact->export_count ||
      !artifact->registry_owner_ctx || arg_count > 16 ||
      arg_count != artifact->export_arities[export_index] || (!args && arg_count) || !out_value)
    return -1;
  runtime_ctx->env.aborted = 0;
  runtime_ctx->env.flow = exprtk_FLOW_NORMAL;
  runtime_ctx->env.curr_recursion = 0;
  runtime_ctx->env.curr_nodes = 0;
  runtime_ctx->env.curr_loop_iterations = 0;
  runtime_ctx->env.last_line = 0;
  runtime_ctx->env.last_column = 0;
  runtime_ctx->env.error_line = 0;
  runtime_ctx->env.error_column = 0;
  runtime_ctx->env.error_msg[0] = '\0';
  exprtk_value_destroy(&runtime_ctx->env.error_value);
  runtime_ctx->env.error_value.type = EXPRTK_VAL_NULL;
  runtime_ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  runtime_ctx->error_msg[0] = '\0';
  return 0;
}

int ts_mir_artifact_export_is_native_numeric(const ts_mir_artifact_t *artifact,
                                             size_t export_index) {
  return artifact && export_index < artifact->export_count && artifact->native_numeric_exports &&
         artifact->native_numeric_exports[export_index] != 0;
}

static int ts_mir_artifact_try_native_numeric(ts_mir_artifact_t *artifact,
                                              turbo_script_ctx_t *runtime_ctx, size_t export_index,
                                              int use_jit, const exprtk_value_t *args,
                                              size_t arg_count, exprtk_value_t *out_value) {
  double numeric_args[16];
  double result;
  size_t frame_bytes;
  int frame_entered = 0;
  if (!ts_mir_artifact_export_is_native_numeric(artifact, export_index)) return 0;
  for (size_t i = 0; i < arg_count; ++i) {
    if (args[i].type != EXPRTK_VAL_NUMBER) return 0;
    numeric_args[i] = args[i].data.number;
  }
  frame_bytes = sizeof(exprtk_env_t) + arg_count * sizeof(exprtk_value_t);
  if (runtime_ctx->env.safe_point) {
    if (runtime_ctx->env.safe_point(runtime_ctx->env.safe_point_user_data,
                                    EXPRTK_SAFE_POINT_FUNCTION_ENTER, frame_bytes) != 0) {
      runtime_ctx->env.aborted = 1;
      return -1;
    }
    frame_entered = 1;
    if (runtime_ctx->env.safe_point(runtime_ctx->env.safe_point_user_data, EXPRTK_SAFE_POINT_STEP,
                                    artifact->native_step_costs[export_index]) != 0) {
      runtime_ctx->env.aborted = 1;
      (void)runtime_ctx->env.safe_point(runtime_ctx->env.safe_point_user_data,
                                        EXPRTK_SAFE_POINT_FUNCTION_LEAVE, frame_bytes);
      return -1;
    }
  }
  if (ts_mir_artifact_execute_numeric(artifact, runtime_ctx, export_index, use_jit,
                                      arg_count != 0 ? numeric_args : NULL, arg_count,
                                      &result) != 0) {
    if (frame_entered)
      (void)runtime_ctx->env.safe_point(runtime_ctx->env.safe_point_user_data,
                                        EXPRTK_SAFE_POINT_FUNCTION_LEAVE, frame_bytes);
    return -1;
  }
  if (frame_entered &&
      runtime_ctx->env.safe_point(runtime_ctx->env.safe_point_user_data,
                                  EXPRTK_SAFE_POINT_FUNCTION_LEAVE, frame_bytes) != 0) {
    runtime_ctx->env.aborted = 1;
    return -1;
  }
  *out_value = exprtk_val_num(result);
  return 1;
}

int ts_mir_artifact_call_interp(ts_mir_artifact_t *artifact, turbo_script_ctx_t *runtime_ctx,
                                size_t export_index, const exprtk_value_t *args, size_t arg_count,
                                exprtk_value_t *out_value) {
  MIR_val_t mir_args[4] = {0};
  MIR_val_t result = {0};
  int native_status;
  if (ts_mir_artifact_call_begin(artifact, runtime_ctx, export_index, args, arg_count, out_value) !=
      0)
    return -1;
  native_status = ts_mir_artifact_try_native_numeric(artifact, runtime_ctx, export_index, 0, args,
                                                     arg_count, out_value);
  if (native_status != 0) return native_status > 0 ? 0 : -1;
  mir_args[0].a = runtime_ctx;
  mir_args[1].a = (void *)args;
  mir_args[2].u = arg_count;
  mir_args[3].a = out_value;
  MIR_interp_arr(artifact->ctx, artifact->host_export_wrappers[export_index], &result, 4, mir_args);
  if (result.i != 0 || runtime_ctx->env.aborted || runtime_ctx->env.flow == exprtk_FLOW_THROW) {
    exprtk_value_destroy(out_value);
    return -1;
  }
  return 0;
}

int ts_mir_artifact_call_jit(ts_mir_artifact_t *artifact, turbo_script_ctx_t *runtime_ctx,
                             size_t export_index, const exprtk_value_t *args, size_t arg_count,
                             exprtk_value_t *out_value) {
  ts_mir_host_export_fn function;
  int native_status;
  if (ts_mir_artifact_call_begin(artifact, runtime_ctx, export_index, args, arg_count, out_value) !=
          0 ||
      !artifact->host_export_addresses[export_index])
    return -1;
  native_status = ts_mir_artifact_try_native_numeric(artifact, runtime_ctx, export_index, 1, args,
                                                     arg_count, out_value);
  if (native_status != 0) return native_status > 0 ? 0 : -1;
  function = (ts_mir_host_export_fn)artifact->host_export_addresses[export_index];
  if (function(runtime_ctx, args, arg_count, out_value) != 0 || runtime_ctx->env.aborted ||
      runtime_ctx->env.flow == exprtk_FLOW_THROW) {
    exprtk_value_destroy(out_value);
    return -1;
  }
  return 0;
}

int ts_mir_artifact_execute_initializer(ts_mir_artifact_t *artifact,
                                        turbo_script_ctx_t *runtime_ctx, int use_jit) {
  MIR_val_t argument = {0};
  MIR_val_t result = {0};
  if (!artifact || !runtime_ctx) return -1;
  runtime_ctx->env.aborted = 0;
  runtime_ctx->env.flow = exprtk_FLOW_NORMAL;
  runtime_ctx->env.error_msg[0] = '\0';
  if (use_jit) {
    if (!artifact->initializer_address) return -1;
    (void)((double (*)(void *))artifact->initializer_address)(runtime_ctx);
  } else {
    argument.a = runtime_ctx;
    MIR_interp_arr(artifact->ctx, artifact->initializer, &result, 1, &argument);
  }
  return runtime_ctx->env.aborted || runtime_ctx->env.flow == exprtk_FLOW_THROW ? -1 : 0;
}

/* =========================================================================
 * 计时辅助函数（跨平台）
 * ========================================================================= */

static uint64_t ts_get_time_us(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000ULL) / freq.QuadPart);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

/* =========================================================================
 * Public API: Compile, Exec, Run
 * ========================================================================= */

static int ts_track_compiled_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast) {
  if (!ctx || !ast) return 0;
  for (size_t i = 0; i < ctx->compiled_ast_count; ++i) {
    if (ctx->compiled_asts[i] == ast) return 1;
  }

  if (ctx->compiled_ast_count >= ctx->compiled_ast_capacity) {
    size_t new_cap = ctx->compiled_ast_capacity == 0 ? 16 : ctx->compiled_ast_capacity * 2;
    exprtk_node_t **new_asts =
        (exprtk_node_t **)realloc(ctx->compiled_asts, new_cap * sizeof(exprtk_node_t *));
    if (!new_asts) return 0;
    ctx->compiled_asts = new_asts;
    ctx->compiled_ast_capacity = new_cap;
  }

  ctx->compiled_asts[ctx->compiled_ast_count++] = ast;
  return 1;
}

static int turbo_script_compile_mir_backend(turbo_script_ctx_t *ctx, const char *script,
                                            exprtk_node_t *provided_ast, int use_interp) {
  uint64_t start_time = 0;
  MIR_context_t mir_ctx = NULL;
  exprtk_node_t *ast = provided_ast;
  int owns_ast = provided_ast == NULL;

  if (!ctx) return -1;
  if (ctx->memory_exhausted) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "memory policy: context is exhausted and must be recreated");
    return -1;
  }
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ast && !script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT compile error: script is NULL");
    return -1;
  }

  // 开始计时（如果启用统计）
  if (ctx->jit_stats_enabled) {
    start_time = ts_get_time_us();
  }

  if (use_interp) {
    if (!ctx->mir_interp_ctx) ctx->mir_interp_ctx = MIR_init();
    mir_ctx = ctx->mir_interp_ctx;
    ctx->mir_interp_last_func = NULL;
  } else {
    if (!ctx->mir_ctx) ctx->mir_ctx = MIR_init();
    mir_ctx = ctx->mir_ctx;
    ctx->mir_last_fn = NULL;
  }

  if (!ast) {
    ast = turbo_script_parse_with_error(ctx, script);
    if (!ast) return -1;
  }

  if (exprtk_validate(ast, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    ctx->error_code = TURBO_SCRIPT_ERROR_VALIDATE;
    if (owns_ast) exprtk_free(ast);
    return -1;
  }
  if (!ts_track_compiled_ast(ctx, ast)) {
    if (owns_ast) exprtk_free(ast);
    ctx->error_code = TURBO_SCRIPT_ERROR_OOM;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT compile error: out of memory");
    return -1;
  }
  if (ctx->expr == ast) ctx->expr_in_compiled_asts = 1;

  char mod_name[64];
  snprintf(mod_name, sizeof(mod_name), "ts_jit_mod_%d", ctx->mir_mod_idx++);

  MIR_module_t mod = MIR_new_module(mir_ctx, mod_name);

  MIR_item_t shared_initializer = NULL;
  const ts_mir_lowering_policy_t policy = {TS_MIR_HOST_SLOTS_DISABLED, NULL};
  if (ts_mir_lower_owned_module(mir_ctx, mod, ctx, ast, mod_name, &policy, NULL,
                                &shared_initializer, NULL, NULL, NULL) != 0) {
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE) ctx->error_code = TURBO_SCRIPT_ERROR_JIT;
    return -1;
  }
  MIR_load_module(mir_ctx, mod);
  if (use_interp) {
    if (!ctx->mir_interp_externals_loaded) {
      ts_mir_load_externals(mir_ctx);
      ctx->mir_interp_externals_loaded = 1;
    }
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);
    ctx->mir_interp_last_func = shared_initializer;
  } else {
    if (!ctx->mir_gen_initialized) {
      MIR_gen_init(mir_ctx);
      ts_mir_load_externals(mir_ctx);
      ctx->mir_gen_initialized = 1;
    }
    MIR_link(mir_ctx, MIR_set_gen_interface, NULL);
    ctx->mir_last_fn = shared_initializer->addr;
  }
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.compile_count++;
    ctx->jit_stats.total_compile_time_us += ts_get_time_us() - start_time;
  }
  return 0;
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, NULL, 0);
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir_interp(turbo_script_ctx_t *ctx,
                                                       const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, NULL, 1);
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast,
                                                    const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, ast, 0);
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir_interp_ast(turbo_script_ctx_t *ctx,
                                                           exprtk_node_t *ast, const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, ast, 1);
}

static void ts_jit_reset_runtime_state(turbo_script_ctx_t *ctx) {
  ctx->env.aborted = 0;
  ctx->env.flow = exprtk_FLOW_NORMAL;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;
  ctx->env.error_msg[0] = '\0';
}

static int ts_jit_finish_runtime_state(turbo_script_ctx_t *ctx) {
  if (ctx->env.aborted || ctx->env.flow == exprtk_FLOW_THROW) {
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE) {
      ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    }
    if (ctx->error_msg[0] == '\0') {
      const char *msg = ctx->env.error_msg[0] ? ctx->env.error_msg : "JIT runtime error";
      snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg);
    }
    return -1;
  }
  return 0;
}

static int ts_jit_exec_fn(turbo_script_ctx_t *ctx, void *fn_ptr) {
  typedef double (*jit_fn_t)(void *);

  if (!ctx || !fn_ptr) return -1;
  ts_jit_reset_runtime_state(ctx);
  ((jit_fn_t)fn_ptr)((void *)ctx);
  return ts_jit_finish_runtime_state(ctx);
}

TURBO_SCRIPT_C_API int turbo_script_exec_jit(turbo_script_ctx_t *ctx) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ctx->mir_last_fn) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT exec error: no compiled module");
    return -1;
  }

  /*  Direct call via cached pointer — no module list traversal */
  return ts_jit_exec_fn(ctx, ctx->mir_last_fn);
}

TURBO_SCRIPT_C_API int turbo_script_exec_mir_interp_result(turbo_script_ctx_t *ctx,
                                                           double *result_out) {
  MIR_val_t args[1];
  MIR_val_t result;

  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ctx->mir_interp_ctx || !ctx->mir_interp_last_func) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "MIR interp exec error: no compiled module");
    return -1;
  }

  ts_jit_reset_runtime_state(ctx);
  memset(&result, 0, sizeof(result));
  args[0].a = (void *)ctx;
  MIR_interp_arr(ctx->mir_interp_ctx, ctx->mir_interp_last_func, &result, 1, args);
  if (result_out) *result_out = result.d;
  return ts_jit_finish_runtime_state(ctx);
}

TURBO_SCRIPT_C_API int turbo_script_exec_mir_interp(turbo_script_ctx_t *ctx) {
  return turbo_script_exec_mir_interp_result(ctx, NULL);
}

TURBO_SCRIPT_C_API int turbo_script_run_mir_interp(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "MIR interp run error: script is NULL");
    return -1;
  }
  if (ctx->expr && ctx->expr_source && strcmp(ctx->expr_source, script) == 0) {
    if (turbo_script_compile_mir_interp_ast(ctx, ctx->expr, script) != 0) return -1;
    return turbo_script_exec_mir_interp(ctx);
  }
  if (turbo_script_compile_mir_interp(ctx, script) != 0) return -1;
  return turbo_script_exec_mir_interp(ctx);
}

/* =========================================================================
 *  FNV-1a hash for compile cache
 * ========================================================================= */

static uint64_t ts_hash_script(const char *s) {
  uint64_t h = 0xcbf29ce484222325ULL; // FNV-1a offset basis
  size_t len = 0;
  for (; *s; s++, len++) {
    h ^= (uint8_t)*s;
    h *= 0x100000001b3ULL; // FNV-1a prime
  }
  h ^= len; // 混合长度，减少短脚本冲突
  return h;
}

TURBO_SCRIPT_C_API int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script) {
  uint64_t exec_start_time = 0;

  if (!ctx) return -1;
  if (ctx->memory_exhausted) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "memory policy: context is exhausted and must be recreated");
    return -1;
  }
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT run error: script is NULL");
    return -1;
  }

  /*  Check compile cache — skip parse/compile on hit */
  uint64_t hash = ts_hash_script(script);
  uint32_t slot = (uint32_t)(hash % TS_JIT_CACHE_SIZE);
  if (ctx->jit_cache[slot].hash == hash && ctx->jit_cache[slot].script &&
      strcmp(ctx->jit_cache[slot].script, script) == 0 && ctx->jit_cache[slot].fn_ptr) {
    ctx->jit_cache[slot].access_count++; // 直接映射缓存；access_count 仅供未来 LRU 使用

    // 记录缓存命中统计
    if (ctx->jit_stats_enabled) {
      ctx->jit_stats.cache_hit_count++;
      ctx->jit_stats.exec_count++;
      exec_start_time = ts_get_time_us();
    }

    int result = ts_jit_exec_fn(ctx, ctx->jit_cache[slot].fn_ptr);

    // 记录执行时间
    if (ctx->jit_stats_enabled) {
      ctx->jit_stats.total_exec_time_us += ts_get_time_us() - exec_start_time;
    }

    return ts_memory_finish_run(ctx, result);
  }

  // 记录缓存未命中
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.cache_miss_count++;
  }

  /* Cache miss — full compile. Unsupported scripts must fail instead of
   * silently running through the interpreter. */
  if (turbo_script_compile_mir(ctx, script) != 0) return -1;

  /* Store in cache. A failed script copy leaves the slot empty so a later
   * lookup cannot match a partial (script == NULL) entry. */
  free(ctx->jit_cache[slot].script);
  ctx->jit_cache[slot].script = strdup(script);
  if (!ctx->jit_cache[slot].script) {
    ctx->jit_cache[slot].hash = 0;
    ctx->jit_cache[slot].fn_ptr = NULL;
    ctx->jit_cache[slot].access_count = 0;
  } else {
    ctx->jit_cache[slot].hash = hash;
    ctx->jit_cache[slot].fn_ptr = ctx->mir_last_fn;
    ctx->jit_cache[slot].access_count = 1; // 初始化访问计数
  }

  // 记录执行统计
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.exec_count++;
    exec_start_time = ts_get_time_us();
  }

  int result = turbo_script_exec_jit(ctx);

  // 记录执行时间
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.total_exec_time_us += ts_get_time_us() - exec_start_time;
  }

  return ts_memory_finish_run(ctx, result);
}
