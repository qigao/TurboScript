#include "turbo_script_cmeta_bridge.h"

#include <string.h>

static double ts_cmeta_signature_map_placeholder(double value) {
  return value;
}

static _Bool ts_cmeta_signature_filter_placeholder(double value) {
  return value != 0.0;
}

static double ts_cmeta_signature_reduce_placeholder(double left, double right) {
  return left + right;
}

static bool ts_cmeta_analysis_only_invoke(const cmeta_callable *self,
                                          void *out,
                                          const void *const *args) {
  (void)self;
  (void)out;
  (void)args;
  return false;
}

static bool ts_cmeta_param_name(const exprtk_node_t *lambda,
                                const char *name) {
  size_t i;
  if (!lambda || !name) return false;
  for (i = 0; i < lambda->data.func_def.arg_count; ++i) {
    const exprtk_node_t *param = lambda->data.func_def.arg_params[i];
    if (param && param->type == EXPRTK_NODE_VARIABLE &&
        param->data.variable.name &&
        strcmp(param->data.variable.name, name) == 0)
      return true;
  }
  return false;
}

static cmeta_effects ts_cmeta_merge_effects(cmeta_effects left,
                                            cmeta_effects right) {
  if (!cmeta_effects_valid(left) || !cmeta_effects_valid(right))
    return CMETA_EFFECT_UNKNOWN;
  return left | right;
}

static cmeta_effects ts_cmeta_expression_effects(const exprtk_node_t *node,
                                                 const exprtk_node_t *lambda,
                                                 size_t *capture_count) {
  size_t i;
  cmeta_effects effects = CMETA_EFFECT_PURE;

  if (!node) return CMETA_EFFECT_UNKNOWN;

  switch (node->type) {
  case EXPRTK_NODE_NUMBER:
  case EXPRTK_NODE_INTEGER:
  case EXPRTK_NODE_STRING:
  case EXPRTK_NODE_NULL:
    return CMETA_EFFECT_PURE;

  case EXPRTK_NODE_VARIABLE:
    if (node->data.variable.name &&
        ts_cmeta_param_name(lambda, node->data.variable.name))
      return CMETA_EFFECT_PURE;
    if (capture_count) ++*capture_count;
    return CMETA_EFFECT_UNKNOWN;

  case EXPRTK_NODE_BINARY_OP:
    return ts_cmeta_merge_effects(
        ts_cmeta_expression_effects(node->data.binary.left, lambda,
                                    capture_count),
        ts_cmeta_expression_effects(node->data.binary.right, lambda,
                                    capture_count));

  case EXPRTK_NODE_FLOW:
    return ts_cmeta_expression_effects(node->data.flow.value, lambda,
                                       capture_count);

  case EXPRTK_NODE_BLOCK:
    for (i = 0; i < node->data.block.count; ++i)
      effects = ts_cmeta_merge_effects(
          effects,
          ts_cmeta_expression_effects(node->data.block.statements[i], lambda,
                                      capture_count));
    return effects;

  case EXPRTK_NODE_VECTOR:
    for (i = 0; i < node->data.vector.count; ++i)
      effects = ts_cmeta_merge_effects(
          effects,
          ts_cmeta_expression_effects(node->data.vector.elements[i], lambda,
                                      capture_count));
    return effects;

  /*
   * Calls, member access, indexing, assignment, control flow and nested
   * functions require more semantic information than this first bridge owns.
   * Mark them UNKNOWN rather than guessing purity.
   */
  default:
    return CMETA_EFFECT_UNKNOWN;
  }
}

static cmeta_fn ts_cmeta_meta_for_role(ts_cmeta_lambda_role_t role) {
  cmeta_fn meta;
  switch (role) {
  case TS_CMETA_LAMBDA_FILTER:
    meta = CMETA_WRAP_TYPED_ANY(ts_cmeta_signature_filter_placeholder);
    break;
  case TS_CMETA_LAMBDA_MAP:
    meta = CMETA_WRAP_TYPED_ANY(ts_cmeta_signature_map_placeholder);
    break;
  case TS_CMETA_LAMBDA_REDUCE:
    meta = CMETA_WRAP_TYPED_ANY(ts_cmeta_signature_reduce_placeholder);
    break;
  default:
    memset(&meta, 0, sizeof(meta));
    break;
  }
  return meta;
}

static size_t ts_cmeta_expected_arity(ts_cmeta_lambda_role_t role) {
  switch (role) {
  case TS_CMETA_LAMBDA_FILTER:
  case TS_CMETA_LAMBDA_MAP:
    return 1u;
  case TS_CMETA_LAMBDA_REDUCE:
    return 2u;
  default:
    return 0u;
  }
}

bool ts_cmeta_describe_value_type(exprtk_value_type_t type,
                                  ts_cmeta_value_semantics_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));

  switch (type) {
  case EXPRTK_VAL_NUMBER:
    out->shape = TS_CMETA_VALUE_SCALAR;
    out->flow_type = &cmeta_type_double;
    return true;
  case EXPRTK_VAL_BOOL:
    out->shape = TS_CMETA_VALUE_SCALAR;
    out->flow_type = &cmeta_type_bool;
    return true;
  case EXPRTK_VAL_VECTOR:
    out->shape = TS_CMETA_VALUE_VECTOR;
    out->flow_type = &cmeta_type_double;
    return true;
  default:
    return false;
  }
}

bool ts_cmeta_analyze_lambda(const exprtk_node_t *lambda,
                             ts_cmeta_lambda_role_t role,
                             ts_cmeta_lambda_contract_t *out,
                             const char **error) {
  size_t expected_arity;
  size_t capture_count = 0u;
  cmeta_fn meta;
  cmeta_callable callable;
  const cmeta_sig_desc *signature;
  cmeta_effects effects;
  cmeta_properties properties;
  size_t i;

  if (error) *error = NULL;
  if (!lambda || !out) {
    if (error) *error = "lambda analysis requires non-null input/output";
    return false;
  }
  memset(out, 0, sizeof(*out));

  if (lambda->type != EXPRTK_NODE_FUNCTION_EXPRESSION) {
    if (error) *error = "graph callable must be a TurboScript function expression";
    return false;
  }

  expected_arity = ts_cmeta_expected_arity(role);
  if (expected_arity == 0u ||
      lambda->data.func_def.arg_count != expected_arity) {
    if (error) *error = "lambda arity does not match graph operator";
    return false;
  }

  for (i = 0; i < lambda->data.func_def.arg_count; ++i) {
    const exprtk_node_t *param = lambda->data.func_def.arg_params[i];
    if (!param || param->type != EXPRTK_NODE_VARIABLE ||
        !param->data.variable.name) {
      if (error) *error =
          "first CMeta graph bridge supports simple named lambda parameters only";
      return false;
    }
  }

  effects = ts_cmeta_expression_effects(lambda->data.func_def.body, lambda,
                                        &capture_count);
  properties = cmeta_effects_are_pure(effects)
                   ? CMETA_PROP_DETERMINISTIC
                   : CMETA_PROP_NONE;

  meta = ts_cmeta_meta_for_role(role);
  if (meta.sig == CMETA_SIG_INVALID) {
    if (error) *error = "unsupported graph lambda role";
    return false;
  }
  meta.effects = effects;
  meta.properties = properties;

  memset(&callable, 0, sizeof(callable));
  callable.meta = meta;
  callable.invoke = ts_cmeta_analysis_only_invoke;
  callable.dispatch = CMETA_CALLABLE_DISPATCH_ADAPTER;

  if (!cmeta_callable_contract_valid(callable)) {
    if (error) *error = "generated CMeta callable contract is invalid";
    return false;
  }

  signature = cmeta_callable_signature(callable);
  if (!signature || signature->param_count != expected_arity) {
    if (error) *error = "generated CMeta signature does not match lambda role";
    return false;
  }

  out->callable = callable;
  out->input_type = signature->params[0];
  out->output_type = signature->return_type;
  out->effects = effects;
  out->properties = properties;
  out->arity = expected_arity;
  out->capture_count = capture_count;
  out->executable = false;
  return true;
}
