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
  TS_CFLOW_TERMINAL_COUNT
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
      strcmp(expr->data.member_call.method, "count") == 0 &&