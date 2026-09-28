#include "turbo_script_cflow_mir_kernel.h"

#include "turbo_script_mir_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ts_cflow_mir_kernel_s {
  MIR_context_t mir_ctx;
  MIR_module_t module;
  MIR_item_t function;
  void *address;
  turbo_script_ctx_t *runtime_ctx; /* borrowed; context must outlive binding */
  ts_mir_owned_string_block_t *owned_strings;
  ts_cmeta_lambda_role_t role;
  int gen_initialized;
};

static ts_compiled_func_t *ts_cflow_find_compiled_kernel(
    ts_mir_compiler_t *compiler, const char *name) {
  if (!compiler || !name) return NULL;
  for (int i = 0; i < compiler->compiled_func_count; ++i) {
    ts_compiled_func_t *entry = &compiler->compiled_funcs[i];
    if (entry->name && strcmp(entry->name, name) == 0) return entry;
  }
  return NULL;
}

static bool ts_cflow_mir_kernel_invoke(const cmeta_callable *self,
                                       void *out,
                                       const void *const *args) {
  ts_cflow_mir_kernel_t *kernel = NULL;
  double input;
  double numeric_result;
  bool predicate_result;

  if (!self || !out || !args || !args[0] ||
      self->capture_size != sizeof(kernel))
    return false;
  memcpy(&kernel, self->capture.bytes, sizeof(kernel));
  if (!kernel || !kernel->runtime_ctx || !kernel->address) return false;
  memcpy(&input, args[0], sizeof(input));

  kernel->runtime_ctx->env.aborted = 0;
  kernel->runtime_ctx->env.flow = exprtk_FLOW_NORMAL;
  kernel->runtime_ctx->env.error_msg[0] = '\0';

  numeric_result =
      ((double (*)(void *, void *, double))kernel->address)(
          kernel->runtime_ctx, &kernel->runtime_ctx->env, input);
  if (kernel->runtime_ctx->env.aborted ||
      kernel->runtime_ctx->env.flow == exprtk_FLOW_THROW)
    return false;

  if (kernel->role == TS_CMETA_LAMBDA_FILTER) {
    predicate_result = numeric_result != 0.0;
    memcpy(out, &predicate_result, sizeof(predicate_result));
    return true;
  }
  if (kernel->role == TS_CMETA_LAMBDA_MAP) {
    memcpy(out, &numeric_result, sizeof(numeric_result));
    return true;
  }
  return false;
}

static void ts_cflow_mir_kernel_destroy(ts_cflow_mir_kernel_t *kernel) {
  if (!kernel) return;
  if (kernel->mir_ctx) {
    if (kernel->gen_initialized) MIR_gen_finish(kernel->mir_ctx);
    MIR_finish(kernel->mir_ctx);
  }
  ts_mir_owned_string_blocks_destroy(kernel->owned_strings);
  free(kernel);
}

bool ts_cflow_mir_kernel_bind(turbo_script_ctx_t *runtime_ctx,
                              const exprtk_node_t *lambda,
                              ts_cmeta_lambda_role_t role,
                              ts_cflow_mir_kernel_binding_t *out,
                              const char **error) {
  static const char kernel_name[] = "__cflow_kernel";
  ts_cmeta_lambda_contract_t contract;
  ts_mir_compiler_t compiler = {0};
  ts_compiled_func_t *compiled = NULL;
  ts_cflow_mir_kernel_t *kernel = NULL;
  const char *analysis_error = NULL;
  int module_finished = 0;
  bool success = false;

  if (error) *error = NULL;
  if (out) memset(out, 0, sizeof(*out));
  if (!runtime_ctx || !lambda || !out) {
    if (error) *error = "MIR kernel binding requires context, lambda and output";
    return false;
  }
  if (role != TS_CMETA_LAMBDA_MAP && role != TS_CMETA_LAMBDA_FILTER) {
    if (error) *error = "first MIR kernel slice supports map/filter only";
    return false;
  }
  if (!ts_cmeta_analyze_lambda(lambda, role, &contract, &analysis_error)) {
    if (error) *error = analysis_error ? analysis_error
                                       : "CMeta lambda analysis failed";
    return false;
  }
  if (!cmeta_effects_are_pure(contract.effects) ||
      contract.capture_count != 0u) {
    if (error) *error = "MIR CFlow kernel requires PURE capture-free lambda";
    return false;
  }

  kernel = (ts_cflow_mir_kernel_t *)calloc(1, sizeof(*kernel));
  if (!kernel) {
    if (error) *error = "out of memory creating MIR CFlow kernel";
    return false;
  }
  kernel->runtime_ctx = runtime_ctx;
  kernel->role = role;
  kernel->mir_ctx = MIR_init();
  if (!kernel->mir_ctx) {
    if (error) *error = "failed to create MIR context for CFlow kernel";
    goto done;
  }
  kernel->module = MIR_new_module(kernel->mir_ctx, "ts_cflow_kernel");
  if (!kernel->module) {
    if (error) *error = "failed to create MIR module for CFlow kernel";
    goto done;
  }

  compiler.ctx = kernel->mir_ctx;
  compiler.module = kernel->module;
  compiler.ts_ctx = runtime_ctx;
  compiler.ast_root = (exprtk_node_t *)lambda;
  compiler.lowering_policy =
      (ts_mir_lowering_policy_t){TS_MIR_HOST_SLOTS_DISABLED, NULL};
  snprintf(compiler.item_prefix, sizeof(compiler.item_prefix), "ts_cflow");
  ts_mir_init_externals(&compiler);

  ts_compile_script_func(&compiler, kernel_name,
                         lambda->data.func_def.arg_params,
                         lambda->data.func_def.arg_count,
                         lambda->data.func_def.body);
  compiled = ts_cflow_find_compiled_kernel(&compiler, kernel_name);
  if (compiler.failed || !compiled || compiled->arg_count != 1u ||
      !compiled->mir_func) {
    if (error) *error = "TurboScript lambda is not eligible for MIR kernel compilation";
    goto compiler_done;
  }
  kernel->function = compiled->mir_func;
  MIR_finish_module(kernel->mir_ctx);
  module_finished = 1;

compiler_done:
  kernel->owned_strings = ts_mir_take_owned_strings(&compiler);
  ts_mir_destroy_compiler_storage(&compiler);
  if (!kernel->function) goto done;

  MIR_load_module(kernel->mir_ctx, kernel->module);
  ts_mir_load_externals(kernel->mir_ctx);
  MIR_link(kernel->mir_ctx, MIR_set_interp_interface, NULL);
  MIR_gen_init(kernel->mir_ctx);
  kernel->gen_initialized = 1;
  MIR_link(kernel->mir_ctx, MIR_set_gen_interface, NULL);
  kernel->address = MIR_gen(kernel->mir_ctx, kernel->function);
  if (!kernel->address) {
    if (error) *error = "MIR failed to generate CFlow kernel code";
    goto done;
  }

  out->callable = contract.callable;
  out->callable.invoke = ts_cflow_mir_kernel_invoke;
  out->callable.generate = NULL;
  out->callable.dispatch = CMETA_CALLABLE_DISPATCH_ADAPTER;
  out->callable.capture_size = sizeof(kernel);
  memset(out->callable.capture.bytes, 0, sizeof(out->callable.capture.bytes));
  memcpy(out->callable.capture.bytes, &kernel, sizeof(kernel));
  if (!cmeta_callable_contract_valid(out->callable)) {
    if (error) *error = "generated MIR CFlow callable contract is invalid";
    memset(out, 0, sizeof(*out));
    goto done;
  }
  out->owner = kernel;
  success = true;

done:
  if (!module_finished && kernel && kernel->mir_ctx && kernel->module)
    MIR_finish_module(kernel->mir_ctx);
  if (!success) ts_cflow_mir_kernel_destroy(kernel);
  return success;
}

void ts_cflow_mir_kernel_binding_destroy(
    ts_cflow_mir_kernel_binding_t *binding) {
  if (!binding) return;
  ts_cflow_mir_kernel_destroy(binding->owner);
  memset(binding, 0, sizeof(*binding));
}
