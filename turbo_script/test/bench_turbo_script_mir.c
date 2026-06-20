/**
 * @file bench_turbo_script_mir.c
 * @brief Performance benchmarks for TurboScript JIT vs Interpreter
 */

#include "turbo_script.h"
#include "../src/turbo_script_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double get_time_ms(void) {
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <sys/time.h>
static double get_time_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
#endif

typedef struct {
  const char *name;
  const char *script;
  const char *result_var;
  int iterations;
  int warmup;
  int supported_jit;
} benchmark_t;

static void configure_bench_ctx(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  ctx->env.max_loop_iterations = 10000000;
  ctx->env.max_nodes = 100000000;
}

static void reset_runtime_limits(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  ctx->env.aborted = 0;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;
}

static int same_result(double a, double b) {
  double diff = fabs(a - b);
  double scale = fmax(fmax(fabs(a), fabs(b)), 1.0);
  return diff <= scale * 1e-9;
}

static const char *error_or_empty(turbo_script_ctx_t *ctx) {
  const char *err = turbo_script_get_error(ctx);
  return err ? err : "";
}

static int text_matches_filter(const char *text, const char *filter) {
  return !filter || !filter[0] || (text && strstr(text, filter));
}

static benchmark_t scaled_benchmark(benchmark_t bench, int quick) {
  if (!quick) return bench;
  bench.iterations = bench.iterations > 10 ? bench.iterations / 10 : bench.iterations;
  bench.warmup = bench.warmup > 1 ? bench.warmup / 2 : bench.warmup;
  if (bench.iterations < 1) bench.iterations = 1;
  if (bench.warmup < 1) bench.warmup = 1;
  return bench;
}

static double benchmark_run_jit(turbo_script_ctx_t *ctx, const char *script, int iterations) {
  double start = get_time_ms();
  for (int i = 0; i < iterations; i++) {
    reset_runtime_limits(ctx);
    if (turbo_script_run_jit(ctx, script) != 0) {
      fprintf(stderr, "run_jit failed: %s\n", error_or_empty(ctx));
      return -1.0;
    }
  }
  double end = get_time_ms();
  return end - start;
}

static double benchmark_interp(turbo_script_ctx_t *ctx, const char *script, int iterations) {
  double start = get_time_ms();
  for (int i = 0; i < iterations; i++) {
    reset_runtime_limits(ctx);
    if (turbo_script_run(ctx, script) != 0) {
      fprintf(stderr, "Interpreter execution failed: %s\n", error_or_empty(ctx));
      return -1.0;
    }
  }
  double end = get_time_ms();
  return end - start;
}

static double benchmark_exec_jit(turbo_script_ctx_t *ctx, int iterations) {
  double start = get_time_ms();
  for (int i = 0; i < iterations; i++) {
    reset_runtime_limits(ctx);
    if (turbo_script_exec_jit(ctx) != 0) {
      fprintf(stderr, "exec_jit failed: %s\n", error_or_empty(ctx));
      return -1.0;
    }
  }
  double end = get_time_ms();
  return end - start;
}

static double benchmark_compile_mir(turbo_script_ctx_t *ctx, const char *script, int iterations) {
  double start = get_time_ms();
  for (int i = 0; i < iterations; i++) {
    reset_runtime_limits(ctx);
    if (turbo_script_compile_mir(ctx, script) != 0) {
      fprintf(stderr, "compile_mir failed: %s\n", error_or_empty(ctx));
      return -1.0;
    }
  }
  double end = get_time_ms();
  return end - start;
}

static void run_benchmark(const benchmark_t *bench) {
  const char *result_var = bench->result_var ? bench->result_var : "sum";
  turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  turbo_script_ctx_t *ctx_exec_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

  if (!ctx_jit || !ctx_exec_jit || !ctx_interp) {
    fprintf(stderr, "Failed to initialize contexts\n");
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }
  configure_bench_ctx(ctx_jit);
  configure_bench_ctx(ctx_exec_jit);
  configure_bench_ctx(ctx_interp);

  printf("\n=== %s ===\n", bench->name);
  printf("Script: %s\n", bench->script);
  printf("Iterations: %d (warmup: %d)\n", bench->iterations, bench->warmup);

  reset_runtime_limits(ctx_interp);
  if (turbo_script_run(ctx_interp, bench->script) != 0) {
    printf("  FAIL: interpreter rejected script: %s\n", error_or_empty(ctx_interp));
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }
  double result_interp = ts_get_num(ctx_interp, result_var);

  if (!bench->supported_jit) {
    printf("  SKIP: not a JIT-supported benchmark case\n");
    printf("  Interpreter result: %.6f\n", result_interp);
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  reset_runtime_limits(ctx_jit);
  if (turbo_script_run_jit(ctx_jit, bench->script) != 0) {
    printf("  FAIL: run_jit rejected script: %s\n", error_or_empty(ctx_jit));
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }
  double result_run_jit = ts_get_num(ctx_jit, result_var);
  if (!same_result(result_interp, result_run_jit)) {
    printf("  FAIL: run_jit result differs: interp=%.12g, jit=%.12g\n", result_interp, result_run_jit);
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  reset_runtime_limits(ctx_exec_jit);
  if (turbo_script_compile_mir(ctx_exec_jit, bench->script) != 0 ||
      turbo_script_exec_jit(ctx_exec_jit) != 0) {
    printf("  FAIL: compile_mir/exec_jit rejected script: %s\n", error_or_empty(ctx_exec_jit));
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }
  double result_exec_jit = ts_get_num(ctx_exec_jit, result_var);
  if (!same_result(result_interp, result_exec_jit)) {
    printf("  FAIL: exec_jit result differs: interp=%.12g, jit=%.12g\n", result_interp, result_exec_jit);
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  for (int i = 0; i < bench->warmup; i++) {
    reset_runtime_limits(ctx_jit);
    reset_runtime_limits(ctx_exec_jit);
    reset_runtime_limits(ctx_interp);
    if (turbo_script_run_jit(ctx_jit, bench->script) != 0 ||
        turbo_script_exec_jit(ctx_exec_jit) != 0 ||
        turbo_script_run(ctx_interp, bench->script) != 0) {
      printf("  FAIL: warmup failed\n");
      turbo_script_free(ctx_jit);
      turbo_script_free(ctx_exec_jit);
      turbo_script_free(ctx_interp);
      return;
    }
  }

  double time_interp = benchmark_interp(ctx_interp, bench->script, bench->iterations);
  if (time_interp < 0) {
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  double time_run_jit = benchmark_run_jit(ctx_jit, bench->script, bench->iterations);
  if (time_run_jit < 0) {
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  double time_exec_jit = benchmark_exec_jit(ctx_exec_jit, bench->iterations);
  if (time_exec_jit < 0) {
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  int compile_iters = bench->iterations < 10 ? bench->iterations : 10;
  turbo_script_ctx_t *ctx_compile = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  configure_bench_ctx(ctx_compile);
  double time_compile = benchmark_compile_mir(ctx_compile, bench->script, compile_iters);
  turbo_script_free(ctx_compile);
  if (time_compile < 0) {
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return;
  }

  double avg_interp = time_interp / bench->iterations;
  double avg_run_jit = time_run_jit / bench->iterations;
  double avg_exec_jit = time_exec_jit / bench->iterations;
  double avg_compile = time_compile / compile_iters;

  printf("\nResults:\n");
  printf("  Interpreter:       %.3f ms total, %.6f ms/iter\n", time_interp, avg_interp);
  printf("  JIT run_jit warm:  %.3f ms total, %.6f ms/iter, speedup %.2fx\n",
         time_run_jit, avg_run_jit, time_interp / time_run_jit);
  printf("  JIT exec only:     %.3f ms total, %.6f ms/iter, speedup %.2fx\n",
         time_exec_jit, avg_exec_jit, time_interp / time_exec_jit);
  printf("  JIT compile once:  %.3f ms/compile\n", avg_compile);
  printf("  Result:            %.6f (verified)\n", result_interp);
  fflush(stdout);

  turbo_script_free(ctx_jit);
  turbo_script_free(ctx_exec_jit);
  turbo_script_free(ctx_interp);
}

int main(int argc, char **argv) {
  const char *filter = NULL;
  int list_only = 0;
  int quick = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--list") == 0) {
      list_only = 1;
    } else if (strcmp(argv[i], "--quick") == 0) {
      quick = 1;
    } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[++i];
    } else {
      fprintf(stderr, "Usage: %s [--list] [--quick] [--filter text]\n", argv[0]);
      return 2;
    }
  }

  printf("TurboScript JIT Performance Benchmarks\n");
  printf("======================================\n");

  benchmark_t benchmarks[] = {
      /* Pure computation */
      {.name = "Pure Loop (100k iterations)",
       .script = "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      {.name = "Nested Loop (10k iterations)",
       .script = "sum = 0; for (i = 0; i < 100; i += 1) { for (j = 0; j < 100; j += 1) { sum "
                 "+= 1; } }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      /* Math functions */
      {.name = "Math Functions (sin, 10k iterations)",
       .script = "sum = 0; for (i = 0; i < 10000; i += 1) { sum += sin(i * 0.001); }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      {.name = "Math Functions (sqrt, 10k iterations)",
       .script = "sum = 0; for (i = 0; i < 10000; i += 1) { sum += sqrt(i); }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      {.name = "Math Functions (mixed, 10k iterations)",
       .script = "sum = 0; for (i = 0; i < 10000; i += 1) { sum += sin(i) + cos(i) + sqrt(i); "
                 "}",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      /* Arithmetic */
      {.name = "Arithmetic Operations (100k iterations)",
       .script = "sum = 0; for (i = 0; i < 100000; i += 1) { sum += (i * 2 + 3) / 4 - 1; }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      /* Conditionals */
      {.name = "Conditional Branches (100k iterations)",
       .script = "sum = 0; for (i = 0; i < 100000; i += 1) { if (i % 2 == 0) { sum += i; } "
                 "else { sum -= i; } }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      /* Break/Continue */
      {.name = "Break/Continue (100k iterations)",
       .script = "sum = 0; i = 0; while (i < 100000) { i += 1; if (i % 10 == 0) { continue; } "
                 "if (i > 50000) { break; } sum += i; }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      /* Function calls */
      {.name = "Function Calls (10k iterations)",
       .script = "func double(x) { return x * 2; } sum = 0; for (i = 0; i < 10000; i += 1) { "
                 "sum += double(i); }",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      /* OOP helper lowering */
      {.name = "OOP Direct Field Read/Write (10k iterations)",
       .script = "class Counter {"
                 "  constructor(value) { this.value = value; }"
                 "};"
                 "c = new Counter(0);"
                 "sum = 0;"
                 "for (i = 0; i < 10000; i += 1) { c.value = c.value + 1; sum += c.value; }",
       .iterations = 50,
       .warmup = 5,
       .supported_jit = 1},

      {.name = "OOP Method Field Read/Write (10k iterations)",
       .script = "class Counter {"
                 "  constructor(value) { this.value = value; }"
                 "  bump(delta) { this.value = this.value + delta; return this.value; }"
                 "};"
                 "c = new Counter(0);"
                 "sum = 0;"
                 "for (i = 0; i < 10000; i += 1) { sum += c.bump(1); }",
       .iterations = 50,
       .warmup = 5,
       .supported_jit = 1},

      {.name = "OOP Method Calls (10k iterations)",
       .script = "class Accumulator {"
                 "  constructor(base) { this.base = base; }"
                 "  add(x) { return this.base + x; }"
                 "};"
                 "a = new Accumulator(2);"
                 "sum = 0;"
                 "for (i = 0; i < 10000; i += 1) { sum += a.add(i); }",
       .iterations = 50,
       .warmup = 5,
       .supported_jit = 1},

      {.name = "OOP Inherited Method Calls (10k iterations)",
       .script = "class Base { value(x) { return x + 1; } };"
                 "class Child extends Base { value(x) { return super.value(x) + 1; } };"
                 "c = new Child();"
                 "sum = 0;"
                 "for (i = 0; i < 10000; i += 1) { sum += c.value(i); }",
       .iterations = 50,
       .warmup = 5,
       .supported_jit = 1},

      {.name = "OOP Inherited Setup Only",
       .script = "class Base { value(x) { return x + 1; } };"
                 "class Child extends Base { value(x) { return super.value(x) + 1; } };"
                 "c = new Child();"
                 "sum = 1;",
       .iterations = 100,
       .warmup = 10,
       .supported_jit = 1},

      {.name = "OOP Inherited Method Calls (100k iterations)",
       .script = "class Base { value(x) { return x + 1; } };"
                 "class Child extends Base { value(x) { return super.value(x) + 1; } };"
                 "c = new Child();"
                 "sum = 0;"
                 "for (i = 0; i < 100000; i += 1) { sum += c.value(i); }",
       .iterations = 20,
       .warmup = 5,
       .supported_jit = 1},

      /* Complex expression */
      {.name = "Complex Expression (10k iterations)",
       .script = "sum = 0; for (i = 0; i < 10000; i += 1) { sum += (sin(i) * cos(i) + sqrt(i * "
                 "i + 1)) / (i + 1); }",
       .iterations = 50,
       .warmup = 5,
       .supported_jit = 1},
  };

  int num_benchmarks = sizeof(benchmarks) / sizeof(benchmarks[0]);

  if (list_only) {
    for (int i = 0; i < num_benchmarks; i++) {
      printf("%s\n", benchmarks[i].name);
    }
    printf("Vector Access (pre-bound, 10k iterations)\n");
    return 0;
  }

  for (int i = 0; i < num_benchmarks; i++) {
    if (!text_matches_filter(benchmarks[i].name, filter)) continue;
    benchmark_t bench = scaled_benchmark(benchmarks[i], quick);
    run_benchmark(&bench);
  }

  /* Vector access benchmark (requires pre-bound vector) */
  if (!text_matches_filter("Vector Access (pre-bound, 10k iterations)", filter)) {
    return 0;
  }

  printf("\n=== Vector Access (pre-bound, 10k iterations) ===\n");
  turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  turbo_script_ctx_t *ctx_exec_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  configure_bench_ctx(ctx_jit);
  configure_bench_ctx(ctx_exec_jit);
  configure_bench_ctx(ctx_interp);

  double *vec_data = (double *)malloc(1000 * sizeof(double));
  for (int i = 0; i < 1000; i++) {
    vec_data[i] = (double)i;
  }

  ts_bind_vec(ctx_jit, "data", vec_data, 1000);
  ts_bind_vec(ctx_exec_jit, "data", vec_data, 1000);
  ts_bind_vec(ctx_interp, "data", vec_data, 1000);

  const char *vec_script = "sum = 0; for (i = 0; i < 10000; i += 1) { sum += data[i % 1000]; }";

  if (turbo_script_run(ctx_interp, vec_script) != 0 ||
      turbo_script_run_jit(ctx_jit, vec_script) != 0 ||
      turbo_script_compile_mir(ctx_exec_jit, vec_script) != 0 ||
      turbo_script_exec_jit(ctx_exec_jit) != 0) {
    printf("  FAIL: vector benchmark rejected script\n");
    free(vec_data);
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return 1;
  }
  double vec_interp = ts_get_num(ctx_interp, "sum");
  double vec_run_jit = ts_get_num(ctx_jit, "sum");
  double vec_exec_jit = ts_get_num(ctx_exec_jit, "sum");
  if (!same_result(vec_interp, vec_run_jit) || !same_result(vec_interp, vec_exec_jit)) {
    printf("  FAIL: vector results differ: interp=%.12g run_jit=%.12g exec_jit=%.12g\n",
           vec_interp, vec_run_jit, vec_exec_jit);
    free(vec_data);
    turbo_script_free(ctx_jit);
    turbo_script_free(ctx_exec_jit);
    turbo_script_free(ctx_interp);
    return 1;
  }

  for (int i = 0; i < 10; i++) {
    reset_runtime_limits(ctx_jit);
    reset_runtime_limits(ctx_exec_jit);
    reset_runtime_limits(ctx_interp);
    turbo_script_run_jit(ctx_jit, vec_script);
    turbo_script_exec_jit(ctx_exec_jit);
    turbo_script_run(ctx_interp, vec_script);
  }

  double time_jit = benchmark_run_jit(ctx_jit, vec_script, 100);
  double time_exec_jit = benchmark_exec_jit(ctx_exec_jit, 100);
  double time_interp = benchmark_interp(ctx_interp, vec_script, 100);

  printf("Results:\n");
  printf("  Interpreter:      %.3f ms total, %.6f ms/iter\n", time_interp, time_interp / 100);
  printf("  JIT run_jit warm: %.3f ms total, %.6f ms/iter, speedup %.2fx\n",
         time_jit, time_jit / 100, time_interp / time_jit);
  printf("  JIT exec only:    %.3f ms total, %.6f ms/iter, speedup %.2fx\n",
         time_exec_jit, time_exec_jit / 100, time_interp / time_exec_jit);
  printf("  Result:           %.6f (verified)\n", vec_interp);
  fflush(stdout);

  free(vec_data);
  turbo_script_free(ctx_jit);
  turbo_script_free(ctx_exec_jit);
  turbo_script_free(ctx_interp);

  /* Summary */
  printf("\n======================================\n");
  printf("Benchmark Summary:\n");
  printf("  - Interpreter and JIT results are verified before timing\n");
  printf("  - run_jit warm path measures cache-hit API overhead plus execution\n");
  printf("  - exec only path measures a precompiled MIR function\n");
  printf("  - compile once reports standalone MIR lowering/codegen cost\n");
  printf("\nRecommendations:\n");
  printf("  - Use JIT for loops with >1000 iterations\n");
  printf("  - Use JIT for math-heavy computations\n");
  printf("  - Use interpreter for one-off scripts\n");
  printf("  - Pre-bind vectors for best performance\n");

  return 0;
}
