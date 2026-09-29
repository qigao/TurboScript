#include "turbo_script_cflow_text_kernel.h"

#include "turbo_script_mir_internal.h"

#include <cflow/function_projection.h>
#include <cmeta/function.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ts_cflow_text_kernel_s {
  MIR_context_t mir_ctx;
  MIR_module_t module;
  MIR_item_t function;
  void *address;
  int gen_initialized;
};

_Static_assert(sizeof(ts_cflow_text_kernel_t *) <= CMETA_CAPTURE_INLINE,
               "text MIR kernel pointer must fit CMeta inline capture");

static const exprtk_node_t *ts_cflow_text_lambda_value(
    const exprtk_node_t *lambda) {
  const exprtk_node_t *body;

  if (!lambda || lambda->type != EXPRTK_NODE_FUNCTION_EXPRESSION ||
      lambda->data.func_def.arg_count != 1u ||
      !lambda->data.func_def.arg_params ||
      !lambda->data.func_def.arg_params[0] ||
      lambda->data.func_def.arg_params[0]->type != EXPRTK_NODE_VARIABLE ||
      !lambda->data.func_def.arg_params[0]->data.variable.name)
    return NULL;

  body = lambda->data.func_def.body;
  if (!body) return NULL;

  if (body->type == EXPRTK_NODE_BLOCK) {
    if (body->data.block.count != 1u || !body->data.block.statements)
      return NULL;
    body = body->data.block.statements[0];
  }

  if (body && body->type == EXPRTK_NODE_FLOW &&
      body->data.flow.type == exprtk_TOKEN_RETURN)
    body = body->data.flow.value;

  return body;
}

static bool ts_cflow_text_lambda_is_length_map(
    const exprtk_node_t *lambda) {
  const exprtk_node_t *body = ts_cflow_text_lambda_value(lambda);
  const exprtk_node_t *param;
  const exprtk_node_t *object;

  if (!body || body->type != EXPRTK_NODE_MEMBER_CALL ||
      !body->data.member_call.method ||
      strcmp(body->data.member_call.method, "length") != 0 ||
      body->data.member_call.arg_count != 0u)
    return false;

  param = lambda->data.func_def.arg_params[0];
  object = body->data.member_call.object;
  return object && object->type == EXPRTK_NODE_VARIABLE &&
         object->data.variable.name &&
         strcmp(object->data.variable.name,
                param->data.variable.name) == 0;
}

static bool ts_cflow_text_length_invoke(
    const cmeta_callable *self,
    void *out,
    const void *const *args) {
  ts_cflow_text_kernel_t *kernel = NULL;
  ts_cflow_line_slice_t slice;
  double result;

  if (!self || !out || !args || !args[0] ||
      self->capture_size != sizeof(kernel))
    return false;

  memcpy(&kernel, self->capture.bytes, sizeof(kernel));
  if (!kernel || !kernel->address) return false;

  memcpy(&slice, args[0], sizeof(slice));
  if (slice.len > (size_t)INT64_MAX) return false;

  result =
      ((double (*)(const char *, int64_t))kernel->address)(
          slice.data, (int64_t)slice.len);
  memcpy(out, &result, sizeof(result));
  return true;
}

static void ts_cflow_text_kernel_destroy(ts_cflow_text_kernel_t *kernel) {
  if (!kernel) return;
  if (kernel->mir_ctx) {
    if (kernel->gen_initialized) MIR_gen_finish(kernel->mir_ctx);
    MIR_finish(kernel->mir_ctx);
  }
  free(kernel);
}

static bool ts_cflow_text_length_kernel_compile(
    ts_cflow_text_kernel_t *kernel,
    const char **error) {
  MIR_type_t result_type = MIR_T_D;
  MIR_var_t args[2];
  MIR_reg_t len_reg;
  MIR_reg_t result_reg;

  if (!kernel) return false;

  kernel->mir_ctx = MIR_init();
  if (!kernel->mir_ctx) {
    if (error) *error = "failed to create MIR context for text kernel";
    return false;
  }

  kernel->module =
      MIR_new_module(kernel->mir_ctx, "ts_cflow_text_kernel");
  if (!kernel->module) {
    if (error) *error = "failed to create MIR module for text kernel";
    return false;
  }

  args[0].type = MIR_T_P;
  args[0].name = "data";
  args[0].size = 0u;
  args[1].type = MIR_T_I64;
  args[1].name = "len";
  args[1].size = 0u;

  kernel->function = MIR_new_func_arr(
      kernel->mir_ctx, "ts_cflow_line_length",
      1u, &result_type, 2u, args);
  if (!kernel->function) {
    if (error) *error = "failed to create MIR text kernel function";
    return false;
  }

  len_reg = MIR_reg(
      kernel->mir_ctx, "len", kernel->function->u.func);
  result_reg = MIR_new_func_reg(
      kernel->mir_ctx, kernel->function->u.func,
      MIR_T_D, "result");

  MIR_append_insn(
      kernel->mir_ctx, kernel->function,
      MIR_new_insn(
          kernel->mir_ctx, MIR_I2D,
          MIR_new_reg_op(kernel->mir_ctx, result_reg),
          MIR_new_reg_op(kernel->mir_ctx, len_reg)));
  MIR_append_insn(
      kernel->mir_ctx, kernel->function,
      MIR_new_ret_insn(
          kernel->mir_ctx, 1u,
          MIR_new_reg_op(kernel->mir_ctx, result_reg)));

  MIR_finish_func(kernel->mir_ctx);
  MIR_finish_module(kernel->mir_ctx);
  MIR_load_module(kernel->mir_ctx, kernel->module);

  MIR_gen_init(kernel->mir_ctx);
  kernel->gen_initialized = 1;
  MIR_link(kernel->mir_ctx, MIR_set_gen_interface, NULL);
  kernel->address =
      MIR_gen(kernel->mir_ctx, kernel->function);
  if (!kernel->address) {
    if (error) *error = "MIR failed to generate text length kernel";
    return false;
  }

  return true;
}

bool ts_cflow_text_length_map_bind(
    const exprtk_node_t *lambda,
    ts_cflow_text_kernel_binding_t *out,
    const char **error) {
  ts_cflow_text_kernel_t *kernel = NULL;

  if (error) *error = NULL;
  if (out) memset(out, 0, sizeof(*out));
  if (!lambda || !out) {
    if (error) *error = "text length MAP binding requires lambda and output";
    return false;
  }

  if (!ts_cflow_text_lambda_is_length_map(lambda)) {
    if (error)
      *error =
          "typed text MAP first slice supports only line => line.length()";
    return false;
  }

  kernel = (ts_cflow_text_kernel_t *)calloc(1u, sizeof(*kernel));
  if (!kernel) {
    if (error) *error = "out of memory creating typed text MIR kernel";
    return false;
  }

  if (!ts_cflow_text_length_kernel_compile(kernel, error)) {
    ts_cflow_text_kernel_destroy(kernel);
    return false;
  }

  out->callable.meta.effects = CMETA_EFFECT_PURE;
  out->callable.meta.properties =
      CMETA_PROP_DETERMINISTIC |
      CMETA_PROP_TOTAL |
      CMETA_PROP_NO_ALIAS;
  out->callable.invoke = ts_cflow_text_length_invoke;
  out->callable.dispatch = CMETA_CALLABLE_DISPATCH_ADAPTER;
  out->callable.capture_size = sizeof(kernel);
  memcpy(out->callable.capture.bytes, &kernel, sizeof(kernel));
  out->owner = kernel;
  return true;
}

bool ts_cflow_text_length_map_graph_add(
    cflow_graph *graph,
    const ts_cflow_text_kernel_binding_t *binding,
    const char **error) {
  cmeta_type_desc input_ptr_type;
  cmeta_param_desc param;
  cmeta_function_desc function;
  cmeta_abi_carrier param_abi[1];
  cmeta_function_abi_desc abi;
  cflow_function_typed_adapter_projection projection;
  cflow_function_projection_status status;

  if (error) *error = NULL;
  if (!graph || !binding || !binding->owner) {
    if (error) *error = "typed text MAP graph admission requires binding";
    return false;
  }

  memset(&input_ptr_type, 0, sizeof(input_ptr_type));
  input_ptr_type.name = "TurboScript.LineSlice.v1 *";
  input_ptr_type.size = sizeof(ts_cflow_line_slice_t *);
  input_ptr_type.align = _Alignof(ts_cflow_line_slice_t *);
  input_ptr_type.kind = CMETA_T_POINTER;
  input_ptr_type.pointee = ts_cflow_line_slice_type();

  memset(&param, 0, sizeof(param));
  param.size = sizeof(param);
  param.name = "line";
  param.type = &input_ptr_type;
  param.flags = CMETA_PARAM_IN | CMETA_PARAM_BORROWED;

  memset(&function, 0, sizeof(function));
  function.size = sizeof(function);
  function.name = "TurboScript.LineSlice.length";
  function.return_type = &cmeta_type_double;
  function.params = &param;
  function.param_count = 1u;
  function.effects = binding->callable.meta.effects;
  function.properties = binding->callable.meta.properties;

  param_abi[0] = CMETA_ABI_OBJECT_POINTER;
  memset(&abi, 0, sizeof(abi));
  abi.size = sizeof(abi);
  abi.function = &function;
  abi.return_carrier = CMETA_ABI_SCALAR;
  abi.param_carriers = param_abi;
  abi.param_count = 1u;

  memset(&projection, 0, sizeof(projection));
  status = cflow_function_typed_adapter_projection_admit(
      &function, &abi, binding->callable,
      ts_cflow_line_slice_type(), &cmeta_type_double,
      &projection);
  if (status != CFLOW_FUNCTION_PROJECTION_OK) {
    if (error)
      *error = cflow_function_projection_status_string(status);
    return false;
  }

  if (!cflow_graph_add_function_typed_adapter_projection(
          graph, &projection)) {
    if (error)
      *error = graph->error ? graph->error
                            : "typed text MAP graph admission failed";
    return false;
  }
  return true;
}

void ts_cflow_text_kernel_binding_destroy(
    ts_cflow_text_kernel_binding_t *binding) {
  if (!binding) return;
  ts_cflow_text_kernel_destroy(binding->owner);
  memset(binding, 0, sizeof(*binding));
}
