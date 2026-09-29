#include "tinytest.h"
#include "exprtk.h"
#include "turbo_script_cflow_mir_kernel.h"

#include <cflow/graph.h>
#include <cflow/plan.h>
#include <cmeta/cmeta.h>

static exprtk_node_t *ts_kernel_test_lambda(const char *source,
                                            exprtk_node_t **root_out) {
  exprtk_node_t *root = exprtk_parse(source, 0);
  if (root_out) *root_out = root;
  if (!root || root->type != EXPRTK_NODE_BLOCK ||
      root->data.block.count != 1u)
    return NULL;
  exprtk_node_t *lambda = root->data.block.statements[0];
  return lambda && lambda->type == EXPRTK_NODE_FUNCTION_EXPRESSION
             ? lambda
             : NULL;
}

spec("TurboScript CFlow MIR kernels") {
  it("executes filter and map MIR kernels through a compiled CFlow plan") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *filter_root = NULL;
    exprtk_node_t *map_root = NULL;
    exprtk_node_t *filter_lambda =
        ts_kernel_test_lambda("x => x > 0", &filter_root);
    exprtk_node_t *map_lambda =
        ts_kernel_test_lambda("x => x * 2", &map_root);
    ts_cflow_mir_kernel_binding_t filter_binding = {0};
    ts_cflow_mir_kernel_binding_t map_binding = {0};
    cflow_graph graph = {0};
    cflow_plan plan = {0};
    cflow_result result = {0};
    cflow_result repeated = {0};
    const cflow_subgraph *surface = NULL;
    const char *error = NULL;
    const double input[] = {-2.0, 0.0, 3.0};
    const double repeated_input[] = {1.0, 2.0};

    graph.root = CMETA_INVALID_ID;
    check_not_null(ctx);
    check_not_null(filter_lambda);
    check_not_null(map_lambda);

    check_true(ts_cflow_mir_kernel_bind(
        ctx, filter_lambda, TS_CMETA_LAMBDA_FILTER,
        &filter_binding, &error));
    check_null(error);
    check_true(ts_cflow_mir_kernel_bind(
        ctx, map_lambda, TS_CMETA_LAMBDA_MAP,
        &map_binding, &error));
    check_null(error);
    check_true(cmeta_callable_contract_valid(filter_binding.callable));
    check_true(cmeta_callable_contract_valid(map_binding.callable));

    cflow_graph_init(&graph, &cmeta_type_double);
    check_null(graph.error);
    check_true(cflow_graph_add(
        &graph, CFLOW_OP_FILTER, filter_binding.callable, NULL));
    check_true(cflow_graph_add(
        &graph, CFLOW_OP_MAP, map_binding.callable, NULL));

    surface = cflow_graph_subgraph(&graph, graph.root);
    check_not_null(surface);
    check((surface->node_count) == ((size_t)3u));
    check((surface->nodes[0].op) == (CFLOW_OP_INPUT));
    check((surface->nodes[1].op) == (CFLOW_OP_FILTER));
    check((surface->nodes[2].op) == (CFLOW_OP_MAP));
    check_true(cmeta_callable_same(
        surface->nodes[1].fn, filter_binding.callable));
    check_true(cmeta_callable_same(
        surface->nodes[2].fn, map_binding.callable));

    check_true(cflow_plan_compile_surface(&plan, &graph, NULL));
    check_true(cflow_plan_eval_array(
        &plan, input, sizeof(input) / sizeof(input[0]), &result));

    check_true(cmeta_type_equal(result.type, &cmeta_type_double));
    check((result.count) == ((size_t)1u));
    check_not_null(result.data);
    check((((const double *)result.data)[0]) == (6.0));

    check_true(cflow_plan_eval_array(
        &plan, repeated_input,
        sizeof(repeated_input) / sizeof(repeated_input[0]), &repeated));
    check_true(cmeta_type_equal(repeated.type, &cmeta_type_double));
    check((repeated.count) == ((size_t)2u));
    check((((const double *)repeated.data)[0]) == (2.0));
    check((((const double *)repeated.data)[1]) == (4.0));

    /* CFlow snapshots copy callable by value but borrow the captured kernel
     * pointer. Destroy all result/plan/graph users before the TurboScript owner. */
    cflow_result_destroy(&repeated);
    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
    ts_cflow_mir_kernel_binding_destroy(&map_binding);
    ts_cflow_mir_kernel_binding_destroy(&filter_binding);
    exprtk_free(map_root);
    exprtk_free(filter_root);
    turbo_script_free(ctx);
  }

  it("executes a seeded reduce MIR kernel through a reusable CFlow plan") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda =
        ts_kernel_test_lambda("(acc, x) => acc + x", &root);
    ts_cflow_mir_kernel_binding_t binding = {0};
    cflow_graph graph = {0};
    cflow_plan plan = {0};
    cflow_result result = {0};
    cflow_result empty = {0};
    const char *error = NULL;
    const double seed = 10.0;
    const double input[] = {1.0, 2.0, 3.0};

    graph.root = CMETA_INVALID_ID;
    check_not_null(ctx);
    check_not_null(lambda);
    check_true(ts_cflow_mir_kernel_bind(
        ctx, lambda, TS_CMETA_LAMBDA_REDUCE, &binding, &error));
    check_null(error);
    check_true(cmeta_callable_contract_valid(binding.callable));

    cflow_graph_init(&graph, &cmeta_type_double);
    check_null(graph.error);
    check_true(cflow_graph_reduce_seeded(&graph, binding.callable, &seed));
    check_true(cflow_plan_compile_surface(&plan, &graph, NULL));

    check_true(cflow_plan_eval_array(
        &plan, input, sizeof(input) / sizeof(input[0]), &result));
    check((result.count) == ((size_t)1u));
    check_not_null(result.data);
    check((((const double *)result.data)[0]) == (16.0));

    check_true(cflow_plan_eval_array(&plan, NULL, 0u, &empty));
    check((empty.count) == ((size_t)1u));
    check_not_null(empty.data);
    check((((const double *)empty.data)[0]) == (10.0));

    cflow_result_destroy(&empty);
    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
    ts_cflow_mir_kernel_binding_destroy(&binding);
    exprtk_free(root);
    turbo_script_free(ctx);
  }

  it("rejects captured lambdas before MIR kernel compilation") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda =
        ts_kernel_test_lambda("x => x * factor", &root);
    ts_cflow_mir_kernel_binding_t binding = {0};
    const char *error = NULL;

    check_not_null(ctx);
    check_not_null(lambda);
    check_false(ts_cflow_mir_kernel_bind(
        ctx, lambda, TS_CMETA_LAMBDA_MAP, &binding, &error));
    check_not_null(error);
    check_null(binding.owner);

    exprtk_free(root);
    turbo_script_free(ctx);
  }
}