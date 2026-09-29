#include "tinytest.h"
#include "exprtk.h"
#include "turbo_script_cflow_lower.h"
#include "turbo_script_cflow_runtime.h"
#include "turbo_script_cflow_text.h"
#include "turbo_script_cflow_text_kernel.h"

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

  it("builds an inspectable typed text-line CFlow plan") {
    ts_cflow_text_lines_source_t source = {0};
    cflow_graph graph = {0};
    cflow_plan plan = {0};
    const cflow_subgraph *sg;
    const char *error = NULL;
    size_t count = 0u;

    graph.root = CMETA_INVALID_ID;
    check_true(ts_cflow_text_lines_source_init(
        &source, "a\r\nb\n", strlen("a\r\nb\n")));
    check_equal(source.count, (size_t)3u);
    check_equal(source.items[0].len, (size_t)1u);
    check_equal(source.items[1].len, (size_t)1u);
    check_equal(source.items[2].len, (size_t)0u);
    check(memcmp(source.items[0].data, "a", 1u) == 0);
    check(memcmp(source.items[1].data, "b", 1u) == 0);

    check_true(ts_cflow_text_lines_plan_compile(&graph, &plan, &error));
    check_null(error);
    check_true(cmeta_type_equal(cflow_graph_input_type(&graph),
                                ts_cflow_line_slice_type()));
    check_true(cmeta_type_equal(cflow_graph_output_type(&graph),
                                ts_cflow_line_slice_type()));
    sg = cflow_graph_subgraph(&graph, graph.root);
    check_not_null(sg);
    check_equal(sg->node_count, (size_t)1u);
    check_equal(sg->nodes[0].op, CFLOW_OP_INPUT);

    {
      cflow_result result = {0};
      const ts_cflow_line_slice_t *lines;

      check_true(ts_cflow_text_lines_plan_eval(
          &plan, &source, &result, &error));
      check_null(error);
      check_equal(result.count, (size_t)3u);
      check_true(cmeta_type_equal(result.type, ts_cflow_line_slice_type()));
      lines = (const ts_cflow_line_slice_t *)result.data;
      check_not_null(lines);
      check_equal(lines[0].len, (size_t)1u);
      check_equal(lines[1].len, (size_t)1u);
      check_equal(lines[2].len, (size_t)0u);
      check(memcmp(lines[0].data, "a", 1u) == 0);
      check(memcmp(lines[1].data, "b", 1u) == 0);
      cflow_result_destroy(&result);
    }

    check_true(ts_cflow_text_lines_plan_count(
        &plan, &source, &count, &error));
    check_null(error);
    check_equal(count, (size_t)3u);

    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
    ts_cflow_text_lines_source_destroy(&source);
  }

  it("preserves legacy text split slices through the typed CFlow plan") {
    ts_cflow_text_lines_source_t source = {0};
    ts_cflow_text_lines_source_t chars = {0};
    ts_cflow_text_lines_source_t empty_chars = {0};
    cflow_graph graph = {0};
    cflow_plan plan = {0};
    cflow_result result = {0};
    const ts_cflow_line_slice_t *tokens;
    const char *error = NULL;

    graph.root = CMETA_INVALID_ID;
    check_true(ts_cflow_text_split_source_init(
        &source, ",a,,b,", strlen(",a,,b,"), ",", 1u));
    check_equal(source.count, (size_t)5u);
    check_equal(source.items[0].len, (size_t)0u);
    check_equal(source.items[1].len, (size_t)1u);
    check(memcmp(source.items[1].data, "a", 1u) == 0);
    check_equal(source.items[2].len, (size_t)0u);
    check_equal(source.items[3].len, (size_t)1u);
    check(memcmp(source.items[3].data, "b", 1u) == 0);
    check_equal(source.items[4].len, (size_t)0u);

    check_true(ts_cflow_text_lines_plan_compile(&graph, &plan, &error));
    check_true(ts_cflow_text_lines_plan_eval(
        &plan, &source, &result, &error));
    check_null(error);
    check_equal(result.count, (size_t)5u);
    tokens = (const ts_cflow_line_slice_t *)result.data;
    check_not_null(tokens);
    check_equal(tokens[0].len, (size_t)0u);
    check_equal(tokens[4].len, (size_t)0u);
    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
    ts_cflow_text_lines_source_destroy(&source);

    check_true(ts_cflow_text_split_source_init(
        &chars, "ABC", 3u, "", 0u));
    check_equal(chars.count, (size_t)3u);
    check_equal(chars.items[0].len, (size_t)1u);
    check(memcmp(chars.items[0].data, "A", 1u) == 0);
    check(memcmp(chars.items[2].data, "C", 1u) == 0);
    ts_cflow_text_lines_source_destroy(&chars);

    check_true(ts_cflow_text_split_source_init(
        &empty_chars, "", 0u, "", 0u));
    check_equal(empty_chars.count, (size_t)0u);
    check_null(empty_chars.items);
    ts_cflow_text_lines_source_destroy(&empty_chars);

    {
      ts_cflow_text_lines_source_t multi = {0};
      check_true(ts_cflow_text_split_source_init(
          &multi, "a<>b<>", strlen("a<>b<>"), "<>", 2u));
      check_equal(multi.count, (size_t)3u);
      check_equal(multi.items[0].len, (size_t)1u);
      check_equal(multi.items[1].len, (size_t)1u);
      check_equal(multi.items[2].len, (size_t)0u);
      check(memcmp(multi.items[0].data, "a", 1u) == 0);
      check(memcmp(multi.items[1].data, "b", 1u) == 0);
      ts_cflow_text_lines_source_destroy(&multi);
    }
  }

  it("executes typed LineSlice length MAP through MIR and CFlow") {
    exprtk_node_t *lambda_root = NULL;
    exprtk_node_t *lambda =
        ts_test_single_expr("line => line.length()", &lambda_root);
    ts_cflow_text_kernel_binding_t binding = {0};
    ts_cflow_text_lines_source_t source = {0};
    cflow_graph graph = {0};
    cflow_plan plan = {0};
    cflow_result result = {0};
    const cflow_subgraph *sg;
    const char *error = NULL;
    const double expected[] = {1.0, 2.0, 0.0};

    graph.root = CMETA_INVALID_ID;
    check_not_null(lambda);
    check_true(ts_cflow_text_length_map_bind(
        lambda, &binding, &error));
    check_null(error);
    check_equal(binding.callable.meta.sig, CMETA_SIG_INVALID);
    check_equal(binding.callable.dispatch,
                CMETA_CALLABLE_DISPATCH_ADAPTER);
    check_equal(binding.callable.meta.effects,
                (cmeta_effects)CMETA_EFFECT_PURE);
    check_true((binding.callable.meta.properties &
                CMETA_PROP_DETERMINISTIC) != 0u);
    check_true((binding.callable.meta.properties &
                CMETA_PROP_TOTAL) != 0u);

    check_true(ts_cflow_text_lines_source_init(
        &source, "a\nbb\n", strlen("a\nbb\n")));
    cflow_graph_init(&graph, ts_cflow_line_slice_type());
    check_null(graph.error);
    check_true(ts_cflow_text_length_map_graph_add(
        &graph, &binding, &error));
    check_null(error);

    sg = cflow_graph_subgraph(&graph, graph.root);
    check_not_null(sg);
    check_equal(sg->node_count, (size_t)2u);
    check_equal(sg->nodes[0].op, CFLOW_OP_INPUT);
    check_equal(sg->nodes[1].op, CFLOW_OP_MAP);
    check_equal(sg->nodes[1].param_kind,
                CFLOW_NODE_PARAM_TYPED_ADAPTER);
    check_true(cmeta_type_equal(
        sg->nodes[1].input_type, ts_cflow_line_slice_type()));
    check_true(cmeta_type_equal(
        sg->nodes[1].output_type, &cmeta_type_double));

    check_true(cflow_plan_compile_surface(
        &plan, &graph, NULL));
    check_true(cflow_plan_eval_array(
        &plan, source.items, source.count, &result));
    check_true(cmeta_type_equal(result.type, &cmeta_type_double));
    check_equal(result.count, (size_t)3u);
    check_not_null(result.data);
    check(fabs(((const double *)result.data)[0] - expected[0]) <= 1e-9);
    check(fabs(((const double *)result.data)[1] - expected[1]) <= 1e-9);
    check(fabs(((const double *)result.data)[2] - expected[2]) <= 1e-9);

    cflow_result_destroy(&result);
    cflow_plan_destroy(&plan);
    cflow_graph_destroy(&graph);
    ts_cflow_text_lines_source_destroy(&source);
    ts_cflow_text_kernel_binding_destroy(&binding);
    exprtk_free(lambda_root);
  }

  it("rejects text MAP shapes outside the first typed ABI slice") {
    exprtk_node_t *trim_root = NULL;
    exprtk_node_t *capture_root = NULL;
    exprtk_node_t *trim_lambda =
        ts_test_single_expr("line => line.trim()", &trim_root);
    exprtk_node_t *capture_lambda =
        ts_test_single_expr("line => line.length() + extra", &capture_root);
    ts_cflow_text_kernel_binding_t binding = {0};
    const char *error = NULL;

    check_not_null(trim_lambda);
    check_not_null(capture_lambda);
    check_false(ts_cflow_text_length_map_bind(
        trim_lambda, &binding, &error));
    check_not_null(error);
    check_null(binding.owner);

    error = NULL;
    check_false(ts_cflow_text_length_map_bind(
        capture_lambda, &binding, &error));
    check_not_null(error);
    check_null(binding.owner);

    exprtk_free(capture_root);
    exprtk_free(trim_root);
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

    {
      exprtk_node_t *list_root = NULL;
      exprtk_node_t *list_expr;
      exprtk_value_t list_result = exprtk_val_num(-1.0);

      check_equal(turbo_script_run(
                      ctx, "bound_list = list(-2, 0, 3);"),
                  0);
      list_expr = ts_test_single_expr(
          "stream.of(bound_list).filter(x => x > 0).count()",
          &list_root);
      check_not_null(list_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, list_expr, &list_result,
                      runtime_error, sizeof(runtime_error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(list_result.type, EXPRTK_VAL_NUMBER);
      check((((double)list_result.data.number)) == (1.0));
      exprtk_value_destroy(&list_result);
      exprtk_free(list_root);
    }

    {
      exprtk_node_t *text_root = NULL;
      exprtk_node_t *text_expr = ts_test_single_expr(
          "stream.text(\"a\\r\\nb\\n\").lines().toList()",
          &text_root);
      exprtk_value_t text_result = exprtk_val_num(-1.0);

      check_not_null(text_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, text_expr, &text_result,
                      runtime_error, sizeof(runtime_error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(text_result.type, EXPRTK_VAL_LIST);
      check_equal(text_result.data.list.count, (size_t)3u);
      check_equal(text_result.data.list.items[0].type, EXPRTK_VAL_STRING);
      check_equal(text_result.data.list.items[0].data.string.len, (size_t)1u);
      check(memcmp(text_result.data.list.items[0].data.string.data, "a", 1u) == 0);
      check_equal(text_result.data.list.items[2].data.string.len, (size_t)0u);

      exprtk_value_destroy(&text_result);
      exprtk_free(text_root);
    }

    {
      exprtk_node_t *split_root = NULL;
      exprtk_node_t *split_expr = ts_test_single_expr(
          "stream.text(\"a,,b,\").split(\",\").toList()",
          &split_root);
      exprtk_value_t split_result = exprtk_val_num(-1.0);

      check_not_null(split_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, split_expr, &split_result,
                      runtime_error, sizeof(runtime_error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(split_result.type, EXPRTK_VAL_LIST);
      check_equal(split_result.data.list.count, (size_t)4u);
      check_equal(split_result.data.list.items[1].data.string.len, (size_t)0u);
      check(memcmp(split_result.data.list.items[2].data.string.data, "b", 1u) == 0);
      check_equal(split_result.data.list.items[3].data.string.len, (size_t)0u);

      exprtk_value_destroy(&split_result);
      exprtk_free(split_root);
    }

    {
      exprtk_node_t *length_root = NULL;
      exprtk_node_t *length_expr = ts_test_single_expr(
          "stream.text(\"a\\nbb\\n\").lines()"
          ".map(line => line.length()).toVector()",
          &length_root);
      exprtk_value_t length_result = exprtk_val_num(-1.0);

      check_not_null(length_expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, length_expr, &length_result,
                      runtime_error, sizeof(runtime_error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(length_result.type, EXPRTK_VAL_VECTOR);
      check_equal(length_result.data.vector.size, (size_t)3u);
      check(fabs(length_result.data.vector.data[0] - 1.0) <= 1e-9);
      check(fabs(length_result.data.vector.data[1] - 2.0) <= 1e-9);
      check(fabs(length_result.data.vector.data[2] - 0.0) <= 1e-9);

      exprtk_value_destroy(&length_result);
      exprtk_free(length_root);
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