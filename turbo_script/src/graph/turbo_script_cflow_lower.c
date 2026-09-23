#include "turbo_script_cflow_lower.h"
#include "turbo_script_cmeta_bridge.h"

#include <string.h>

static bool ts_cflow_fail(ts_cflow_lowered_pipeline_t *out,
                          const char **error,
                          const char *message) {
  if (error) *error = message;
  if (out) cflow_graph_destroy(&out->graph);
  return false;
}

static bool ts_cflow_vector_source(const exprtk_node_t *node) {
  size_t i;
  if (!node || node->type != EXPRTK_NODE_VECTOR) return false;
  for (i = 0; i < node->data.vector.count; ++i) {
    const exprtk_node_t *element = node->data.vector.elements[i];
    if (!element ||
        (element->type != EXPRTK_NODE_NUMBER &&
         element->type != EXPRTK_NODE_INTEGER))
      return false;
  }
  return true;
}

static bool ts_cflow_append_callable(ts_cflow_lowered_pipeline_t *out,
                                     const exprtk_node_t *lambda,
                                     ts_cmeta_lambda_role_t role,
                                     cflow_op op,
                                     const char **error) {
  ts_cmeta_lambda_contract_t contract;
  const char *analysis_error = NULL;

  if (!ts_cmeta_analyze_lambda(lambda, role, &contract, &analysis_error)) {
    if (error) *error = analysis_error ? analysis_error
                                       : "CMeta lambda analysis failed";
    return false;
  }

  if (!cmeta_effects_are_pure(contract.effects)) {
    if (error) *error =
        "first CFlow lowering slice requires PURE graph callables";
    return false;
  }

  if (!cflow_graph_add(&out->graph, op, contract.callable, NULL)) {
    if (error) *error = out->graph.error ? out->graph.error
                                         : "CFlow graph operator admission failed";
    return false;
  }
  return true;
}

static bool ts_cflow_lower_node(const exprtk_node_t *expr,
                                ts_cflow_lowered_pipeline_t *out,
                                const char **error) {
  const char *name;

  if (!expr || !out) {
    if (error) *error = "pipeline lowering requires non-null AST/output";
    return false;
  }

  if (ts_cflow_vector_source(expr)) {
    cflow_graph_init(&out->graph, &cmeta_type_double);
    if (out->graph.error) {
      if (error) *error = out->graph.error;
      return false;
    }
    out->source_expr = expr;
    return true;
  }

  if (expr->type == EXPRTK_NODE_MEMBER_CALL &&
      expr->data.member_call.method) {
    const exprtk_node_t *object = expr->data.member_call.object;
    name = expr->data.member_call.method;

    /* vector.stream() is a language facade boundary, not a graph node. */
    if (strcmp(name, "stream") == 0) {
      if (expr->data.member_call.arg_count != 0u) {
        if (error) *error = "stream() pipeline source takes no arguments";
        return false;
      }
      return ts_cflow_lower_node(object, out, error);
    }

    /* stream.of(vector) is represented by the parser as a module-style
     * member call on the 'stream' namespace. */
    if (strcmp(name, "of") == 0 && object &&
        object->type == EXPRTK_NODE_VARIABLE &&
        object->data.variable.name &&
        strcmp(object->data.variable.name, "stream") == 0) {
      if (expr->data.member_call.arg_count != 1u) {
        if (error) *error = "stream.of() requires exactly one source";
        return false;
      }
      return ts_cflow_lower_node(expr->data.member_call.args[0], out, error);
    }

    if (strcmp(name, "filter") == 0) {
      if (expr->data.member_call.arg_count != 1u) {
        if (error) *error = "stream filter requires one predicate";
        return false;
      }
      if (!ts_cflow_lower_node(object, out, error))
        return false;
      return ts_cflow_append_callable(out, expr->data.member_call.args[0],
                                      TS_CMETA_LAMBDA_FILTER,
                                      CFLOW_OP_FILTER, error);
    }

    if (strcmp(name, "map") == 0) {
      if (expr->data.member_call.arg_count != 1u) {
        if (error) *error = "stream map requires one mapper";
        return false;
      }
      if (!ts_cflow_lower_node(object, out, error))
        return false;
      return ts_cflow_append_callable(out, expr->data.member_call.args[0],
                                      TS_CMETA_LAMBDA_MAP,
                                      CFLOW_OP_MAP, error);
    }

    if (strcmp(name, "reduce") == 0) {
      if (expr->data.member_call.arg_count != 2u) {
        if (error) *error = "stream reduce requires seed and reducer";
        return false;
      }
      if (!ts_cflow_lower_node(object, out, error))
        return false;
      out->reduce_seed_expr = expr->data.member_call.args[0];
      out->has_reduce_seed = true;
      return ts_cflow_append_callable(out, expr->data.member_call.args[1],
                                      TS_CMETA_LAMBDA_REDUCE,
                                      CFLOW_OP_REDUCE, error);
    }

    if (error) *error =
        "member call is not part of the first CFlow pipeline slice";
    return false;
  }

  if (expr->type != EXPRTK_NODE_FUNCTION_CALL ||
      !expr->data.function.name) {
    if (error) *error =
        "graphable pipeline must start from a numeric vector source";
    return false;
  }

  name = expr->data.function.name;

  if (strcmp(name, "filter") == 0) {
    if (expr->data.function.arg_count != 2u) {
      if (error) *error = "filter pipeline node requires source and predicate";
      return false;
    }
    if (!ts_cflow_lower_node(expr->data.function.args[0], out, error))
      return false;
    return ts_cflow_append_callable(out, expr->data.function.args[1],
                                    TS_CMETA_LAMBDA_FILTER,
                                    CFLOW_OP_FILTER, error);
  }

  if (strcmp(name, "map") == 0) {
    if (expr->data.function.arg_count != 2u) {
      if (error) *error = "map pipeline node requires source and mapper";
      return false;
    }
    if (!ts_cflow_lower_node(expr->data.function.args[0], out, error))
      return false;
    return ts_cflow_append_callable(out, expr->data.function.args[1],
                                    TS_CMETA_LAMBDA_MAP,
                                    CFLOW_OP_MAP, error);
  }

  if (strcmp(name, "reduce") == 0) {
    if (expr->data.function.arg_count != 3u) {
      if (error) *error =
          "reduce pipeline node requires source, seed and reducer";
      return false;
    }
    if (!ts_cflow_lower_node(expr->data.function.args[0], out, error))
      return false;
    out->reduce_seed_expr = expr->data.function.args[1];
    out->has_reduce_seed = true;
    return ts_cflow_append_callable(out, expr->data.function.args[2],
                                    TS_CMETA_LAMBDA_REDUCE,
                                    CFLOW_OP_REDUCE, error);
  }

  if (error) *error = "function is not part of the first CFlow pipeline slice";
  return false;
}

bool ts_cflow_lower_pipeline(const exprtk_node_t *expr,
                             ts_cflow_lowered_pipeline_t *out,
                             const char **error) {
  const char *validation_error = NULL;

  if (error) *error = NULL;
  if (!out) {
    if (error) *error = "pipeline lowering requires an output object";
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->graph.root = CMETA_INVALID_ID;

  if (!ts_cflow_lower_node(expr, out, error)) {
    cflow_graph_destroy(&out->graph);
    return false;
  }

  if (!cflow_graph_validate(&out->graph, &validation_error))
    return ts_cflow_fail(out, error,
                         validation_error ? validation_error
                                          : "lowered CFlow graph is invalid");

  /*
   * Analysis-only CMeta callables intentionally cannot execute yet, and
   * TurboScript reduce carries an explicit seed while current CFlow REDUCE is
   * an unseeded fold. #17 must satisfy both execution contracts before this
   * flag may become true.
   */
  out->executable = false;
  return true;
}

void ts_cflow_lowered_pipeline_destroy(ts_cflow_lowered_pipeline_t *pipeline) {
  if (!pipeline) return;
  cflow_graph_destroy(&pipeline->graph);
  memset(pipeline, 0, sizeof(*pipeline));
  pipeline->graph.root = CMETA_INVALID_ID;
}
