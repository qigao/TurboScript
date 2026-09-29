#include "turbo_script_cflow_lower.h"
#include "turbo_script_cmeta_bridge.h"
#include "exprtk_grammar.h"

#include <stdlib.h>
#include <string.h>

static void ts_cflow_destroy_kernel_bindings(
    ts_cflow_lowered_pipeline_t *out) {
  if (!out) return;
  for (size_t i = 0; i < out->kernel_binding_count; ++i)
    ts_cflow_mir_kernel_binding_destroy(&out->kernel_bindings[i]);
  free(out->kernel_bindings);
  out->kernel_bindings = NULL;
  out->kernel_binding_count = 0u;
  out->kernel_binding_capacity = 0u;
}

static void ts_cflow_release_owned_lowering(
    ts_cflow_lowered_pipeline_t *out) {
  if (!out) return;
  /* Graph/Plan-visible callable snapshots borrow MIR kernel owner pointers.
   * Release the graph before destroying those owners. */
  cflow_graph_destroy(&out->graph);
  ts_cflow_destroy_kernel_bindings(out);
  out->source_expr = NULL;
  out->reduce_seed_expr = NULL;
  out->has_reduce_seed = false;
  out->executable = false;
}

static bool ts_cflow_fail(ts_cflow_lowered_pipeline_t *out,
                          const char **error,
                          const char *message) {
  if (error) *error = message;
  ts_cflow_release_owned_lowering(out);
  return false;
}

static bool ts_cflow_reserve_kernel_binding(
    ts_cflow_lowered_pipeline_t *out, const char **error) {
  size_t capacity;
  ts_cflow_mir_kernel_binding_t *grown;

  if (!out) return false;
  if (out->kernel_binding_count < out->kernel_binding_capacity) return true;

  capacity = out->kernel_binding_capacity
                 ? out->kernel_binding_capacity * 2u
                 : 4u;
  grown = (ts_cflow_mir_kernel_binding_t *)realloc(
      out->kernel_bindings, capacity * sizeof(*grown));
  if (!grown) {
    if (error) *error = "out of memory retaining CFlow MIR kernels";
    return false;
  }
  memset(grown + out->kernel_binding_capacity, 0,
         (capacity - out->kernel_binding_capacity) * sizeof(*grown));
  out->kernel_bindings = grown;
  out->kernel_binding_capacity = capacity;
  return true;
}

static bool ts_cflow_numeric_literal_value(const exprtk_node_t *node,
                                           double *out) {
  double value;

  if (!node || !out) return false;
  if (node->type == EXPRTK_NODE_NUMBER) {
    *out = node->data.number;
    return true;
  }
  if (node->type == EXPRTK_NODE_INTEGER) {
    *out = (double)node->data.integer;
    return true;
  }

  /* TurboScript represents unary +/- as a BINARY_OP with a NULL left operand.
   * Keep executable seed admission deliberately narrower than general constant
   * folding so dynamic seed evaluation remains a language-runtime concern. */
  if (node->type != EXPRTK_NODE_BINARY_OP ||
      node->data.binary.left != NULL ||
      (node->data.binary.op != exprtk_TOKEN_PLUS &&
       node->data.binary.op != exprtk_TOKEN_MINUS) ||
      !ts_cflow_numeric_literal_value(node->data.binary.right, &value))
    return false;

  *out = node->data.binary.op == exprtk_TOKEN_MINUS ? -value : value;
  return true;
}

static bool ts_cflow_numeric_source_literal(const exprtk_node_t *node) {
  double value;
  return ts_cflow_numeric_literal_value(node, &value);
}

static bool ts_cflow_vector_source(const exprtk_node_t *node) {
  size_t i;
  if (!node || node->type != EXPRTK_NODE_VECTOR) return false;
  for (i = 0; i < node->data.vector.count; ++i) {
    if (!ts_cflow_numeric_source_literal(node->data.vector.elements[i]))
      return false;
  }
  return true;
}

static bool ts_cflow_append_callable(
    turbo_script_ctx_t *runtime_ctx, bool executable_mode,
    ts_cflow_lowered_pipeline_t *out, const exprtk_node_t *lambda,
    ts_cmeta_lambda_role_t role, cflow_op op,
    const exprtk_node_t *reduce_seed_expr, const char **error) {
  ts_cmeta_lambda_contract_t contract;
  ts_cflow_mir_kernel_binding_t binding = {0};
  const char *analysis_error = NULL;
  const char *kernel_error = NULL;
  cmeta_callable callable;
  double reduce_seed = 0.0;

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

  callable = contract.callable;
  if (executable_mode) {
    if (role == TS_CMETA_LAMBDA_REDUCE &&
        !ts_cflow_numeric_literal_value(reduce_seed_expr, &reduce_seed)) {
      if (error)
        *error =
            "executable CFlow reduce requires a numeric literal seed";
      return false;
    }
    if (!runtime_ctx) {
      if (error) *error = "executable CFlow lowering requires a runtime context";
      return false;
    }
    if (!ts_cflow_reserve_kernel_binding(out, error)) return false;
    if (!ts_cflow_mir_kernel_bind(runtime_ctx, lambda, role, &binding,
                                  &kernel_error)) {
      if (error) *error = kernel_error ? kernel_error
                                       : "MIR kernel binding failed";
      return false;
    }
    callable = binding.callable;
  }

  if ((executable_mode && role == TS_CMETA_LAMBDA_REDUCE)
          ? !cflow_graph_reduce_seeded(&out->graph, callable, &reduce_seed)
          : !cflow_graph_add(&out->graph, op, callable, NULL)) {
    if (executable_mode) ts_cflow_mir_kernel_binding_destroy(&binding);
    if (error) *error = out->graph.error ? out->graph.error
                                         : "CFlow graph operator admission failed";
    return false;
  }

  if (executable_mode)
    out->kernel_bindings[out->kernel_binding_count++] = binding;
  return true;
}

static bool ts_cflow_lower_node(turbo_script_ctx_t *runtime_ctx,
                                bool executable_mode,
                                const exprtk_node_t *expr,
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
      return ts_cflow_lower_node(runtime_ctx, executable_mode, object, out, error);
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
      return ts_cflow_lower_node(runtime_ctx, executable_mode, expr->data.member_call.args[0], out, error);
    }

    if (strcmp(name, "filter") == 0) {
      if (expr->data.member_call.arg_count != 1u) {
        if (error) *error = "stream filter requires one predicate";
        return false;
      }
      if (!ts_cflow_lower_node(runtime_ctx, executable_mode, object, out, error))
        return false;
      return ts_cflow_append_callable(
          runtime_ctx, executable_mode, out,
          expr->data.member_call.args[0], TS_CMETA_LAMBDA_FILTER,
          CFLOW_OP_FILTER, NULL, error);
    }

    if (strcmp(name, "map") == 0) {
      if (expr->data.member_call.arg_count != 1u) {
        if (error) *error = "stream map requires one mapper";
        return false;
      }
      if (!ts_cflow_lower_node(runtime_ctx, executable_mode, object, out, error))
        return false;
      return ts_cflow_append_callable(
          runtime_ctx, executable_mode, out,
          expr->data.member_call.args[0], TS_CMETA_LAMBDA_MAP,
          CFLOW_OP_MAP, NULL, error);
    }

    if (strcmp(name, "reduce") == 0) {
      if (expr->data.member_call.arg_count != 2u) {
        if (error) *error = "stream reduce requires seed and reducer";
        return false;
      }
      if (!ts_cflow_lower_node(runtime_ctx, executable_mode, object, out, error))
        return false;
      out->reduce_seed_expr = expr->data.member_call.args[0];
      out->has_reduce_seed = true;
      return ts_cflow_append_callable(
          runtime_ctx, executable_mode, out,
          expr->data.member_call.args[1], TS_CMETA_LAMBDA_REDUCE,
          CFLOW_OP_REDUCE, expr->data.member_call.args[0], error);
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
    if (!ts_cflow_lower_node(runtime_ctx, executable_mode, expr->data.function.args[0], out, error))
      return false;
    return ts_cflow_append_callable(
        runtime_ctx, executable_mode, out,
        expr->data.function.args[1], TS_CMETA_LAMBDA_FILTER,
        CFLOW_OP_FILTER, NULL, error);
  }

  if (strcmp(name, "map") == 0) {
    if (expr->data.function.arg_count != 2u) {
      if (error) *error = "map pipeline node requires source and mapper";
      return false;
    }
    if (!ts_cflow_lower_node(runtime_ctx, executable_mode, expr->data.function.args[0], out, error))
      return false;
    return ts_cflow_append_callable(
        runtime_ctx, executable_mode, out,
        expr->data.function.args[1], TS_CMETA_LAMBDA_MAP,
        CFLOW_OP_MAP, NULL, error);
  }

  if (strcmp(name, "reduce") == 0) {
    if (expr->data.function.arg_count != 3u) {
      if (error) *error =
          "reduce pipeline node requires source, seed and reducer";
      return false;
    }
    if (!ts_cflow_lower_node(runtime_ctx, executable_mode, expr->data.function.args[0], out, error))
      return false;
    out->reduce_seed_expr = expr->data.function.args[1];
    out->has_reduce_seed = true;
    return ts_cflow_append_callable(
        runtime_ctx, executable_mode, out,
        expr->data.function.args[2], TS_CMETA_LAMBDA_REDUCE,
        CFLOW_OP_REDUCE, expr->data.function.args[1], error);
  }

  if (error) *error = "function is not part of the first CFlow pipeline slice";
  return false;
}

static bool ts_cflow_lower_pipeline_impl(
    turbo_script_ctx_t *runtime_ctx, bool executable_mode,
    const exprtk_node_t *expr, ts_cflow_lowered_pipeline_t *out,
    const char **error) {
  const char *validation_error = NULL;

  if (error) *error = NULL;
  if (!out) {
    if (error) *error = "pipeline lowering requires an output object";
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->graph.root = CMETA_INVALID_ID;

  if (executable_mode && !runtime_ctx) {
    if (error) *error = "executable CFlow lowering requires a runtime context";
    return false;
  }

  if (!ts_cflow_lower_node(runtime_ctx, executable_mode, expr, out, error)) {
    ts_cflow_release_owned_lowering(out);
    return false;
  }

  if (!cflow_graph_validate(&out->graph, &validation_error))
    return ts_cflow_fail(out, error,
                         validation_error ? validation_error
                                          : "lowered CFlow graph is invalid");

  out->executable = executable_mode;
  return true;
}

bool ts_cflow_lower_pipeline(const exprtk_node_t *expr,
                             ts_cflow_lowered_pipeline_t *out,
                             const char **error) {
  return ts_cflow_lower_pipeline_impl(NULL, false, expr, out, error);
}

bool ts_cflow_lower_pipeline_executable(turbo_script_ctx_t *runtime_ctx,
                                        const exprtk_node_t *expr,
                                        ts_cflow_lowered_pipeline_t *out,
                                        const char **error) {
  return ts_cflow_lower_pipeline_impl(runtime_ctx, true, expr, out, error);
}

void ts_cflow_lowered_pipeline_destroy(ts_cflow_lowered_pipeline_t *pipeline) {
  if (!pipeline) return;
  ts_cflow_release_owned_lowering(pipeline);
  memset(pipeline, 0, sizeof(*pipeline));
  pipeline->graph.root = CMETA_INVALID_ID;
}