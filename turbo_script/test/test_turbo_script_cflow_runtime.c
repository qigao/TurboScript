#include "../src/turbo_script_internal.h"
#include "../src/graph/turbo_script_cflow_runtime.h"
#include "exprtk.h"
#include "tinytest.h"

#include <math.h>
#include <string.h>

static exprtk_node_t *ts_stream_test_single_expr(turbo_script_ctx_t *ctx,
                                                 const char *source,
                                                 exprtk_node_t **root_out) {
  exprtk_node_t *root = turbo_script_parse_with_error(ctx, source);
  if (root_out) *root_out = root;
  if (!root) return NULL;
  if (root->type == EXPRTK_NODE_BLOCK && root->data.block.count == 1u)
    return root->data.block.statements[0];
  return root;
}

spec("TurboScript CFlow stream runtime") {
  describe("terminal admission") {
    it("executes a numeric stream reduce through CFlow") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "[1, 2, 3].stream().filter(x => x > 1).map(x => x * 2)"
          ".reduce(10, (acc, x) => acc + x);",
          &root);
      exprtk_value_t result = exprtk_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, EXPRTK_VAL_NUMBER);
      check(fabs(result.data.number - 20.0) <= 1e-9);
      check_equal(error[0], '\0');

      exprtk_value_destroy(&result);
      exprtk_free(root);
      turbo_script_free(ctx);
    }

    it("executes stream count through the same CFlow plan path") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([-2, 0, 3]).filter(x => x > 0).map(x => x * 2).count();",
          &root);
      exprtk_value_t result = exprtk_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, EXPRTK_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);
      check_equal(error[0], '\0');

      exprtk_value_destroy(&result);
      exprtk_free(root);
      turbo_script_free(ctx);
    }

    it("executes text lines count through a typed CFlow plan") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *lines_root = NULL;
      exprtk_node_t *empty_root = NULL;
      exprtk_node_t *number_root = NULL;
      exprtk_node_t *lines_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"a\\r\\nb\\n\").lines().count();", &lines_root);
      exprtk_node_t *empty_expr = ts_stream_test_single_expr(
          ctx, "stream.text(\"\").lines().count();", &empty_root);
      exprtk_node_t *number_expr = ts_stream_test_single_expr(
          ctx, "stream.text(42).lines().count();", &number_root);
      exprtk_value_t lines_result = exprtk_val_num(-1.0);
      exprtk_value_t empty_result = exprtk_val_num(-1.0);
      exprtk_value_t number_result = exprtk_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(lines_expr);
      check_not_null(empty_expr);
      check_not_null(number_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, lines_expr, &lines_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(lines_result.type, EXPRTK_VAL_NUMBER);
      check(fabs(lines_result.data.number - 3.0) <= 1e-9);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, empty_expr, &empty_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check(fabs(empty_result.data.number - 1.0) <= 1e-9);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, number_expr, &number_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check(fabs(number_result.data.number - 1.0) <= 1e-9);

      exprtk_value_destroy(&number_result);
      exprtk_value_destroy(&empty_result);
      exprtk_value_destroy(&lines_result);
      exprtk_free(number_root);
      exprtk_free(empty_root);
      exprtk_free(lines_root);
      turbo_script_free(ctx);
    }

    it("admits a bound vector source through runtime analysis") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      const double values[] = {-2.0, 0.0, 3.0};
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr;
      exprtk_value_t result = exprtk_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      exprtk_env_set(&ctx->env, "values",
                     exprtk_val_vec((double *)values,
                                    sizeof(values) / sizeof(values[0])));
      expr = ts_stream_test_single_expr(
          ctx,
          "stream.of(values).filter(x => x > 0).map(x => x * 2).count();",
          &root);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, EXPRTK_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);
      check_equal(error[0], '\0');

      exprtk_value_destroy(&result);
      exprtk_free(root);
      turbo_script_free(ctx);
    }

    it("admits a numeric list source through runtime analysis") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr;
      exprtk_value_t result = exprtk_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_equal(turbo_script_run(ctx, "values = list(-2, 0, 3);"), 0);
      expr = ts_stream_test_single_expr(
          ctx,
          "stream.of(values).filter(x => x > 0).map(x => x * 2).count();",
          &root);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, EXPRTK_VAL_NUMBER);
      check(fabs(result.data.number - 1.0) <= 1e-9);
      check_equal(error[0], '\0');

      exprtk_value_destroy(&result);
      exprtk_free(root);
      turbo_script_free(ctx);
    }

    it("returns zero for an empty CFlow count terminal") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr = ts_stream_test_single_expr(
          ctx, "stream.of([]).map(x => x * 2).count();", &root);
      exprtk_value_t result = exprtk_val_num(-1.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(result.type, EXPRTK_VAL_NUMBER);
      check(fabs(result.data.number) <= 1e-9);
      check_equal(error[0], '\0');

      exprtk_value_destroy(&result);
      exprtk_free(root);
      turbo_script_free(ctx);
    }

    it("materializes toVector and toList through CFlow") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *vector_root = NULL;
      exprtk_node_t *list_root = NULL;
      exprtk_node_t *vector_expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([-2, 0, 3]).filter(x => x > 0).map(x => x * 2).toVector();",
          &vector_root);
      exprtk_node_t *list_expr = ts_stream_test_single_expr(
          ctx,
          "[1, 2, 3].stream().filter(x => x > 1).toList();",
          &list_root);
      exprtk_value_t vector_result = exprtk_val_num(0.0);
      exprtk_value_t list_result = exprtk_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(vector_expr);
      check_not_null(list_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, vector_expr, &vector_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(vector_result.type, EXPRTK_VAL_VECTOR);
      check_equal(vector_result.data.vector.size, (size_t)1u);
      check(fabs(vector_result.data.vector.data[0] - 6.0) <= 1e-9);
      check_equal(error[0], '\0');

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, list_expr, &list_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(list_result.type, EXPRTK_VAL_LIST);
      check_equal(list_result.data.list.count, (size_t)2u);
      check(fabs(list_result.data.list.items[0].data.number - 2.0) <= 1e-9);
      check(fabs(list_result.data.list.items[1].data.number - 3.0) <= 1e-9);
      check_equal(error[0], '\0');

      exprtk_value_destroy(&list_result);
      exprtk_value_destroy(&vector_result);
      exprtk_free(list_root);
      exprtk_free(vector_root);
      turbo_script_free(ctx);
    }

    it("preserves legacy collect vector-vs-list shape") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *filter_root = NULL;
      exprtk_node_t *map_root = NULL;
      exprtk_node_t *filter_expr = ts_stream_test_single_expr(
          ctx, "[1, 2, 3].stream().filter(x => x > 1).collect();",
          &filter_root);
      exprtk_node_t *map_expr = ts_stream_test_single_expr(
          ctx, "[1, 2, 3].stream().map(x => x * 2).collect();",
          &map_root);
      exprtk_value_t filter_result = exprtk_val_num(0.0);
      exprtk_value_t map_result = exprtk_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(filter_expr);
      check_not_null(map_expr);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, filter_expr, &filter_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(filter_result.type, EXPRTK_VAL_VECTOR);
      check_equal(filter_result.data.vector.size, (size_t)2u);
      check(fabs(filter_result.data.vector.data[0] - 2.0) <= 1e-9);
      check(fabs(filter_result.data.vector.data[1] - 3.0) <= 1e-9);

      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, map_expr, &map_result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_HANDLED);
      check_equal(map_result.type, EXPRTK_VAL_LIST);
      check_equal(map_result.data.list.count, (size_t)3u);
      check(fabs(map_result.data.list.items[0].data.number - 2.0) <= 1e-9);
      check(fabs(map_result.data.list.items[2].data.number - 6.0) <= 1e-9);

      exprtk_value_destroy(&map_result);
      exprtk_value_destroy(&filter_result);
      exprtk_free(map_root);
      exprtk_free(filter_root);
      turbo_script_free(ctx);
    }

    it("does not reinterpret reduce output as a countable stream") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr = ts_stream_test_single_expr(
          ctx,
          "stream.of([1, 2, 3]).reduce(0, (acc, x) => acc + x).count();",
          &root);
      exprtk_value_t result = exprtk_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(error[0], '\0');

      exprtk_free(root);
      turbo_script_free(ctx);
    }

    it("leaves dynamic-seed reduce on the existing facade during this slice") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_node_t *root = NULL;
      exprtk_node_t *expr = ts_stream_test_single_expr(
          ctx, "[1, 2, 3].stream().reduce(seed, (acc, x) => acc + x);", &root);
      exprtk_value_t result = exprtk_val_num(0.0);
      char error[256] = {0};

      check_not_null(ctx);
      check_not_null(expr);
      check_equal(ts_cflow_runtime_try_scalar_terminal(
                      ctx, expr, &result, error, sizeof(error)),
                  TS_CFLOW_RUNTIME_NOT_APPLICABLE);
      check_equal(error[0], '\0');

      exprtk_free(root);
      turbo_script_free(ctx);
    }
  }

  describe("legacy public run integration") {
    it("keeps interpreter and JIT parity for CFlow terminals") {
      const char *source =
          "reduced = [1, 2, 3].stream().filter(x => x > 1).map(x => x * 2)"
          ".reduce(10, (acc, x) => acc + x);"
          "counted = stream.of([-2, 0, 3]).filter(x => x > 0)"
          ".map(x => x * 2).count();"
          "vectorized = stream.of([1, 2, 3]).map(x => x * 3).toVector();"
          "listed = stream.of([1, 2, 3]).filter(x => x > 1).toList();"
          "collected_filter = stream.of([1, 2, 3]).filter(x => x > 1).collect();"
          "collected_map = stream.of([1, 2, 3]).map(x => x * 2).collect();";
      turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_value_t interp_vector;
      exprtk_value_t jit_vector;
      exprtk_value_t interp_list;
      exprtk_value_t jit_list;
      exprtk_value_t interp_filter_collect;
      exprtk_value_t jit_filter_collect;
      exprtk_value_t interp_map_collect;
      exprtk_value_t jit_map_collect;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(turbo_script_run(interp, source), 0);
      check_equal(turbo_script_run_jit(jit, source), 0);
      check(fabs(ts_get_num(interp, "reduced") - 20.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "reduced") - 20.0) <= 1e-9);
      check(fabs(ts_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "counted") - 1.0) <= 1e-9);

      interp_vector = exprtk_env_get(&interp->env, "vectorized");
      jit_vector = exprtk_env_get(&jit->env, "vectorized");
      check_equal(interp_vector.type, EXPRTK_VAL_VECTOR);
      check_equal(jit_vector.type, EXPRTK_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[2] - 9.0) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 9.0) <= 1e-9);

      interp_list = exprtk_env_get(&interp->env, "listed");
      jit_list = exprtk_env_get(&jit->env, "listed");
      check_equal(interp_list.type, EXPRTK_VAL_LIST);
      check_equal(jit_list.type, EXPRTK_VAL_LIST);
      check_equal(interp_list.data.list.count, (size_t)2u);
      check_equal(jit_list.data.list.count, (size_t)2u);

      interp_filter_collect = exprtk_env_get(&interp->env, "collected_filter");
      jit_filter_collect = exprtk_env_get(&jit->env, "collected_filter");
      check_equal(interp_filter_collect.type, EXPRTK_VAL_VECTOR);
      check_equal(jit_filter_collect.type, EXPRTK_VAL_VECTOR);
      check_equal(interp_filter_collect.data.vector.size, (size_t)2u);
      check_equal(jit_filter_collect.data.vector.size, (size_t)2u);

      interp_map_collect = exprtk_env_get(&interp->env, "collected_map");
      jit_map_collect = exprtk_env_get(&jit->env, "collected_map");
      check_equal(interp_map_collect.type, EXPRTK_VAL_LIST);
      check_equal(jit_map_collect.type, EXPRTK_VAL_LIST);
      check_equal(interp_map_collect.data.list.count, (size_t)3u);
      check_equal(jit_map_collect.data.list.count, (size_t)3u);
      check(fabs(interp_map_collect.data.list.items[2].data.number - 6.0) <= 1e-9);
      check(fabs(jit_map_collect.data.list.items[2].data.number - 6.0) <= 1e-9);

      turbo_script_free(jit);
      turbo_script_free(interp);
    }

    it("keeps text lines count aligned in interpreter and JIT") {
      const char *source =
          "line_count = stream.text(\"a\\r\\nb\\n\").lines().count();"
          "empty_count = stream.text(\"\").lines().count();"
          "coerced_empty_count = stream.text(42).lines().count();";
      turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_BARE);

      check_not_null(interp);
      check_not_null(jit);
      check_equal(turbo_script_run(interp, source), 0);
      check_equal(turbo_script_run_jit(jit, source), 0);
      check(fabs(ts_get_num(interp, "line_count") - 3.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "line_count") - 3.0) <= 1e-9);
      check(fabs(ts_get_num(interp, "empty_count") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "empty_count") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(interp, "coerced_empty_count") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "coerced_empty_count") - 1.0) <= 1e-9);

      turbo_script_free(jit);
      turbo_script_free(interp);
    }

    it("keeps bound vector sources aligned in interpreter and JIT") {
      const char *source =
          "values = [-2, 0, 3];"
          "counted = values.stream().filter(x => x > 0).map(x => x * 2).count();"
          "vectorized = stream.of(values).map(x => x + 1).toVector();";
      turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_value_t interp_vector;
      exprtk_value_t jit_vector;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(turbo_script_run(interp, source), 0);
      check_equal(turbo_script_run_jit(jit, source), 0);
      check(fabs(ts_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "counted") - 1.0) <= 1e-9);

      interp_vector = exprtk_env_get(&interp->env, "vectorized");
      jit_vector = exprtk_env_get(&jit->env, "vectorized");
      check_equal(interp_vector.type, EXPRTK_VAL_VECTOR);
      check_equal(jit_vector.type, EXPRTK_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[0] - (-1.0)) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 4.0) <= 1e-9);

      turbo_script_free(jit);
      turbo_script_free(interp);
    }

    it("keeps numeric list sources aligned in interpreter and JIT") {
      const char *source =
          "values = list(-2, 0, 3);"
          "counted = stream.of(values).filter(x => x > 0).map(x => x * 2).count();"
          "vectorized = stream.of(values).map(x => x + 1).toVector();";
      turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      exprtk_value_t interp_vector;
      exprtk_value_t jit_vector;

      check_not_null(interp);
      check_not_null(jit);
      check_equal(turbo_script_run(interp, source), 0);
      check_equal(turbo_script_run_jit(jit, source), 0);
      check(fabs(ts_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "counted") - 1.0) <= 1e-9);

      interp_vector = exprtk_env_get(&interp->env, "vectorized");
      jit_vector = exprtk_env_get(&jit->env, "vectorized");
      check_equal(interp_vector.type, EXPRTK_VAL_VECTOR);
      check_equal(jit_vector.type, EXPRTK_VAL_VECTOR);
      check_equal(interp_vector.data.vector.size, (size_t)3u);
      check_equal(jit_vector.data.vector.size, (size_t)3u);
      check(fabs(interp_vector.data.vector.data[0] - (-1.0)) <= 1e-9);
      check(fabs(jit_vector.data.vector.data[2] - 4.0) <= 1e-9);

      turbo_script_free(jit);
      turbo_script_free(interp);
    }

    it("keeps heterogeneous list variables on the legacy stream facade") {
      const char *source =
          "values = list(1, \"x\", 3);"
          "answer = stream.of(values).count();";
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(turbo_script_run(ctx, source), 0);
      check(fabs(ts_get_num(ctx, "answer") - 3.0) <= 1e-9);
      turbo_script_free(ctx);
    }

    it("preserves dynamic-seed legacy semantics until that source slice migrates") {
      const char *source =
          "seed = 10;"
          "answer = [1, 2, 3].stream().reduce(seed, (acc, x) => acc + x);";
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);

      check_not_null(ctx);
      check_equal(turbo_script_run(ctx, source), 0);
      check(fabs(ts_get_num(ctx, "answer") - 16.0) <= 1e-9);
      turbo_script_free(ctx);
    }
  }
}