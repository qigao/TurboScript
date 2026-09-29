#include "../src/turbo_script_internal.h"
#include "../src/graph/turbo_script_cflow_runtime.h"
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
  describe("scalar terminal admission") {
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
    it("keeps interpreter and JIT parity for CFlow reduce and count terminals") {
      const char *source =
          "reduced = [1, 2, 3].stream().filter(x => x > 1).map(x => x * 2)"
          ".reduce(10, (acc, x) => acc + x);"
          "counted = stream.of([-2, 0, 3]).filter(x => x > 0)"
          ".map(x => x * 2).count();";
      turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_BARE);

      check_not_null(interp);
      check_not_null(jit);
      check_equal(turbo_script_run(interp, source), 0);
      check_equal(turbo_script_run_jit(jit, source), 0);
      check(fabs(ts_get_num(interp, "reduced") - 20.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "reduced") - 20.0) <= 1e-9);
      check(fabs(ts_get_num(interp, "counted") - 1.0) <= 1e-9);
      check(fabs(ts_get_num(jit, "counted") - 1.0) <= 1e-9);

      turbo_script_free(jit);
      turbo_script_free(interp);
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