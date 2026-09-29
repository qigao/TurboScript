#include "turbo_script_cflow_runtime.h"

#include "../turbo_script_internal.h"
#include "exprtk.h"
#include "turbo_script_cflow_lower.h"
#include "turbo_script_cflow_text.h"
#include "turbo_script_cflow_text_kernel.h"
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

typedef enum ts_cflow_text_terminal_e {
  TS_CFLOW_TEXT_TERMINAL_NONE = 0,
  TS_CFLOW_TEXT_TERMINAL_COUNT,
  TS_CFLOW_TEXT_TERMINAL_COLLECT,
  TS_CFLOW_TEXT_TERMINAL_TO_LIST
} ts_cflow_text_terminal_t;

typedef enum ts_cflow_text_source_kind_e {
  TS_CFLOW_TEXT_SOURCE_NONE = 0,
  TS_CFLOW_TEXT_SOURCE_LINES,
  TS_CFLOW_TEXT_SOURCE_SPLIT
} ts_cflow_text_source_kind_t;

typedef struct ts_cflow_text_match_s {
  ts_cflow_text_terminal_t terminal;
  ts_cflow_text_source_kind_t source_kind;
  const exprtk_node_t *text_expr;
  const exprtk_node_t *separator_expr;
} ts_cflow_text_match_t;

static bool ts_cflow_runtime_text_terminal_match(
    const exprtk_node_t *expr, ts_cflow_text_match_t *out) {
  const exprtk_node_t *source_call;
  const exprtk_node_t *text_call;
  const exprtk_node_t *stream_ns;
  ts_cflow_text_match_t match = {0};

  if (!expr || !out || expr->type != EXPRTK_NODE_MEMBER_CALL ||
      !expr->data.member_call.method ||
      expr->data.member_call.arg_count != 0u)
    return false;

  if (strcmp(expr->data.member_call.method, "count") == 0)
    match.terminal = TS_CFLOW_TEXT_TERMINAL_COUNT;
  else if (strcmp(expr->data.member_call.method, "collect") == 0)
    match.terminal = TS_CFLOW_TEXT_TERMINAL_COLLECT;
  else if (strcmp(expr->data.member_call.method, "toList") == 0)
    match.terminal = TS_CFLOW_TEXT_TERMINAL_TO_LIST;
  else
    return false;

  source_call = expr->data.member_call.object;
  if (!source_call || source_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !source_call->data.member_call.method)
    return false;

  if (strcmp(source_call->data.member_call.method, "lines") == 0 &&
      source_call->data.member_call.arg_count == 0u) {
    match.source_kind = TS_CFLOW_TEXT_SOURCE_LINES;
  } else if (strcmp(source_call->data.member_call.method, "split") == 0 &&
             source_call->data.member_call.arg_count == 1u) {
    match.source_kind = TS_CFLOW_TEXT_SOURCE_SPLIT;
    match.separator_expr = source_call->data.member_call.args[0];
  } else {
    return false;
  }

  text_call = source_call->data.member_call.object;
  if (!text_call || text_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !text_call->data.member_call.method ||
      strcmp(text_call->data.member_call.method, "text") != 0 ||
      text_call->data.member_call.arg_count != 1u)
    return false;

  stream_ns = text_call->data.member_call.object;
  if (!stream_ns || stream_ns->type != EXPRTK_NODE_VARIABLE ||
      !stream_ns->data.variable.name ||
      strcmp(stream_ns->data.variable.name, "stream") != 0)
    return false;

  match.text_expr = text_call->data.member_call.args[0];
  *out = match;
  return true;
}

static bool ts_cflow_runtime_text_result_to_list(const cflow_result *result,
                                                 exprtk_value_t *out) {
  exprtk_value_t list;
  const ts_cflow_line_slice_t *slices;

  if (!result || !out ||
      !cmeta_type_equal(result->type, ts_cflow_line_slice_type()) ||
      (result->count != 0u && !result->data))
    return false;

  list = exprtk_val_list_empty();
  slices = (const ts_cflow_line_slice_t *)result->data;
  for (size_t i = 0u; i < result->count; ++i) {
    exprtk_value_t item = exprtk_val_str(
        vstr_from_buf((char *)slices[i].data, slices[i].len));
    if (exprtk_list_push(&list, item) != 0) {
      exprtk_value_destroy(&list);
      return false;
    }
  }
  *out = list;
  return true;
}

static ts_cflow_runtime_status_t ts_cflow_runtime_try_text_terminal(
    turbo_script_ctx_t *ctx, const exprtk_node_t *expr, exprtk_value_t *out,
    char *error, size_t error_size) {
  ts_cflow_text_match_t match = {0};
  exprtk_value_t text_value = {.type = EXPRTK_VAL_NULL};
  exprtk_value_t separator_value = {.type = EXPRTK_VAL_NULL};
  ts_cflow_text_lines_source_t source = {0};
  cflow_graph graph = {0};
  cflow_plan plan = {0};
  cflow_result result = {0};
  const char *cflow_error = NULL;
  const char *text_data = NULL;
  const char *separator_data = NULL;
  size_t text_len = 0u;
  size_t separator_len = 0u;
  bool plan_ready = false;
  bool source_ready = false;
  bool result_ready = false;
  ts_cflow_runtime_status_t status = TS_CFLOW_RUNTIME_ERROR;

  graph.root = CMETA_INVALID_ID;
  if (!ts_cflow_runtime_text_terminal_match(expr, &match))
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;

  text_value = turbo_script_mir_eval_node(match.text_expr, &ctx->env);
  if (ctx->env.aborted || ctx->env.flow == exprtk_FLOW_THROW) {
    ts_cflow_runtime_error(
        error, error_size,
        ctx->env.error_msg[0] ? ctx->env.error_msg
                              : "TurboScript text source evaluation failed");
    goto done;
  }

  /* Match stream.text(non-string): the legacy factory substitutes empty text. */
  if (text_value.type == EXPRTK_VAL_STRING) {
    text_data = text_value.data.string.data;
    text_len = text_value.data.string.len;
  } else {
    text_data = "";
    text_len = 0u;
  }

  if (match.source_kind == TS_CFLOW_TEXT_SOURCE_SPLIT) {
    separator_value =
        turbo_script_mir_eval_node(match.separator_expr, &ctx->env);
    if (ctx->env.aborted || ctx->env.flow == exprtk_FLOW_THROW) {
      ts_cflow_runtime_error(
          error, error_size,
          ctx->env.error_msg[0] ? ctx->env.error_msg
                                : "TurboScript split separator evaluation failed");
      goto done;
    }

    if (separator_value.type == EXPRTK_VAL_STRING) {
      separator_data = separator_value.data.string.data;
      separator_len = separator_value.data.string.len;
      if (!ts_cflow_text_split_source_init(
              &source, text_data, text_len,
              separator_data, separator_len)) {
        ts_cflow_runtime_error(
            error, error_size,
            "CFlow text split source materialization failed");
        goto done;
      }
    } else {
      /* Legacy split(non-string) returns an empty list/stream. */
      memset(&source, 0, sizeof(source));
    }
  } else if (!ts_cflow_text_lines_source_init(&source, text_data, text_len)) {
    ts_cflow_runtime_error(error, error_size,
                           "CFlow text-line source materialization failed");
    goto done;
  }
  source_ready = true;

  if (!ts_cflow_text_lines_plan_compile(&graph, &plan, &cflow_error)) {
    ts_cflow_runtime_error(
        error, error_size,
        cflow_error ? cflow_error : "CFlow text-slice Plan compilation failed");
    goto done;
  }
  plan_ready = true;

  if (!ts_cflow_text_lines_plan_eval(&plan, &source, &result, &cflow_error)) {
    ts_cflow_runtime_error(
        error, error_size,
        cflow_error ? cflow_error : "CFlow text-slice Plan execution failed");
    goto done;
  }
  result_ready = true;

  if (match.terminal == TS_CFLOW_TEXT_TERMINAL_COUNT) {
    *out = exprtk_val_num((double)result.count);
  } else if (!ts_cflow_runtime_text_result_to_list(&result, out)) {
    ts_cflow_runtime_error(error, error_size,
                           "CFlow text-slice list materialization failed");
    goto done;
  }

  status = TS_CFLOW_RUNTIME_HANDLED;

done:
  if (result_ready) cflow_result_destroy(&result);
  if (plan_ready) {
    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
  }
  if (source_ready) ts_cflow_text_lines_source_destroy(&source);
  exprtk_value_destroy(&separator_value);
  exprtk_value_destroy(&text_value);
  return status;
}

typedef struct ts_cflow_text_length_map_match_s {
  ts_cflow_terminal_t terminal;
  const exprtk_node_t *text_expr;
  const exprtk_node_t *lambda;
} ts_cflow_text_length_map_match_t;

static bool ts_cflow_runtime_text_length_map_match(
    const exprtk_node_t *expr,
    ts_cflow_text_length_map_match_t *out) {
  const exprtk_node_t *map_call;
  const exprtk_node_t *lines_call;
  const exprtk_node_t *text_call;
  const exprtk_node_t *stream_ns;
  ts_cflow_text_length_map_match_t match = {0};

  if (!expr || !out || expr->type != EXPRTK_NODE_MEMBER_CALL ||
      !expr->data.member_call.method ||
      expr->data.member_call.arg_count != 0u)
    return false;

  if (strcmp(expr->data.member_call.method, "toVector") == 0)
    match.terminal = TS_CFLOW_TERMINAL_TO_VECTOR;
  else if (strcmp(expr->data.member_call.method, "toList") == 0)
    match.terminal = TS_CFLOW_TERMINAL_TO_LIST;
  else if (strcmp(expr->data.member_call.method, "collect") == 0)
    match.terminal = TS_CFLOW_TERMINAL_COLLECT;
  else
    return false;

  map_call = expr->data.member_call.object;
  if (!map_call || map_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !map_call->data.member_call.method ||
      strcmp(map_call->data.member_call.method, "map") != 0 ||
      map_call->data.member_call.arg_count != 1u)
    return false;

  lines_call = map_call->data.member_call.object;
  if (!lines_call || lines_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !lines_call->data.member_call.method ||
      strcmp(lines_call->data.member_call.method, "lines") != 0 ||
      lines_call->data.member_call.arg_count != 0u)
    return false;

  text_call = lines_call->data.member_call.object;
  if (!text_call || text_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !text_call->data.member_call.method ||
      strcmp(text_call->data.member_call.method, "text") != 0 ||
      text_call->data.member_call.arg_count != 1u)
    return false;

  stream_ns = text_call->data.member_call.object;
  if (!stream_ns || stream_ns->type != EXPRTK_NODE_VARIABLE ||
      !stream_ns->data.variable.name ||
      strcmp(stream_ns->data.variable.name, "stream") != 0)
    return false;

  match.text_expr = text_call->data.member_call.args[0];
  match.lambda = map_call->data.member_call.args[0];
  *out = match;
  return true;
}

typedef struct ts_cflow_text_nonempty_filter_match_s {
  ts_cflow_terminal_t terminal;
  const exprtk_node_t *text_expr;
  const exprtk_node_t *lambda;
} ts_cflow_text_nonempty_filter_match_t;

static bool ts_cflow_runtime_text_nonempty_filter_match(
    const exprtk_node_t *expr,
    ts_cflow_text_nonempty_filter_match_t *out) {
  const exprtk_node_t *filter_call;
  const exprtk_node_t *lines_call;
  const exprtk_node_t *text_call;
  const exprtk_node_t *stream_ns;
  ts_cflow_text_nonempty_filter_match_t match = {0};

  if (!expr || !out || expr->type != EXPRTK_NODE_MEMBER_CALL ||
      !expr->data.member_call.method ||
      expr->data.member_call.arg_count != 0u)
    return false;

  if (strcmp(expr->data.member_call.method, "count") == 0)
    match.terminal = TS_CFLOW_TERMINAL_COUNT;
  else if (strcmp(expr->data.member_call.method, "collect") == 0)
    match.terminal = TS_CFLOW_TERMINAL_COLLECT;
  else if (strcmp(expr->data.member_call.method, "toList") == 0)
    match.terminal = TS_CFLOW_TERMINAL_TO_LIST;
  else
    return false;

  filter_call = expr->data.member_call.object;
  if (!filter_call ||
      filter_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !filter_call->data.member_call.method ||
      strcmp(filter_call->data.member_call.method, "filter") != 0 ||
      filter_call->data.member_call.arg_count != 1u)
    return false;

  lines_call = filter_call->data.member_call.object;
  if (!lines_call ||
      lines_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !lines_call->data.member_call.method ||
      strcmp(lines_call->data.member_call.method, "lines") != 0 ||
      lines_call->data.member_call.arg_count != 0u)
    return false;

  text_call = lines_call->data.member_call.object;
  if (!text_call ||
      text_call->type != EXPRTK_NODE_MEMBER_CALL ||
      !text_call->data.member_call.method ||
      strcmp(text_call->data.member_call.method, "text") != 0 ||
      text_call->data.member_call.arg_count != 1u)
    return false;

  stream_ns = text_call->data.member_call.object;
  if (!stream_ns || stream_ns->type != EXPRTK_NODE_VARIABLE ||
      !stream_ns->data.variable.name ||
      strcmp(stream_ns->data.variable.name, "stream") != 0)
    return false;

  match.text_expr = text_call->data.member_call.args[0];
  match.lambda = filter_call->data.member_call.args[0];
  *out = match;
  return true;
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

static ts_cflow_runtime_status_t
ts_cflow_runtime_try_text_length_map_terminal(
    turbo_script_ctx_t *ctx,
    const exprtk_node_t *expr,
    exprtk_value_t *out,
    char *error,
    size_t error_size) {
  ts_cflow_text_length_map_match_t match = {0};
  ts_cflow_text_kernel_binding_t binding = {0};
  exprtk_value_t text_value = {.type = EXPRTK_VAL_NULL};
  ts_cflow_text_lines_source_t source = {0};
  cflow_graph graph = {0};
  cflow_plan plan = {0};
  cflow_result result = {0};
  const char *kernel_error = NULL;
  const char *text_data = "";
  size_t text_len = 0u;
  bool binding_ready = false;
  bool source_ready = false;
  bool graph_ready = false;
  bool plan_ready = false;
  bool result_ready = false;
  ts_cflow_runtime_status_t status = TS_CFLOW_RUNTIME_ERROR;

  graph.root = CMETA_INVALID_ID;
  if (!ts_cflow_runtime_text_length_map_match(expr, &match))
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;

  /*
   * Lambda binding is the admission boundary. Unsupported string MAP shapes
   * remain on the legacy facade without evaluating the source expression.
   */
  if (!ts_cflow_text_length_map_bind(
          match.lambda, &binding, &kernel_error))
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;
  binding_ready = true;

  text_value = turbo_script_mir_eval_node(match.text_expr, &ctx->env);
  if (ctx->env.aborted || ctx->env.flow == exprtk_FLOW_THROW) {
    ts_cflow_runtime_error(
        error, error_size,
        ctx->env.error_msg[0] ? ctx->env.error_msg
                              : "TurboScript text MAP source evaluation failed");
    goto done;
  }

  if (text_value.type == EXPRTK_VAL_STRING) {
    text_data = text_value.data.string.data;
    text_len = text_value.data.string.len;
  }

  if (!ts_cflow_text_lines_source_init(
          &source, text_data, text_len)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow text MAP source materialization failed");
    goto done;
  }
  source_ready = true;

  cflow_graph_init(&graph, ts_cflow_line_slice_type());
  if (graph.error) {
    ts_cflow_runtime_error(
        error, error_size, graph.error);
    goto done;
  }
  graph_ready = true;

  if (!ts_cflow_text_length_map_graph_add(
          &graph, &binding, &kernel_error)) {
    ts_cflow_runtime_error(
        error, error_size,
        kernel_error ? kernel_error
                     : "CFlow typed text MAP admission failed");
    goto done;
  }

  if (!cflow_plan_compile_surface(&plan, &graph, NULL)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text MAP Plan compilation failed");
    goto done;
  }
  plan_ready = true;

  if (!cflow_plan_eval_array(
          &plan, source.items, source.count, &result)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text MAP Plan execution failed");
    goto done;
  }
  result_ready = true;

  if (!ts_cflow_runtime_result_is_double(&result)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text MAP produced an invalid result");
    goto done;
  }

  if (match.terminal == TS_CFLOW_TERMINAL_TO_VECTOR) {
    if (!ts_cflow_runtime_result_to_vector(ctx, &result, out)) {
      ts_cflow_runtime_error(
          error, error_size,
          "CFlow typed text MAP vector materialization failed");
      goto done;
    }
  } else {
    if (!ts_cflow_runtime_result_to_list(&result, out)) {
      ts_cflow_runtime_error(
          error, error_size,
          "CFlow typed text MAP list materialization failed");
      goto done;
    }
  }

  status = TS_CFLOW_RUNTIME_HANDLED;

done:
  if (result_ready) cflow_result_destroy(&result);
  if (plan_ready) cflow_plan_destroy(&plan);
  if (graph_ready) cflow_graph_destroy(&graph);
  if (source_ready) ts_cflow_text_lines_source_destroy(&source);
  exprtk_value_destroy(&text_value);
  if (binding_ready)
    ts_cflow_text_kernel_binding_destroy(&binding);
  return status;
}

static ts_cflow_runtime_status_t
ts_cflow_runtime_try_text_nonempty_filter_terminal(
    turbo_script_ctx_t *ctx,
    const exprtk_node_t *expr,
    exprtk_value_t *out,
    char *error,
    size_t error_size) {
  ts_cflow_text_nonempty_filter_match_t match = {0};
  ts_cflow_text_kernel_binding_t binding = {0};
  exprtk_value_t text_value = {.type = EXPRTK_VAL_NULL};
  ts_cflow_text_lines_source_t source = {0};
  cflow_graph graph = {0};
  cflow_plan plan = {0};
  cflow_result result = {0};
  const char *kernel_error = NULL;
  const char *text_data = "";
  size_t text_len = 0u;
  bool binding_ready = false;
  bool source_ready = false;
  bool graph_ready = false;
  bool plan_ready = false;
  bool result_ready = false;
  ts_cflow_runtime_status_t status = TS_CFLOW_RUNTIME_ERROR;

  graph.root = CMETA_INVALID_ID;
  if (!ts_cflow_runtime_text_nonempty_filter_match(expr, &match))
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;

  /*
   * Predicate binding is the admission boundary. Unsupported string FILTER
   * shapes stay on the legacy facade without evaluating the source expression.
   */
  if (!ts_cflow_text_nonempty_filter_bind(
          match.lambda, &binding, &kernel_error))
    return TS_CFLOW_RUNTIME_NOT_APPLICABLE;
  binding_ready = true;

  text_value = turbo_script_mir_eval_node(match.text_expr, &ctx->env);
  if (ctx->env.aborted || ctx->env.flow == exprtk_FLOW_THROW) {
    ts_cflow_runtime_error(
        error, error_size,
        ctx->env.error_msg[0] ? ctx->env.error_msg
                              : "TurboScript text FILTER source evaluation failed");
    goto done;
  }

  if (text_value.type == EXPRTK_VAL_STRING) {
    text_data = text_value.data.string.data;
    text_len = text_value.data.string.len;
  }

  if (!ts_cflow_text_lines_source_init(
          &source, text_data, text_len)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow text FILTER source materialization failed");
    goto done;
  }
  source_ready = true;

  cflow_graph_init(&graph, ts_cflow_line_slice_type());
  if (graph.error) {
    ts_cflow_runtime_error(error, error_size, graph.error);
    goto done;
  }
  graph_ready = true;

  if (!ts_cflow_text_nonempty_filter_graph_add(
          &graph, &binding, &kernel_error)) {
    ts_cflow_runtime_error(
        error, error_size,
        kernel_error ? kernel_error
                     : "CFlow typed text FILTER admission failed");
    goto done;
  }

  if (!cflow_plan_compile_surface(&plan, &graph, NULL)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text FILTER Plan compilation failed");
    goto done;
  }
  plan_ready = true;

  if (!cflow_plan_eval_array(
          &plan, source.items, source.count, &result)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text FILTER Plan execution failed");
    goto done;
  }
  result_ready = true;

  if (!cmeta_type_equal(
          result.type, ts_cflow_line_slice_type()) ||
      (result.count != 0u && !result.data)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text FILTER produced an invalid result");
    goto done;
  }

  if (match.terminal == TS_CFLOW_TERMINAL_COUNT) {
    *out = exprtk_val_num((double)result.count);
  } else if (!ts_cflow_runtime_text_result_to_list(&result, out)) {
    ts_cflow_runtime_error(
        error, error_size,
        "CFlow typed text FILTER list materialization failed");
    goto done;
  }

  status = TS_CFLOW_RUNTIME_HANDLED;

done:
  if (result_ready) cflow_result_destroy(&result);
  if (plan_ready) cflow_plan_destroy(&plan);
  if (graph_ready) cflow_graph_destroy(&graph);
  if (source_ready) ts_cflow_text_lines_source_destroy(&source);
  exprtk_value_destroy(&text_value);
  if (binding_ready)
    ts_cflow_text_kernel_binding_destroy(&binding);
  return status;
}

static int ts_cflow_runtime_value_to_double(
    const exprtk_value_t *value, double *out) {
  if (!value || !out) return 0;
  if (value->type == EXPRTK_VAL_NUMBER) {
    *out = value->data.number;
    return 1;
  }
  if (value->type == EXPRTK_VAL_INTEGER) {
    *out = (double)value->data.integer;
    return 1;
  }
  return 0;
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

    if (value.type == EXPRTK_VAL_VECTOR) {
      count = value.data.vector.size;
      if (count > 0u) {
        data = (double *)malloc(count * sizeof(*data));
        if (!data) return 0;
        memcpy(data, value.data.vector.data, count * sizeof(*data));
      }
    } else if (value.type == EXPRTK_VAL_LIST) {
      count = value.data.list.count;
      if (count > 0u) {
        data = (double *)malloc(count * sizeof(*data));
        if (!data) return 0;
      }
      for (size_t i = 0; i < count; ++i) {
        if (!ts_cflow_runtime_value_to_double(
                &value.data.list.items[i], &data[i])) {
          free(data);
          return 0;
        }
      }
    } else {
      return 0;
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

  {
    ts_cflow_runtime_status_t text_filter_status =
        ts_cflow_runtime_try_text_nonempty_filter_terminal(
            ctx, expr, out, error, error_size);
    if (text_filter_status != TS_CFLOW_RUNTIME_NOT_APPLICABLE)
      return text_filter_status;
  }

  {
    ts_cflow_runtime_status_t text_map_status =
        ts_cflow_runtime_try_text_length_map_terminal(
            ctx, expr, out, error, error_size);
    if (text_map_status != TS_CFLOW_RUNTIME_NOT_APPLICABLE)
      return text_map_status;
  }

  {
    ts_cflow_runtime_status_t text_status =
        ts_cflow_runtime_try_text_terminal(
            ctx, expr, out, error, error_size);
    if (text_status != TS_CFLOW_RUNTIME_NOT_APPLICABLE)
      return text_status;
  }

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