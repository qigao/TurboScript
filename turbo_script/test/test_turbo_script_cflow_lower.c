#include "tinytest.h"
#include "exprtk.h"
#include "turbo_script_cflow_lower.h"
#include "turbo_script_cflow_runtime.h"

#include <cflow/graph.h>
#include <cflow/plan.h>
#include <cmeta/cmeta.h>

static exprtk_node_t *ts_test_single_expr(const char *source,
                                          exprtk_node_t **root_out) {
  exprtk_node_t *root = exprtk_parse(source, 0);
  if (root_out) *root_out = root;
  if (!root || root->type != EXPRTK_NODE_BLOCK ||
      root->data.block.count != 1u)
    return NULL;
  return root->data.block.statements[0];
}

spec("TurboScript CFlow pipeline lowering") {
  it("lowers filter map reduce pipe syntax into a typed CFlow graph") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1, -2, 3, 0] "
        "|> filter(x => x > 0) "
        "|> map(x => x * 10) "
        "|> reduce(0, (acc, x) => acc + x)",
        &root);
    ts_cflow_lowered_pipeline_t lowered;
    const cflow_subgraph *sg;
    const char *error = NULL;

    check_not_null(expr);
    check_true(ts_cflow_lower_pipeline(expr, &lowered, &error));
    check_null(error);
    check_false(lowered.executable);
    check_true(lowered.has_reduce_seed);
    check_not_null(lowered.reduce_seed_expr);
    check_true(cmeta_type_equal(cflow_graph_input_type(&lowered.graph),
                                &cmeta_type_double));
    check_true(cmeta_type_equal(cflow_graph_output_type(&lowered.graph),
                                &cmeta_type_double));

    sg = cflow_graph_subgraph(&lowered.graph, lowered.graph.root);
    check_not_null(sg);
    check((sg->node_count) == (4u));
    check((sg->nodes[0].op) == (CFLOW_OP_INPUT));
    check((sg->nodes[1].op) == (CFLOW_OP_FILTER));
    check((sg->nodes[2].op) == (CFLOW_OP_MAP));
    check((sg->nodes[3].op) == (CFLOW_OP_REDUCE));

    check_true(cmeta_type_equal(sg->nodes[1].input_type,
                                &cmeta_type_double));
    check_true(cmeta_type_equal(sg->nodes[1].output_type,
                                &cmeta_type_double));
    check_true(cmeta_type_equal(sg->nodes[2].input_type,
                                &cmeta_type_double));
    check_true(cmeta_type_equal(sg->nodes[2].output_type,
                                &cmeta_type_double));
    check_true(cmeta_type_equal(sg->nodes[3].input_type,
                                &cmeta_type_double));
    check_true(cmeta_type_equal(sg->nodes[3].output_type,
                                &cmeta_type_double));

    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
  }

  it("lowers vector stream method chains into the same CFlow topology") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1, -2, 3, 0].stream()"
        ".filter(x => x > 0)"
        ".map(x => x * 10)"
        ".reduce(0, (acc, x) => acc + x)",
        &root);
    ts_cflow_lowered_pipeline_t lowered;
    const cflow_subgraph *sg;
    const char *error = NULL;

    check_not_null(expr);
    check_true(ts_cflow_lower_pipeline(expr, &lowered, &error));
    check_null(error);
    check_true(lowered.has_reduce_seed);

    sg = cflow_graph_subgraph(&lowered.graph, lowered.graph.root);
    check_not_null(sg);
    check((sg->node_count) == (4u));
    check((sg->nodes[0].op) == (CFLOW_OP_INPUT));
    check((sg->nodes[1].op) == (CFLOW_OP_FILTER));
    check((sg->nodes[2].op) == (CFLOW_OP_MAP));
    check((sg->nodes[3].op) == (CFLOW_OP_REDUCE));

    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
  }

  it("lowers stream.of numeric vectors as CFlow sources") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "stream.of([1, 2, 3]).map(x => x * 2)", &root);
    ts_cflow_lowered_pipeline_t lowered;
    const cflow_subgraph *sg;
    const char *error = NULL;

    check_not_null(expr);
    check_true(ts_cflow_lower_pipeline(expr, &lowered, &error));
    check_null(error);

    sg = cflow_graph_subgraph(&lowered.graph, lowered.graph.root);
    check_not_null(sg);
    check((sg->node_count) == (2u));
    check((sg->nodes[0].op) == (CFLOW_OP_INPUT));
    check((sg->nodes[1].op) == (CFLOW_OP_MAP));

    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
  }

  it("binds map filter lambdas to MIR kernels for executable lowering") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[-2, 0, 3].stream()"
        ".filter(x => x > 0)"
        ".map(x => x * 2)",
        &root);
    ts_cflow_lowered_pipeline_t lowered;
    cflow_plan plan = {0};
    cflow_result result = {0};
    const cflow_subgraph *sg;
    const char *error = NULL;
    const double input[] = {-2.0, 0.0, 3.0};

    check_not_null(ctx);
    check_not_null(expr);
    check_true(ts_cflow_lower_pipeline_executable(
        ctx, expr, &lowered, &error));
    check_null(error);
    check_true(lowered.executable);
    check((lowered.kernel_binding_count) == ((size_t)2u));

    sg = cflow_graph_subgraph(&lowered.graph, lowered.graph.root);
    check_not_null(sg);
    check((sg->node_count) == ((size_t)3u));
    check((sg->nodes[0].op) == (CFLOW_OP_INPUT));
    check((sg->nodes[1].op) == (CFLOW_OP_FILTER));
    check((sg->nodes[2].op) == (CFLOW_OP_MAP));
    check_true(cmeta_callable_same(
        sg->nodes[1].fn, lowered.kernel_bindings[0].callable));
    check_true(cmeta_callable_same(
        sg->nodes[2].fn, lowered.kernel_bindings[1].callable));

    check_true(cflow_plan_compile_surface(&plan, &lowered.graph, NULL));
    check_true(cflow_plan_eval_array(
        &plan, input, sizeof(input) / sizeof(input[0]), &result));
    check_true(cmeta_type_equal(result.type, &cmeta_type_double));
    check((result.count) == ((size_t)1u));
    check_not_null(result.data);
    check((((const double *)result.data)[0]) == (6.0));

    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
    turbo_script_free(ctx);
  }

  it("sanitizes the runtime CFlow terminal seam") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *count_root = NULL;
    exprtk_node_t *vector_root = NULL;
    exprtk_node_t *count_expr = ts_test_single_expr(
        "stream.of([-2, 0, 3]).filter(x => x > 0).map(x => x * 2).count()",
        &count_root);
    exprtk_node_t *vector_expr = ts_test_single_expr(
        "stream.of([1, 2, 3]).filter(x => x > 1).toVector()",
        &vector_root);
    exprtk_value_t count_result = exprtk_val_num(-1.0);
    exprtk_value_t vector_result = exprtk_val_num(-1.0);
    char runtime_error[256] = {0};

    check_not_null(ctx);
    check_not_null(count_expr);
    check_not_null(vector_expr);

    check_equal(ts_cflow_runtime_try_scalar_terminal(
                    ctx, count_expr, &count_result,
                    runtime_error, sizeof(runtime_error)),
                TS_CFLOW_RUNTIME_HANDLED);
    check_equal(count_result.type, EXPRTK_VAL_NUMBER);
    check((((double)count_result.data.number)) == (1.0));
    check_equal(runtime_error[0], '\0');

    check_equal(ts_cflow_runtime_try_scalar_terminal(
                    ctx, vector_expr, &vector_result,
                    runtime_error, sizeof(runtime_error)),
                TS_CFLOW_RUNTIME_HANDLED);
    check_equal(vector_result.type, EXPRTK_VAL_VECTOR);
    check_equal(vector_result.data.vector.size, (size_t)2u);
    check((((double)vector_result.data.vector.data[0])) == (2.0));
    check((((double)vector_result.data.vector.data[1])) == (3.0));
    check_equal(runtime_error[0], '\0');

    {
      exprtk_node_t *bound_root = NULL;
      exprtk_node_t *bound_expr;
      exprtk_value_t bound_result = exprtk_val_num(-1.0);

      check_equal(turbo_script_run(
                      ctx, "bound_values = [-2, 0, 3];"),
                  0);
      bound_expr = ts_test_single_expr(
          "stream.of(bound_values).filter(x => x > 0).count()",
          &bound_root);
      check_not_null(bound_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, bound_expr, &bound_result,
                      runtime_error, sizeof(runtime_error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(bound_result.type, EXPRTK_VAL_NUMBER);
      check((((double)bound_result.data.number)) == (1.0));
      exprtk_value_destroy(&bound_result);
      exprtk_free(bound_root);
    }

    exprtk_value_destroy(&vector_result);
    exprtk_value_destroy(&count_result);
    exprtk_free(vector_root);
    exprtk_free(count_root);
    turbo_script_free(ctx);
  }

  it("executes seeded reduce through a MIR kernel and CFlow plan") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1, 2, 3] |> map(x => x * 2) "
        "|> reduce(10, (acc, x) => acc + x)",
        &root);
    ts_cflow_lowered_pipeline_t lowered;
    cflow_plan plan = {0};
    cflow_result result = {0};
    const cflow_subgraph *sg;
    const void *seed;
    const char *error = NULL;
    const double input[] = {1.0, 2.0, 3.0};

    check_not_null(ctx);
    check_not_null(expr);
    check_true(ts_cflow_lower_pipeline_executable(
        ctx, expr, &lowered, &error));
    check_null(error);
    check_true(lowered.executable);
    check_true(lowered.has_reduce_seed);
    check((lowered.kernel_binding_count) == ((size_t)2u));

    sg = cflow_graph_subgraph(&lowered.graph, lowered.graph.root);
    check_not_null(sg);
    check((sg->node_count) == ((size_t)3u));
    check((sg->nodes[0].op) == (CFLOW_OP_INPUT));
    check((sg->nodes[1].op) == (CFLOW_OP_MAP));
    check((sg->nodes[2].op) == (CFLOW_OP_REDUCE));
    check_true(cmeta_callable_same(
        sg->nodes[2].fn, lowered.kernel_bindings[1].callable));
    seed = cflow_node_reduce_seed(&sg->nodes[2]);
    check_not_null(seed);
    check((((const double *)seed)[0]) == (10.0));

    check_true(cflow_plan_compile_surface(&plan, &lowered.graph, NULL));
    check_true(cflow_plan_eval_array(
        &plan, input, sizeof(input) / sizeof(input[0]), &result));
    check_true(cmeta_type_equal(result.type, &cmeta_type_double));
    check((result.count) == ((size_t)1u));
    check_not_null(result.data);
    check((((const double *)result.data)[0]) == (22.0));

    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
    turbo_script_free(ctx);
  }

  it("returns the owned seed for an empty executable reduce input") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1] |> reduce(-7, (acc, x) => acc + x)", &root);
    ts_cflow_lowered_pipeline_t lowered;
    cflow_plan plan = {0};
    cflow_result result = {0};
    const char *error = NULL;

    check_not_null(ctx);
    check_not_null(expr);
    check_true(ts_cflow_lower_pipeline_executable(
        ctx, expr, &lowered, &error));
    check_null(error);
    check_true(cflow_plan_compile_surface(&plan, &lowered.graph, NULL));
    check_true(cflow_plan_eval_array(&plan, NULL, 0u, &result));
    check((result.count) == ((size_t)1u));
    check_not_null(result.data);
    check((((const double *)result.data)[0]) == (-7.0));

    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
    turbo_script_free(ctx);
  }

  it("rejects dynamic reduce seeds without legacy fallback") {
    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1, 2, 3] |> reduce(seed, (acc, x) => acc + x)", &root);
    ts_cflow_lowered_pipeline_t lowered;
    const char *error = NULL;

    check_not_null(ctx);
    check_not_null(expr);
    check_false(ts_cflow_lower_pipeline_executable(
        ctx, expr, &lowered, &error));
    check_not_null(error);
    check_false(lowered.executable);
    check((lowered.kernel_binding_count) == ((size_t)0u));
    check_null(lowered.kernel_bindings);
    check((lowered.graph.root) == (CMETA_INVALID_ID));

    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
    turbo_script_free(ctx);
  }

  it("retains the TurboScript reduce seed AST for analysis lowering") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1, 2, 3] |> reduce(100, (acc, x) => acc + x)", &root);
    ts_cflow_lowered_pipeline_t lowered;
    const char *error = NULL;

    check_true(ts_cflow_lower_pipeline(expr, &lowered, &error));
    check_true(lowered.has_reduce_seed);
    check_not_null(lowered.reduce_seed_expr);
    check((lowered.reduce_seed_expr->type) == (EXPRTK_NODE_INTEGER));
    check((lowered.reduce_seed_expr->data.integer) == (100));

    ts_cflow_lowered_pipeline_destroy(&lowered);
    exprtk_free(root);
  }

  it("rejects captured callables from the first pure graph slice") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr(
        "[1, 2, 3] |> map(x => x * factor)", &root);
    ts_cflow_lowered_pipeline_t lowered;
    const char *error = NULL;

    check_false(ts_cflow_lower_pipeline(expr, &lowered, &error));
    check_not_null(error);

    exprtk_free(root);
  }

  it("rejects non-pipeline expressions") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *expr = ts_test_single_expr("1 + 2", &root);
    ts_cflow_lowered_pipeline_t lowered;
    const char *error = NULL;

    check_false(ts_cflow_lower_pipeline(expr, &lowered, &error));
    check_not_null(error);

    exprtk_free(root);
  }
}
