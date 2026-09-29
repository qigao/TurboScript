#include "../src/turbo_script_internal.h"
#include "../src/graph/turbo_script_cflow_lower.h"

#include "exprtk.h"
#include "tinytest.h"

#include <cflow/adapters.h>
#include <cflow/lower.h>
#include <cflow/opt.h>
#include <cflow/plan.h>

#include <math.h>
#include <string.h>

static exprtk_node_t *ts_cflow_qualification_single_expr(
    turbo_script_ctx_t *ctx,
    const char *source,
    exprtk_node_t **root_out) {
  exprtk_node_t *root = turbo_script_parse_with_error(ctx, source);
  if (root_out) *root_out = root;
  if (!root) return NULL;
  if (root->type == EXPRTK_NODE_BLOCK && root->data.block.count == 1u)
    return root->data.block.statements[0];
  return root;
}

static const exprtk_node_t *ts_cflow_qualification_count_pipeline(
    const exprtk_node_t *expr) {
  if (!expr || expr->type != EXPRTK_NODE_MEMBER_CALL ||
      !expr->data.member_call.method ||
      strcmp(expr->data.member_call.method, "count") != 0 ||
      expr->data.member_call.arg_count != 0u)
    return NULL;
  return expr->data.member_call.object;
}

static const cflow_node *ts_cflow_qualification_find_op(
    const cflow_graph *graph,
    cflow_op op) {
  const cflow_subgraph *root;
  if (!graph) return NULL;
  root = cflow_graph_subgraph(graph, graph->root);
  if (!root) return NULL;
  for (size_t i = 0u; i < root->node_count; ++i) {
    if (root->nodes[i].op == op) return &root->nodes[i];
  }
  return NULL;
}

spec("TurboScript CFlow qualification") {
  it("records stable optimizer structure for a real two-map pipeline") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    const double input[] = {1.0, 2.0, 3.0, 4.0};
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr;
    const exprtk_node_t *pipeline;
    ts_cflow_lowered_pipeline_t lowered = {0};
    cflow_graph normalized = {0};
    cflow_graph optimized = {0};
    cflow_opt_stats stats = {0};
    cflow_plan plan = {0};
    cflow_plan_compile_stats compile_stats = {0};
    cflow_result direct = {0};
    cflow_result compiled = {0};
    exprtk_value_t legacy = {.type = EXPRTK_VAL_NULL};
    const cflow_subgraph *surface_root;
    const cflow_subgraph *optimized_root;
    const cflow_node *fused_map;
    const char *error = NULL;
    const double expected[] = {4.0, 6.0, 8.0, 10.0};

    normalized.root = CMETA_INVALID_ID;
    optimized.root = CMETA_INVALID_ID;
    check_not_null(ctx);
    exprtk_env_set(
        &ctx->env, "values",
        exprtk_val_vec((double *)input, sizeof(input) / sizeof(input[0])));

    expr = ts_cflow_qualification_single_expr(
        ctx,
        "stream.of(values)"
        ".map(x => x + 1)"
        ".map(x => x * 2)"
        ".count();",
        &root);
    check_not_null(expr);
    pipeline = ts_cflow_qualification_count_pipeline(expr);
    check_not_null(pipeline);

    legacy = exprtk_eval(expr, &ctx->env);
    check_equal(legacy.type, EXPRTK_VAL_NUMBER);
    check(fabs(legacy.data.number - 4.0) <= 1e-9);

    check_true(ts_cflow_lower_pipeline_executable(
        ctx, pipeline, &lowered, &error));
    check_null(error);
    check_true(lowered.executable);
    check_equal(lowered.kernel_binding_count, (size_t)2u);

    surface_root = cflow_graph_subgraph(
        &lowered.graph, lowered.graph.root);
    check_not_null(surface_root);
    check_equal(surface_root->node_count, (size_t)3u);
    check_equal(surface_root->nodes[0].op, CFLOW_OP_INPUT);
    check_equal(surface_root->nodes[1].op, CFLOW_OP_MAP);
    check_equal(surface_root->nodes[2].op, CFLOW_OP_MAP);

    check_true(cflow_graph_normalize(&normalized, &lowered.graph));
    check_true(cflow_graph_optimize(
        &optimized, &normalized,
        (cflow_opt_options){CMETA_OPT_DEFAULT}, &stats));
    check_equal(stats.nodes_before, (size_t)3u);
    check_equal(stats.nodes_after, (size_t)2u);
    check_equal(stats.map_nodes_fused, (size_t)1u);

    optimized_root = cflow_graph_subgraph(
        &optimized, optimized.root);
    check_not_null(optimized_root);
    check_equal(optimized_root->node_count, (size_t)2u);
    check_equal(optimized_root->nodes[0].op, CFLOW_OP_INPUT);
    check_equal(optimized_root->nodes[1].op, CFLOW_OP_MAP);
    fused_map = ts_cflow_qualification_find_op(
        &optimized, CFLOW_OP_MAP);
    check_not_null(fused_map);
    check_equal(fused_map->fn_chain_count, (size_t)2u);
    check_true(cmeta_type_equal(
        fused_map->input_type, &cmeta_type_double));
    check_true(cmeta_type_equal(
        fused_map->output_type, &cmeta_type_double));

    check_true(cflow_eval_array(
        &normalized, input,
        sizeof(input) / sizeof(input[0]), &direct));
    check_true(cflow_plan_compile(
        &plan, &optimized, &compile_stats));
    check_equal(compile_stats.graph_nodes, (size_t)2u);
    check_equal(compile_stats.map_callbacks, (size_t)2u);
    check_true(cflow_plan_eval_array(
        &plan, input,
        sizeof(input) / sizeof(input[0]), &compiled));

    check_equal(direct.count, compiled.count);
    check_equal(compiled.count, (size_t)4u);
    check_true(cmeta_type_equal(
        direct.type, &cmeta_type_double));
    check_true(cmeta_type_equal(
        compiled.type, &cmeta_type_double));
    for (size_t i = 0u; i < 4u; ++i) {
      check(fabs(((const double *)direct.data)[i] - expected[i]) <= 1e-9);
      check(fabs(((const double *)compiled.data)[i] - expected[i]) <= 1e-9);
    }

    cflow_result_destroy(&compiled);
    cflow_result_destroy(&direct);
    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&optimized);
    cflow_graph_destroy(&normalized);
    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_value_destroy(&legacy);
    exprtk_free(root);
    turbo_script_free(ctx);
  }

  it("reuses one compiled Plan without rebuilding MIR kernel bindings") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    double input[64];
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr;
    const exprtk_node_t *pipeline;
    ts_cflow_lowered_pipeline_t lowered = {0};
    cflow_plan plan = {0};
    const char *error = NULL;

    check_not_null(ctx);
    for (size_t i = 0u; i < 64u; ++i)
      input[i] = (double)((int)(i % 4u) - 1);
    exprtk_env_set(
        &ctx->env, "values", exprtk_val_vec(input, 64u));

    expr = ts_cflow_qualification_single_expr(
        ctx,
        "stream.of(values)"
        ".filter(x => x > 0)"
        ".map(x => x * 2)"
        ".count();",
        &root);
    check_not_null(expr);
    pipeline = ts_cflow_qualification_count_pipeline(expr);
    check_not_null(pipeline);

    check_true(ts_cflow_lower_pipeline_executable(
        ctx, pipeline, &lowered, &error));
    check_null(error);
    check_equal(lowered.kernel_binding_count, (size_t)2u);
    check_true(cflow_plan_compile_surface(
        &plan, &lowered.graph, NULL));

    for (size_t iteration = 0u; iteration < 64u; ++iteration) {
      cflow_result result = {0};
      check_true(cflow_plan_eval_array(
          &plan, input, 64u, &result));
      check_equal(result.count, (size_t)32u);
      check_true(cmeta_type_equal(
          result.type, &cmeta_type_double));
      cflow_result_destroy(&result);
    }

    cflow_plan_destroy(&plan);
    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
    turbo_script_free(ctx);
  }
}
