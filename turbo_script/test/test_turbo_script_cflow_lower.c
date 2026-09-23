#include "tinytest.h"
#include "exprtk.h"
#include "turbo_script_cflow_lower.h"

#include <cflow/graph.h>
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

describe(cflow_pipeline_lowering) {
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

  it("retains the TurboScript reduce seed outside the unseeded CFlow fold") {
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
