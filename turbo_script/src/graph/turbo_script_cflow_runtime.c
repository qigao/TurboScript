#include "turbo_script_cflow_runtime.h"

#include "../turbo_script_internal.h"
#include "turbo_script_cflow_lower.h"
#include "exprtk_grammar.h"

#include <cflow/plan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum ts_cflow_terminal_e {
  TS_CFLOW_TERMINAL_NONE = 0,
  TS_CFLOW_TERMINAL_REDUCE,
  TS_CFLOW_TERMINAL_COUNT,
  TS_CFLOW_TERMINAL_COLLECT,
  TS_CFLOW_TERMINAL_TO_LIST,
  TS_CFLOW_TERMINAL_TO_VECTOR
} ts_cflow_terminal_t;

static void ts_cflow_runtime_error(char *error, size_t error_size,
                                   const char *message) {
  if (!error || error_size == 0u) return;
  snprintf(error, error_size, "%s", message ? message : "CFlow stream execution failed");
}

static int ts_cflow_runtime_numeric_literal(const exprtk_node_t *node,
                                            double *out) {
  double value;
  if (!node || !out) return 0;
  if (node->type == EXPRTK_NODE_NUMBER) {
    *out = node->data.number;
    return 1;
  }
  if (node->type == EXPRTK_NODE_INTEGER) {
    *out = (double)node->data.integer;
    return 1;
  }
  if (node->type != EXPRTK_NODE_BINARY_OP || node->data.binary.left != NULL ||
      (node->data.binary.op != exprtk_TOKEN_PLUS &&
       node->data.binary.op != exprtk_TOKEN_MINUS) ||
      !ts_cflow_runtime_numeric_literal(node->data.binary.right, &value))
    return 0;
  *out = node->data.binary.op == exprtk_TOKEN_MINUS ? -value : value;
  return 1;
}

static int ts_cflow_runtime_literal_seed(const exprtk_node_t *expr) {
  double seed;
  if (!expr) return 0;
  if (expr->type == EXPRTK_NODE_MEMBER_CALL &&
      expr->data.member_call.method &&
      strcmp(expr->data.member_call.method, "reduce") == 0 &&
      expr->data.member_call.arg_count == 2u)
    return ts_cflow_runtime_numeric_literal(expr->data.member_call.args[0], &seed);
  if (expr->type == EXPRTK_NODE_FUNCTION_CALL &&
      expr->data.function.name &&
      strcmp(expr->data.function.name, "reduce") == 0 &&
      expr->data.function.arg_count == 3u)
    return ts_cflow_runtime_numeric_literal(expr->data.function.args[1], &seed);
  return 0;
}

static ts_cflow_terminal_t ts_cflow_runtime_terminal(
    const exprtk_node_t *expr, const exprtk_node_t **pipeline) {
  if (pipeline) *pipeline = NULL;
  if (!expr) return TS_CFLOW_TERMINAL_NONE;

  if ((expr->type == EXPRTK_NODE_MEMBER_CALL &&
       expr->data.member_call.method &&
       strcmp(expr->data.member_call.method, "reduce") == 0 &&
       expr->data.member_call.arg_count == 2u) ||
      (expr->type == EXPRTK_NODE_FUNCTION_CALL &&
       expr->data.function.name &&
       strcmp(expr->data.function.name, "reduce") == 0 &&
       expr->data.function.arg_count == 3u)) {
    if (!ts_cflow_runtime_literal_seed(expr)) return TS_CFLOW_TERMINAL_NONE;
    if (pipeline) *pipeline = expr;
    return TS_CFLOW_TERMINAL_REDUCE;
  }

  if (expr->type == EXPRTK_NODE_MEMBER_CALL &&
      expr->data.member_call.method &&
      expr->data.member_call.arg_count == 0u) {
    if (strcmp(expr->data.member_call.method, "count") == 0) {
      if (pipeline) *pipeline = expr->data.member_call.object;
      return TS_CFLOW_TERMINAL_COUNT;
    }
    if (strcmp(expr->data.member_call.method, "collect") == 0) {
      if (pipeline) *pipeline = expr->data.member_call.object;
      return TS_CFLOW_TERMINAL_COLLECT;
    }
    if (strcmp(expr->data.member_call.method, "toList") == 0) {
      if (pipeline) *pipeline = expr->data.member_call.object;
      return TS_CFLOW_TERMINAL_TO_LIST;
    }
    if (strcmp(expr->data.member_call.method, "toVector") == 0) {
      if (pipeline) *pipeline = expr->data.member_call.object;
      return TS_CFLOW_TERMINAL_TO_VECTOR;
    }
  }

  return TS_CFLOW_TERMINAL_NONE;
}

static int ts_cflow_runtime_graph_has_op(const cflow_graph *graph, cflow_op op) {
  const cflow_subgraph *root;
  if (!graph) return 0;
  root = cflow_graph_subgraph(graph, graph->root);
  if (!root) return 0;
  for (size_t i = 0; i < root->node_count; ++i) {
    if (root->nodes[i].op == op) return 1;
  }
  return 0;
}

static int ts_cflow_runtime_result_is_double(const cflow_result *result) {
  return result && cmeta_type_equal(result->type, &cmeta_type_double) &&
         (result->count == 0u || result->data != NULL);
}

static int ts_cflow_runtime_result_to_vector(turbo_script_ctx_t *ctx,
                                             const cflow_result *result,
                                             exprtk_value_t *out) {
  exprtk_value_t borrowed;
  if (!ctx || !out || !ts_cflow_runtime_result_is_double(result)) return 0;
  borrowed = exprtk_val_vec((double *)result->data, result->count);
  return exprtk_value_copy_to_env(borrowed, &ctx->env, out) == 0;
}

static int ts_cflow_runtime_result_to_list(const cflow_result *result,
                                           exprtk_value_t *out) {
  exprtk_value_t list;
  const double *values;
  if (!out || !ts_cflow_runtime_result_is_double(result)) return 0;
  list = exprtk_val_list_empty();
  values = (const double *)result->data;
  for (size_t i = 0; i < result->count; ++i) {
    if (exprtk_list_push(&list, exprtk_val_num(values[i])) != 0) {
      exprtk_value_destroy(&list);
      return 0;
    }
  }
  *out = list;
  return 1;
}

static int ts_cflow_runtime_source_array(turbo_script_ctx_t *ctx,
                                         const exprtk_node_t *source,
                                         double **data_out,
                                         size_t *count_out) {
  double *data = NULL;
  size_t count = 0u;

  if (data_out) *data_out = NULL;
  if (count_out) *count_out = 0u;
  if (!ctx || !source || !data_out || !count_out) return 0;

  if (source->type == EXPRTK_NODE_VECTOR) {
    count = source->data.vector.count;
    if (count > 0u) {
      data = (double *)malloc(count * sizeof(*data));
      if (!data) return 0;
    }
    for (size_t i = 0; i < count; ++i) {
      if (!ts_cflow_runtime_numeric_literal(source->data.vector.elements[i],
                                            &data[i])) {
        free(data);
        return 0;
      }
    }
  } else if (source->type == EXPRTK_NODE_VARIABLE &&
             source->data.variable.name) {
    exprtk_value_t value =
        exprtk_env_get(&ctx->env, source->data.variable.name);
    if (value.type != EXPRTK_VAL_VECTOR) return 0;
    count = value.data.vector.size;
    if (count > 0u) {
      data = (double *)malloc(count * sizeof(*data));
      if (!data) return 0;
      memcpy(data, value.data.vector.data, count * sizeof(*data));
    }
  } else {
    return 0;
  }

  *data_out = data;
  *count_out = count;
  return 1;
}

ts_cflow_runtime_status_t ts_cflow_runtime_try_scalar_terminal(
    turbo_script_ctx_t *ctx, const exprtk_node_t *expr, exprtk_value_t *out,
    char *error, size_t error_size) {
  const exprtk_node_t *pipeline_expr = NULL;
  ts_cflow_terminal_t terminal;
  ts_cflow_lowered_pipeline_t analysis = {0};
  ts_cflow_lowered_pipeline_t lowered = {0};
  cflow_plan plan = {0};
  cflow_result result = {0};
  double *input = NULL;
  size_t input_count = 0u;
  const char *lower_error = NULL;
  int collect_as_list = 0;
  ts_cflow_runtime_status_t status = TS_CFLOW_RUNTIME_ERROR;

  if (error && error_size > 0u) error[0] = '\0';
  if (!ctx || !expr || !out) return TS_CFLOW_RUNTIME_NOT_APPLICABLE;

  terminal = ts_cflow_runtime_terminal(expr, &pipeline_expr);
  if (terminal == TS_CFLOW_TERMINAL_NONE || !pipeline_expr)
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;

  /*
   * Analysis admission is the migration boundary. Capturing/effectful lambdas
   * and non-numeric sources stay on the existing facade for now. After this
   * succeeds, executable failures are real CFlow errors and must not fallback.
   */
  if (!ts_cflow_lower_pipeline_runtime_analysis(
          ctx, pipeline_expr, &analysis, &lower_error))
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;

  if (terminal != TS_CFLOW_TERMINAL_REDUCE &&
      ts_cflow_runtime_graph_has_op(&analysis.graph, CFLOW_OP_REDUCE)) {
    ts_cflow_lowered_pipeline_destroy(&analysis);
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;
  }
  collect_as_list =
      terminal == TS_CFLOW_TERMINAL_COLLECT &&
      ts_cflow_runtime_graph_has_op(&analysis.graph, CFLOW_OP_MAP);
  ts_cflow_lowered_pipeline_destroy(&analysis);

  if (!ts_cflow_lower_pipeline_executable(ctx, pipeline_expr, &lowered,
                                          &lower_error)) {
    ts_cflow_runtime_error(error, error_size,
                           lower_error ? lower_error
                                       : "CFlow executable lowering failed");
    goto done;
  }

  if (!ts_cflow_runtime_source_array(
          ctx, lowered.source_expr, &input, &input_count)) {
    ts_cflow_runtime_error(error, error_size,
                           "CFlow stream source materialization failed");
    goto done;
  }

  if (!cflow_plan_compile_surface(&plan, &lowered.graph, NULL)) {
    ts_cflow_runtime_error(error, error_size,
                           "CFlow stream plan compilation failed");
    goto done;
  }
  if (!cflow_plan_eval_array(&plan, input, input_count, &result)) {
    ts_cflow_runtime_error(error, error_size,
                           "CFlow stream plan execution failed");
    goto done;
  }

  if (terminal == TS_CFLOW_TERMINAL_REDUCE) {
    if (result.count != 1u || !result.data ||
        !cmeta_type_equal(result.type, &cmeta_type_double)) {
      ts_cflow_runtime_error(error, error_size,
                             "CFlow reduce produced an invalid result");
      goto done;
    }
    *out = exprtk_val_num(((const double *)result.data)[0]);
  } else if (terminal == TS_CFLOW_TERMINAL_COUNT) {
    *out = exprtk_val_num((double)result.count);
  } else if (terminal == TS_CFLOW_TERMINAL_TO_VECTOR ||
             (terminal == TS_CFLOW_TERMINAL_COLLECT && !collect_as_list)) {
    if (!ts_cflow_runtime_result_to_vector(ctx, &result, out)) {
      ts_cflow_runtime_error(error, error_size,
                             "CFlow vector materialization failed");
      goto done;
    }
  } else {
    if (!ts_cflow_runtime_result_to_list(&result, out)) {
      ts_cflow_runtime_error(error, error_size,
                             "CFlow list materialization failed");
      goto done;
    }
  }
  status = TS_CFLOW_RUNTIME_HANDLED;

done:
  cflow_result_destroy(&result);
  cflow_plan_destroy(&plan);
  ts_cflow_lowered_pipeline_destroy(&lowered);
  free(input);
  return status;
}