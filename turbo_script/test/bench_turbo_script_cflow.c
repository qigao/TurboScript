#include "../src/turbo_script_internal.h"
#include "../src/graph/turbo_script_cflow_lower.h"
#include "../src/graph/turbo_script_cflow_runtime.h"

#include "exprtk.h"

#include <cflow/plan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct ts_cflow_bench_case {
  size_t elements;
  size_t legacy_iterations;
  size_t runtime_iterations;
  size_t cached_iterations;
} ts_cflow_bench_case;

static double ts_bench_now_us(void) {
  return ((double)clock() * 1000000.0) / (double)CLOCKS_PER_SEC;
}

static exprtk_node_t *ts_bench_single_expr(
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

static const exprtk_node_t *ts_bench_count_pipeline(
    const exprtk_node_t *expr) {
  if (!expr || expr->type != EXPRTK_NODE_MEMBER_CALL ||
      !expr->data.member_call.method ||
      strcmp(expr->data.member_call.method, "count") != 0 ||
      expr->data.member_call.arg_count != 0u)
    return NULL;
  return expr->data.member_call.object;
}

static int ts_bench_number_equals(
    const exprtk_value_t *value,
    double expected) {
  if (!value) return 0;
  if (value->type == EXPRTK_VAL_NUMBER)
    return fabs(value->data.number - expected) <= 1e-9;
  if (value->type == EXPRTK_VAL_INTEGER)
    return fabs((double)value->data.integer - expected) <= 1e-9;
  return 0;
}

static double ts_bench_legacy(
    turbo_script_ctx_t *ctx,
    const exprtk_node_t *expr,
    size_t expected_count,
    size_t iterations) {
  const double start = ts_bench_now_us();
  for (size_t i = 0u; i < iterations; ++i) {
    exprtk_value_t value = exprtk_eval(expr, &ctx->env);
    if (!ts_bench_number_equals(&value, (double)expected_count)) {
      exprtk_value_destroy(&value);
      return -1.0;
    }
    exprtk_value_destroy(&value);
  }
  return (ts_bench_now_us() - start) / (double)iterations;
}

static double ts_bench_runtime(
    turbo_script_ctx_t *ctx,
    const exprtk_node_t *expr,
    size_t expected_count,
    size_t iterations) {
  const double start = ts_bench_now_us();
  for (size_t i = 0u; i < iterations; ++i) {
    exprtk_value_t value = exprtk_val_num(-1.0);
    char error[256] = {0};
    const ts_cflow_runtime_status_t status =
        ts_cflow_runtime_try_scalar_terminal(
            ctx, expr, &value, error, sizeof(error));
    if (status != TS_CFLOW_RUNTIME_HANDLED ||
        !ts_bench_number_equals(&value, (double)expected_count)) {
      fprintf(stderr,
              "runtime benchmark failed: status=%d error=%s\n",
              (int)status, error);
      exprtk_value_destroy(&value);
      return -1.0;
    }
    exprtk_value_destroy(&value);
  }
  return (ts_bench_now_us() - start) / (double)iterations;
}

static double ts_bench_cached_plan(
    const cflow_plan *plan,
    const double *input,
    size_t count,
    size_t expected_count,
    size_t iterations) {
  const double start = ts_bench_now_us();
  for (size_t i = 0u; i < iterations; ++i) {
    cflow_result result = {0};
    if (!cflow_plan_eval_array(plan, input, count, &result) ||
        result.count != expected_count ||
        !cmeta_type_equal(result.type, &cmeta_type_double)) {
      cflow_result_destroy(&result);
      return -1.0;
    }
    cflow_result_destroy(&result);
  }
  return (ts_bench_now_us() - start) / (double)iterations;
}

static int ts_bench_count_case(ts_cflow_bench_case cfg) {
  static const char *script =
      "stream.of(values)"
      ".filter(x => x > 0)"
      ".map(x => x * 2)"
      ".count();";
  turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
  double *input = NULL;
  exprtk_node_t *root = NULL;
  exprtk_node_t *expr;
  const exprtk_node_t *pipeline;
  ts_cflow_lowered_pipeline_t lowered = {0};
  cflow_plan plan = {0};
  cflow_plan_compile_stats compile_stats = {0};
  const char *error = NULL;
  const size_t expected_count = cfg.elements / 2u;
  double legacy_us;
  double runtime_us;
  double cached_us;

  if (!ctx || cfg.elements == 0u || cfg.elements % 4u != 0u)
    goto fail;

  input = (double *)malloc(cfg.elements * sizeof(*input));
  if (!input) goto fail;
  for (size_t i = 0u; i < cfg.elements; ++i)
    input[i] = (double)((int)(i % 4u) - 1);

  exprtk_env_set(
      &ctx->env, "values",
      exprtk_val_vec(input, cfg.elements));
  expr = ts_bench_single_expr(ctx, script, &root);
  if (!expr) goto fail;
  pipeline = ts_bench_count_pipeline(expr);
  if (!pipeline) goto fail;

  if (!ts_cflow_lower_pipeline_executable(
          ctx, pipeline, &lowered, &error)) {
    fprintf(stderr, "CFlow benchmark lowering failed: %s\n",
            error ? error : "unknown");
    goto fail;
  }
  if (!cflow_plan_compile_surface(
          &plan, &lowered.graph, &compile_stats)) {
    fprintf(stderr, "CFlow benchmark Plan compile failed: %s\n",
            plan.error ? plan.error : "unknown");
    goto fail;
  }

  /* Warm all three paths before recording telemetry. */
  if (ts_bench_legacy(ctx, expr, expected_count, 2u) < 0.0 ||
      ts_bench_runtime(ctx, expr, expected_count, 1u) < 0.0 ||
      ts_bench_cached_plan(
          &plan, input, cfg.elements, expected_count, 4u) < 0.0)
    goto fail;

  legacy_us = ts_bench_legacy(
      ctx, expr, expected_count, cfg.legacy_iterations);
  runtime_us = ts_bench_runtime(
      ctx, expr, expected_count, cfg.runtime_iterations);
  cached_us = ts_bench_cached_plan(
      &plan, input, cfg.elements, expected_count,
      cfg.cached_iterations);
  if (legacy_us <= 0.0 || runtime_us <= 0.0 || cached_us <= 0.0)
    goto fail;

  printf(
      "CFLOW_BENCH scenario=filter_map_count elements=%zu "
      "path=legacy_eager us_per_eval=%.3f iterations=%zu\n",
      cfg.elements, legacy_us, cfg.legacy_iterations);
  printf(
      "CFLOW_BENCH scenario=filter_map_count elements=%zu "
      "path=current_runtime us_per_eval=%.3f iterations=%zu\n",
      cfg.elements, runtime_us, cfg.runtime_iterations);
  printf(
      "CFLOW_BENCH scenario=filter_map_count elements=%zu "
      "path=cached_plan us_per_eval=%.3f iterations=%zu\n",
      cfg.elements, cached_us, cfg.cached_iterations);
  printf(
      "CFLOW_PLAN scenario=filter_map_count elements=%zu "
      "graph_nodes=%zu instructions=%zu map_callbacks=%zu "
      "inference_queries=%zu runtime_over_cached=%.3f "
      "legacy_over_cached=%.3f\n",
      cfg.elements,
      compile_stats.graph_nodes,
      compile_stats.instructions,
      compile_stats.map_callbacks,
      compile_stats.inference_queries,
      runtime_us / cached_us,
      legacy_us / cached_us);

  cflow_plan_destroy(&plan);
  ts_cflow_lowered_pipeline_destroy(&lowered);
  exprtk_free(root);
  free(input);
  turbo_script_free(ctx);
  return 0;

fail:
  cflow_plan_destroy(&plan);
  ts_cflow_lowered_pipeline_destroy(&lowered);
  if (root) exprtk_free(root);
  free(input);
  turbo_script_free(ctx);
  return 1;
}

static int ts_bench_reduce_case(void) {
  static const char *script =
      "stream.of(values)"
      ".map(x => x * 2)"
      ".reduce(0, (acc, x) => acc + x);";
  const size_t elements = 4096u;
  const size_t legacy_iterations = 20u;
  const size_t runtime_iterations = 6u;
  const size_t cached_iterations = 120u;
  turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
  double *input = NULL;
  exprtk_node_t *root = NULL;
  exprtk_node_t *expr;
  ts_cflow_lowered_pipeline_t lowered = {0};
  cflow_plan plan = {0};
  cflow_plan_compile_stats compile_stats = {0};
  const char *error = NULL;
  const double expected =
      (double)elements * (double)(elements - 1u);
  double legacy_us;
  double runtime_us;
  double cached_us;
  double start;

  if (!ctx) goto fail;
  input = (double *)malloc(elements * sizeof(*input));
  if (!input) goto fail;
  for (size_t i = 0u; i < elements; ++i)
    input[i] = (double)i;
  exprtk_env_set(&ctx->env, "values", exprtk_val_vec(input, elements));

  expr = ts_bench_single_expr(ctx, script, &root);
  if (!expr) goto fail;
  if (!ts_cflow_lower_pipeline_executable(
          ctx, expr, &lowered, &error)) {
    fprintf(stderr, "reduce lowering failed: %s\n",
            error ? error : "unknown");
    goto fail;
  }
  if (!cflow_plan_compile_surface(
          &plan, &lowered.graph, &compile_stats))
    goto fail;

  /* Legacy eager reduce. */
  start = ts_bench_now_us();
  for (size_t i = 0u; i < legacy_iterations; ++i) {
    exprtk_value_t value = exprtk_eval(expr, &ctx->env);
    if (!ts_bench_number_equals(&value, expected)) {
      exprtk_value_destroy(&value);
      goto fail;
    }
    exprtk_value_destroy(&value);
  }
  legacy_us =
      (ts_bench_now_us() - start) / (double)legacy_iterations;

  /* Current runtime cutover: lower/bind/compile each call. */
  start = ts_bench_now_us();
  for (size_t i = 0u; i < runtime_iterations; ++i) {
    exprtk_value_t value = exprtk_val_num(-1.0);
    char runtime_error[256] = {0};
    if (ts_cflow_runtime_try_scalar_terminal(
            ctx, expr, &value, runtime_error,
            sizeof(runtime_error)) != TS_CFLOW_RUNTIME_HANDLED ||
        !ts_bench_number_equals(&value, expected)) {
      exprtk_value_destroy(&value);
      goto fail;
    }
    exprtk_value_destroy(&value);
  }
  runtime_us =
      (ts_bench_now_us() - start) / (double)runtime_iterations;

  /* Reuse already-bound MIR kernels and compiled Plan. */
  start = ts_bench_now_us();
  for (size_t i = 0u; i < cached_iterations; ++i) {
    cflow_result result = {0};
    if (!cflow_plan_eval_array(
            &plan, input, elements, &result) ||
        result.count != 1u ||
        !cmeta_type_equal(result.type, &cmeta_type_double) ||
        fabs(((const double *)result.data)[0] - expected) > 1e-9) {
      cflow_result_destroy(&result);
      goto fail;
    }
    cflow_result_destroy(&result);
  }
  cached_us =
      (ts_bench_now_us() - start) / (double)cached_iterations;

  printf(
      "CFLOW_BENCH scenario=map_reduce elements=%zu "
      "path=legacy_eager us_per_eval=%.3f iterations=%zu\n",
      elements, legacy_us, legacy_iterations);
  printf(
      "CFLOW_BENCH scenario=map_reduce elements=%zu "
      "path=current_runtime us_per_eval=%.3f iterations=%zu\n",
      elements, runtime_us, runtime_iterations);
  printf(
      "CFLOW_BENCH scenario=map_reduce elements=%zu "
      "path=cached_plan us_per_eval=%.3f iterations=%zu\n",
      elements, cached_us, cached_iterations);
  printf(
      "CFLOW_PLAN scenario=map_reduce elements=%zu "
      "graph_nodes=%zu instructions=%zu map_callbacks=%zu "
      "runtime_over_cached=%.3f legacy_over_cached=%.3f\n",
      elements,
      compile_stats.graph_nodes,
      compile_stats.instructions,
      compile_stats.map_callbacks,
      runtime_us / cached_us,
      legacy_us / cached_us);

  cflow_plan_destroy(&plan);
  ts_cflow_lowered_pipeline_destroy(&lowered);
  exprtk_free(root);
  free(input);
  turbo_script_free(ctx);
  return 0;

fail:
  cflow_plan_destroy(&plan);
  ts_cflow_lowered_pipeline_destroy(&lowered);
  if (root) exprtk_free(root);
  free(input);
  turbo_script_free(ctx);
  return 1;
}

int main(void) {
  const ts_cflow_bench_case cases[] = {
      {16u, 300u, 12u, 2000u},
      {4096u, 30u, 6u, 160u},
      {65536u, 6u, 2u, 24u},
  };

  printf("TurboScript CFlow qualification telemetry\n");
  printf("Timing uses process CPU clock; values are machine-specific and non-gating.\n");

  for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (ts_bench_count_case(cases[i]) != 0) return 1;
  }
  if (ts_bench_reduce_case() != 0) return 1;
  return 0;
}
