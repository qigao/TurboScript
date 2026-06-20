#include "turbo_script.h"
#include "../src/turbo_script_internal.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>

static void bench_reset_runtime(turbo_script_ctx_t *ctx) {
  if (!ctx)
    return;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_nodes = 0;
  ctx->env.aborted = 0;
}

spec("TurboScript Benchmark") {

  describe("Interpreter Performance (No MIR)") {

    describe("Loops and Math Fixture") {
      turbo_script_ctx_t *ctx = NULL;

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Loops and Math") {
        benchmark("1000 iters loop", 1000, 1.0) {
          turbo_script_run(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }");
        }

        benchmark("10000 iters loop", 100, 1.0) {
          turbo_script_run(ctx, "sum = 0; for (i = 0; i < 10000; i += 1) { sum += i; }");
        }

        benchmark("complex math 1000", 1000, 1.0) {
          const char *script =
              "a = 10.5; b = 20.3; for(i=0; i<1000; i+=1) { c = (a * b) + (a / b) - (a + b); }";
          turbo_script_run(ctx, script);
        }
      }
    }

    describe("Reuse Context Fixture") {
      turbo_script_ctx_t *ctx = NULL;

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Re-using context (Eval only)") {
        benchmark("loop 1000 (re-use ctx)", 1000, 1.0) {
          turbo_script_run(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }");
        }
      }
    }

    describe("Compile vs Run Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      turbo_script_compiled_t *compiled = NULL;
      const char *script = "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        compiled = turbo_script_compile(ctx, script);
        check_not_null(compiled);
      }

      after_each() {
        turbo_script_compiled_free(compiled);
        compiled = NULL;
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Compile vs Run") {
        benchmark("run (parse + eval)", 1000, 1.0) {
          turbo_script_run(ctx, script);
        }

        benchmark("exec (eval only)", 1000, 1.0) {
          turbo_script_exec(ctx, compiled);
        }
      }
    }

    describe("Function Overhead Fixture") {
      turbo_script_ctx_t *ctx = NULL;

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        turbo_script_run(ctx, "func add(a, b) { return a + b; }");
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Function Overhead") {
        benchmark("inline add 1000", 1000, 1.0) {
          turbo_script_run(ctx, "sum = 0; for(i=0; i<1000; i+=1) { sum = sum + 1 + 2; }");
        }

        benchmark("func call add 1000", 1000, 1.0) {
          turbo_script_run(ctx, "sum = 0; for(i=0; i<1000; i+=1) { sum = add(sum, 1); }");
        }
      }
    }
  }

  describe("Native JIT Comparison") {

    describe("Small Loop Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script = "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Small Loop (1,000 iterations)") {
        benchmark("Interpreter", 1000, 1.0) {
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Large Loop Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script = "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Large Loop (100,000 iterations)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 100, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Arithmetic Intensity Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "a = 1.5; b = 2.7; c = 0.0;"
          "for (i = 0; i < 10000; i += 1) {"
          "  c = (a * b + c) * 0.999 - (a / (b + 1.0));"
          "  a = a + 0.001; b = b - 0.001;"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Arithmetic Intensity (FMA-heavy)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Nested Loops Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "total = 0;"
          "for (i = 0; i < 100; i += 1) {"
          "  for (j = 0; j < 100; j += 1) {"
          "    total += i * j;"
          "  }"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Nested Loops (100 x 100)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Branchy Code Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "count = 0;"
          "for (i = 0; i < 10000; i += 1) {"
          "  r = i % 7;"
          "  if (r == 0) { count += 1; }"
          "  else if (r == 1) { count += 2; }"
          "  else if (r == 2) { count += 3; }"
          "  else if (r == 3) { count += 4; }"
          "  else { count += 5; }"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Branchy Code (if/else chain)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Compound Assignment Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "a = 1.0; b = 2.0; c = 3.0;"
          "for (i = 0; i < 10000; i += 1) {"
          "  a += b; b -= 0.001; c *= 0.9999;"
          "  a /= 1.0001;"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Compound Assignment Ops") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Function Calls Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "sum = 0;"
          "for (i = 0; i < 5000; i += 1) {"
          "  sum += square(i);"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 100000000;
        turbo_script_run(ctx, "func square(x) { return x * x; }");
        bench_reset_runtime(ctx);
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Function Calls in Loop") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT (bridge)", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Comparison Heavy Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "lo = 0; hi = 0; val = 0;"
          "for (i = 0; i < 10000; i += 1) {"
          "  val = (i * 7 + 13) % 1000;"
          "  if (val > hi) { hi = val; }"
          "  if (val < lo) { lo = val; }"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Comparison-Heavy (min/max search)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Switch Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "total = 0;"
          "for (i = 0; i < 5000; i += 1) {"
          "  m = i % 4;"
          "  switch (m) {"
          "    case 0: { total += 1; }"
          "    case 1: { total += 2; }"
          "    case 2: { total += 3; }"
          "    default: { total += 4; }"
          "  }"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Switch in Loop") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Power Modulo Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "sum = 0;"
          "for (i = 1; i < 5000; i += 1) {"
          "  sum += (i ^ 0.5) + (i % 17);"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Power & Modulo (math-heavy)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Logical Operators Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "count = 0;"
          "for (i = 0; i < 10000; i += 1) {"
          "  if ((i > 100 && i < 9000) || i % 2 == 0) {"
          "    count += 1;"
          "  }"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Logical Operators (short-circuit)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Vector Sum Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      double *vec_data = NULL;
      const char *script =
          "sum = 0;"
          "for (i = 0; i < n; i += 1) {"
          "  sum += v[i];"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        vec_data = (double *)malloc(10000 * sizeof(double));
        check_not_null(vec_data);
        for (int i = 0; i < 10000; i++)
          vec_data[i] = (double)i;
        ts_bind_vec(ctx, "v", vec_data, 10000);
        ts_bind_num(ctx, "n", 10000);
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        free(vec_data);
        vec_data = NULL;
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Vector Sum (10,000 elements)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Dot Product Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      double *a_data = NULL;
      double *b_data = NULL;
      const char *script =
          "dot = 0;"
          "for (i = 0; i < 1000; i += 1) {"
          "  dot += a[i] * b[i];"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        a_data = (double *)malloc(1000 * sizeof(double));
        b_data = (double *)malloc(1000 * sizeof(double));
        check_not_null(a_data);
        check_not_null(b_data);
        for (int i = 0; i < 1000; i++) {
          a_data[i] = (double)i * 0.1;
          b_data[i] = (double)(1000 - i) * 0.1;
        }
        ts_bind_vec(ctx, "a", a_data, 1000);
        ts_bind_vec(ctx, "b", b_data, 1000);
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        free(a_data);
        free(b_data);
        a_data = NULL;
        b_data = NULL;
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Dot Product (1,000 elements)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Map Field Access Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script =
          "sum = 0;"
          "for (i = 0; i < 10000; i += 1) {"
          "  sum += cfg.x * cfg.y + cfg.z;"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_run(ctx, "cfg = map {x: 1.5, y: 2.7, z: 0.3};");
        turbo_script_compile_mir(ctx, script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Map Field Access (pre-bound, 10K reads)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, script);
        }

        benchmark("MIR JIT", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }
  }

  describe("Lifecycle & Engine Overheads") {

    bench("Init/Free Costs") {
      benchmark("init_bare + free", 100, 1.0) {
        turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
        turbo_script_free(ctx);
      }

      benchmark("init_full + free", 100, 1.0) {
        turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        turbo_script_free(ctx);
      }
    }

    describe("Compile Parse Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *script = "x = 1; y = 2; z = x + y;";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Compile/Parse Costs") {
        benchmark("compile/parse only", 1000, 1.0) {
          turbo_script_compiled_t *compiled = turbo_script_compile(ctx, script);
          turbo_script_compiled_free(compiled);
        }
      }
    }
  }

  describe("Closure JIT Performance") {

    describe("Simple Closure Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *setup =
          "func makeAdder(x) {"
          "  return func(y) { return x + y; };"
          "}"
          "add5 = makeAdder(5);";
      const char *test_script = "result = add5(10);";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_run(ctx, setup);
        bench_reset_runtime(ctx);
        turbo_script_compile_mir(ctx, test_script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Simple Closure (1 captured var)") {
        benchmark("Interpreter", 1000, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, test_script);
        }

        benchmark("JIT (closure)", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Multiple Captures Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *setup =
          "func makeCalc(a, b) {"
          "  return func(x) { return a * x + b; };"
          "}"
          "calc = makeCalc(2, 3);";
      const char *test_script = "result = calc(5);";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_run(ctx, setup);
        bench_reset_runtime(ctx);
        turbo_script_compile_mir(ctx, test_script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Multiple Captures (2 vars)") {
        benchmark("Interpreter", 1000, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, test_script);
        }

        benchmark("JIT (closure)", 1000, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Closure in Loop Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *setup =
          "func makeMultiplier(factor) {"
          "  return func(n) { return factor * n; };"
          "}"
          "times3 = makeMultiplier(3);";
      const char *test_script =
          "sum = 0;"
          "for (i = 1; i <= 1000; i += 1) {"
          "  sum += times3(i);"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_run(ctx, setup);
        bench_reset_runtime(ctx);
        turbo_script_compile_mir(ctx, test_script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Closure in Loop (1000 calls)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, test_script);
        }

        benchmark("JIT (closure)", 100, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("Complex Closure Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *setup =
          "func makeFormula(a, b, c) {"
          "  return func(x) { return a * x * x + b * x + c; };"
          "}"
          "quadratic = makeFormula(1, -5, 6);";
      const char *test_script =
          "sum = 0;"
          "for (i = 0; i < 1000; i += 1) {"
          "  sum += quadratic(i);"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_run(ctx, setup);
        bench_reset_runtime(ctx);
        turbo_script_compile_mir(ctx, test_script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("Complex Closure (3 vars, math)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, test_script);
        }

        benchmark("JIT (closure)", 100, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }

    describe("No Capture Fixture") {
      turbo_script_ctx_t *ctx = NULL;
      const char *setup =
          "func makeConst(c) {"
          "  return func() { return 42; };"
          "}"
          "getC = makeConst(999);";
      const char *test_script =
          "sum = 0;"
          "for (i = 0; i < 1000; i += 1) {"
          "  sum += getC();"
          "}";

      before_each() {
        ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_not_null(ctx);
        ctx->env.max_loop_iterations = 1000000;
        ctx->env.max_nodes = 10000000;
        turbo_script_run(ctx, setup);
        bench_reset_runtime(ctx);
        turbo_script_compile_mir(ctx, test_script);
      }

      after_each() {
        turbo_script_free(ctx);
        ctx = NULL;
      }

      bench("No Capture (optimized)") {
        benchmark("Interpreter", 100, 1.0) {
          bench_reset_runtime(ctx);
          turbo_script_run(ctx, test_script);
        }

        benchmark("JIT (no closure)", 100, 1.0) {
          turbo_script_exec_jit(ctx);
        }
      }
    }
  }
}
