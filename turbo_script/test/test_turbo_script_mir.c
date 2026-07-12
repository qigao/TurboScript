#include "../src/turbo_script_internal.h"
#include "tinytest.h"
#include "turbo_script.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define EPS 1e-9

static int count_last_module_functions_containing(turbo_script_ctx_t *ctx, const char *needle) {
  int count = 0;
  DLIST(MIR_module_t) *modules = NULL;
  MIR_module_t last_mod = NULL;

  if (!ctx || !ctx->mir_ctx || !needle)
    return 0;

  modules = MIR_get_module_list(ctx->mir_ctx);
  if (!modules)
    return 0;

  last_mod = DLIST_TAIL(MIR_module_t, *modules);
  if (!last_mod)
    return 0;

  for (MIR_item_t it = DLIST_HEAD(MIR_item_t, last_mod->items); it != NULL;
       it = DLIST_NEXT(MIR_item_t, it)) {
    if (it->item_type == MIR_func_item && strstr(it->u.func->name, needle)) {
      count++;
    }
  }

  return count;
}

spec("turbo_script_mir") {

  describe("compile_mir") {

    it("should compile a simple assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 42;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile arithmetic expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 10; b = 20; c = a + b;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile all binary operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(
                       ctx, "a = 10; b = 3; c = a + b; d = a - b; e = a * b; f = a / b;"),
                   0);
      turbo_script_free(ctx);
    }

    it("should compile compound assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 10; x += 5; x -= 2; x *= 3; x /= 2;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile comparison operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(
                       ctx, "a = 10; b = 20; c = a < b; d = a > b; e = a == b; f = a != b;"),
                   0);
      turbo_script_free(ctx);
    }

    it("should compile a while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "i = 0; while (i < 10) { i += 1; }"), 0);
      turbo_script_free(ctx);
    }

    it("should compile a for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"), 0);
      turbo_script_free(ctx);
    }

    it("should compile an if statement") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 10; if (x > 5) { y = 1; }"), 0);
      turbo_script_free(ctx);
    }

    it("should compile if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 3; if (x > 5) { y = 1; } else { y = 0; }"),
                   0);
      turbo_script_free(ctx);
    }

    it("should compile logical AND/OR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 1; b = 0; c = a && b; d = a || b;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile unary NOT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 1; b = !a;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile modulo") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 10; b = 3; c = a % b;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile power") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "a = 2; b = a ^ 10;"), 0);
      turbo_script_free(ctx);
    }

    it("should compile do-while") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "i = 0; do { i += 1; } while (i < 10);"), 0);
      turbo_script_free(ctx);
    }

    it("should compile break/continue") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 20; i += 1) { if (i == 15) { "
                                        "break; } if (i % 2 == 0) { continue; } sum += i; }"),
          0);
      turbo_script_free(ctx);
    }

    it("should compile function calls") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = sin(3.14);"), 0);
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") {
      check_int_eq(turbo_script_compile_mir(NULL, "x = 1;"), -1);
    }

    it("should return -1 on NULL script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, NULL), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_ARGUMENT);
      turbo_script_free(ctx);
    }

    it("should return -1 on invalid syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "??? +++"), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_PARSE);
      check_not_null(strstr(turbo_script_get_error(ctx), "line 1"));
      turbo_script_free(ctx);
    }

    it("should compile and execute scripts with more than 128 variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char script[4096];
      size_t len = 0;

      memset(script, 0, sizeof(script));
      for (int i = 0; i < 129; ++i) {
        len += (size_t)snprintf(script + len, sizeof(script) - len, "v%d = %d;", i, i);
      }

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_null(strstr(turbo_script_get_error(ctx), "variable limit exceeded"));
      check_double_eq(ts_get_num(ctx, "v0"), 0.0, EPS);
      check_double_eq(ts_get_num(ctx, "v64"), 64.0, EPS);
      check_double_eq(ts_get_num(ctx, "v128"), 128.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("run_jit") {

    it("should run a simple assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 42;"), 0);
      turbo_script_free(ctx);
    }

    it("should run arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "a = 10.5; b = 20.3; c = (a * b) + (a / b) - (a + b);"), 0);
      turbo_script_free(ctx);
    }

    it("should run a while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; i = 0; while (i < 100) { sum += i; i += 1; }"), 0);
      turbo_script_free(ctx);
    }

    it("should run a for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }"), 0);
      turbo_script_free(ctx);
    }

    it("should run nested for loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { for (j = 0; j < 10; j += 1) { sum += 1; } }"),
          0);
      turbo_script_free(ctx);
    }

    it("should run if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; if (x > 5) { y = 1; } else { y = 0; }"), 0);
      turbo_script_free(ctx);
    }

    it("should run compound assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 100; x += 50; x -= 20; x *= 2; x /= 4;"), 0);
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") { check_int_eq(turbo_script_run_jit(NULL, "x = 1;"), -1); }

    it("should return -1 on NULL script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, NULL), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_ARGUMENT);
      turbo_script_free(ctx);
    }

    it("should return -1 on invalid syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "??? +++"), -1);
      turbo_script_free(ctx);
    }
  }

  describe("exec_jit") {

    it("should execute a pre-compiled module") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 42;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      turbo_script_free(ctx);
    }

    it("should execute repeatedly without crash") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"), 0);
      for (int n = 0; n < 100; n++) {
        check_int_eq(turbo_script_exec_jit(ctx), 0);
      }
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") { check_int_eq(turbo_script_exec_jit(NULL), -1); }

    it("should return -1 when no module compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_exec_jit(ctx), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_STATE);
      turbo_script_free(ctx);
    }
  }

  describe("mir interpreter") {
    it("should compile and execute a MIR interpreted module") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check_int_eq(turbo_script_compile_mir_interp(ctx, "x = 41; x += 1;"), 0);
      check_int_eq(turbo_script_exec_mir_interp(ctx), 0);
      check_double_eq(ts_get_num(ctx, "x"), 42.0, EPS);

      turbo_script_free(ctx);
    }

    it("should run a script through the MIR interpreter") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check_int_eq(turbo_script_run_mir_interp(
                       ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"),
                   0);
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);

      turbo_script_free(ctx);
    }

    it("should match public MIR interpreter entrypoint results") {
      turbo_script_ctx_t *ctx_run = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_mir = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "class Counter { constructor(v) { this.value = v; } bump(x) { this.value = this.value + x; return this.value; } };"
          "c = new Counter(0);"
          "sum = 0;"
          "for (i = 0; i < 10; i += 1) { sum += c.bump(2); }";

      check_not_null(ctx_run);
      check_not_null(ctx_mir);
      check_int_eq(turbo_script_run(ctx_run, script), 0);
      check_int_eq(turbo_script_run_mir_interp(ctx_mir, script), 0);
      check_double_eq(ts_get_num(ctx_mir, "sum"), ts_get_num(ctx_run, "sum"), EPS);

      turbo_script_free(ctx_run);
      turbo_script_free(ctx_mir);
    }

    it("should return -1 when no MIR interpreted module is compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check_int_eq(turbo_script_exec_mir_interp(ctx), -1);

      turbo_script_free(ctx);
    }
  }

  describe("compile then exec separation") {

    it("should compile once and exec many times") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 1; x += 1;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      turbo_script_free(ctx);
    }

    it("should handle recompilation") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 1;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_int_eq(turbo_script_compile_mir(ctx, "y = 2;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      turbo_script_free(ctx);
    }

    it("run_jit should equal compile + exec") {
      turbo_script_ctx_t *ctx1 = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx2 = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "a = 5; b = 10; c = a + b;";
      check_int_eq(turbo_script_run_jit(ctx1, script), 0);
      check_int_eq(turbo_script_compile_mir(ctx2, script), 0);
      check_int_eq(turbo_script_exec_jit(ctx2), 0);
      turbo_script_free(ctx1);
      turbo_script_free(ctx2);
    }
  }

  describe("for loop correctness") {

    it("should handle for loop with large iteration count") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }"), 0);
      turbo_script_free(ctx);
    }

    it("should handle for loop with step > 1") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 10; i += 2) { sum += i; }"),
                   0);
      turbo_script_free(ctx);
    }

    it("should handle for loop with zero iterations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 10; i < 0; i += 1) { sum += i; }"),
                   0);
      turbo_script_free(ctx);
    }

    it("should handle nested for + if") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "count = 0;"
                           "for (i = 0; i < 20; i += 1) {"
                           "  if (i > 10) { count += 1; }"
                           "}";
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 1: New operator + control flow tests ===== */

  describe("logical operators") {

    it("should compute AND correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 1 && 1; b = 1 && 0; c = 0 && 1; d = 0 && 0;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 0.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 0.0, EPS);
      check_double_eq(ts_get_num(ctx, "d"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute OR correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 1 || 1; b = 1 || 0; c = 0 || 1; d = 0 || 0;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "d"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute NOT correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = !0; b = !1; c = !5;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 0.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should short-circuit AND") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* If AND short-circuits, the right side (x += 1) should not execute when left is 0 */
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; y = 0 && (x = 99); r = x;"), 0);
      check_double_eq(ts_get_num(ctx, "r"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should short-circuit OR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* If OR short-circuits, the right side should not execute when left is truthy */
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; y = 1 || (x = 99); r = x;"), 0);
      check_double_eq(ts_get_num(ctx, "r"), 10.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("modulo and power") {

    it("should compute modulo correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10 % 3; b = 7 % 2; c = 15 % 5;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute power correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 2 ^ 10; b = 3 ^ 3; c = 10 ^ 0;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1024.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 27.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 1.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("do-while loop") {

    it("should execute body at least once") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 0; do { x += 1; } while (x < 0);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should loop correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; i = 1; do { sum += i; i += 1; } while (i <= 10);"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 55.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("break and continue") {

    it("should break out of for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx, "sum = 0; for (i = 0; i < 100; i += 1) { if (i == 5) { break; } sum += i; }"),
          0);
      /* sum = 0+1+2+3+4 = 10 */
      check_double_eq(ts_get_num(ctx, "sum"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should continue in for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { if (i % 2 == 0) { continue; } sum += i; }"),
          0);
      /* sum = 1+3+5+7+9 = 25 */
      check_double_eq(ts_get_num(ctx, "sum"), 25.0, EPS);
      turbo_script_free(ctx);
    }

    it("should break out of while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "i = 0; while (i < 100) { if (i == 3) { break; } i += 1; }"),
          0);
      check_double_eq(ts_get_num(ctx, "i"), 3.0, EPS);
      turbo_script_free(ctx);
    }

    it("should break out of do-while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(
                       ctx, "i = 0; do { if (i == 3) { break; } i += 1; } while (i < 100);"),
                   0);
      check_double_eq(ts_get_num(ctx, "i"), 3.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 3: Variable bridge tests ===== */

  describe("variable bridge") {

    it("should read variables back via ts_get_num after run_jit") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 42;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute and store arithmetic results") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10; b = 20; c = a + b;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 10.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 20.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 30.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compute for loop sum correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"),
                   0);
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should read pre-set variables inside JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_num(ctx, "input", 100.0);
      check_int_eq(turbo_script_run_jit(ctx, "result = input * 2;"), 0);
      check_double_eq(ts_get_num(ctx, "result"), 200.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle compound assignments correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 100; x += 50; x -= 20; x *= 2; x /= 4;"), 0);
      /* (100+50-20)*2/4 = 130*2/4 = 65 */
      check_double_eq(ts_get_num(ctx, "x"), 65.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle if-else variable assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; if (x > 5) { y = 1; } else { y = 0; }"), 0);
      check_double_eq(ts_get_num(ctx, "y"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle nested loops with variable bridge") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { for (j = 0; j < 10; j += 1) { sum += 1; } }"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 100.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; for (i = 1; i <= 50; i += 1) { sum += i * i; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "sum"), ts_get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 4: Function call bridge tests ===== */

  describe("function call bridge") {

    it("should call sin from JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sin(0);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call abs from JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = abs(-42);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call max with 2 args") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = max(10, 20);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 20.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call min with 2 args") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = min(10, 20);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call functions in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 10; i += 1) { sum += abs(-1); }"), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter function call results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = sqrt(144) + floor(3.7) + ceil(2.1);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "x"), ts_get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix metadata and solve helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "A = [2, 1, 1, 3]; "
                           "shape = matrix.shape(A, 2, 2); "
                           "info = matrix.info(A, 2, 2); "
                           "X = matrix.solve(A, [5, 10], 2, 1); "
                           "result = shape[0] + shape[1] + info.rows + info.cols + X[0] + X[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 12.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix determinant and norm helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "Acol = [1, 0, 5, 2, 1, 6, 3, 4, 0]; "
                           "Arow = [1, 2, 3, 0, 1, 4, 5, 6, 0]; "
                           "Mcol = [1, -4, -2, 5, 3, -6]; "
                           "Mrow = [1, -2, 3, -4, 5, -6]; "
                           "d0 = matrix.det(Acol, 3); "
                           "d1 = matrix.det(Arow, 3, 1); "
                           "nf = round(matrix.norm([1, 2, 3, 4], 2, 2) * 1000); "
                           "nl1 = matrix.norm(Mcol, 2, 3, \"l1\"); "
                           "ninf = matrix.norm(Mrow, 2, 3, \"inf\", 1); "
                           "result = d0 + d1 + nf + nl1 + ninf;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 5503.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix trace and diagonal helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "Mcol = [1, 3, 5, 2, 4, 6]; "
                           "Mrow = [1, 2, 3, 4, 5, 6]; "
                           "v0 = matrix.diag(Mcol, 3, 2); "
                           "v1 = matrix.diag(Mrow, 3, 2, 1); "
                           "t0 = matrix.trace(Mcol, 3, 2); "
                           "t1 = matrix.trace(Mrow, 3, 2, 1); "
                           "a0 = v0[0]; "
                           "a1 = v0[1]; "
                           "b0 = v1[0]; "
                           "b1 = v1[1]; "
                           "result = t0 + t1 + a0 + a1 + b0 + b1;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 20.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix transpose helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "Mcol = [1, 4, 2, 5, 3, 6]; "
                           "Mrow = [1, 2, 3, 4, 5, 6]; "
                           "Tcol = matrix.transpose(Mcol, 2, 3); "
                           "Trow = matrix.transpose(Mrow, 2, 3, 1); "
                           "result = Tcol[0] + Tcol[1] + Tcol[2] + Tcol[3] + Tcol[4] + Tcol[5] + "
                           "Trow[0] + Trow[1] + Trow[2] + Trow[3] + Trow[4] + Trow[5];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix constructors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "Z = matrix.zeros(2, 3); "
                           "O = matrix.ones(2, 3, 1); "
                           "F = matrix.full(2, 3, 7); "
                           "A = matrix.full(2, 3, 4, 1); "
                           "zn = vec.len(Z); "
                           "z0 = Z[0]; z5 = Z[5]; "
                           "o0 = O[0]; o5 = O[5]; "
                           "f0 = F[0]; f5 = F[5]; "
                           "a0 = A[0]; a5 = A[5]; "
                           "result = zn + z0 + z5 + o0 + o5 + f0 + f5 + a0 + a5;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 30.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for elementwise matrix helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "A = [1, 2, 3, 4]; "
                           "B = [10, 20, 30, 40]; "
                           "S = matrix.add(A, B, 2, 2); "
                           "D = matrix.sub(B, A, 2, 2, 1); "
                           "H = matrix.hadamard(A, B, 2, 2); "
                           "C = matrix.scale(A, 2, 2, 2); "
                           "result = S[0] + S[3] + D[1] + H[2] + C[3];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 171.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix reduce helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "Mcol = [1, 4, 2, 5, 3, 6]; "
                           "Mrow = [1, 2, 3, 4, 5, 6]; "
                           "cs = matrix.sum(Mcol, 2, 3, \"col\"); "
                           "rs = matrix.sum(Mrow, 2, 3, 1, 1); "
                           "cm = matrix.mean(Mrow, 2, 3, 0, 1); "
                           "rmax = matrix.max(Mrow, 2, 3, \"row\", 1); "
                           "cmin = matrix.min(Mrow, 2, 3, 0, 1); "
                           "total = matrix.sum(Mcol, 2, 3); "
                           "avg = matrix.mean(Mcol, 2, 3); "
                           "mn = matrix.min(Mcol, 2, 3); "
                           "mx = matrix.max(Mcol, 2, 3); "
                           "cs0 = cs[0]; cs2 = cs[2]; "
                           "rs0 = rs[0]; rs1 = rs[1]; "
                           "cm0 = cm[0]; cm2 = cm[2]; "
                           "rm0 = rmax[0]; rm1 = rmax[1]; "
                           "cmin0 = cmin[0]; cmin2 = cmin[2]; "
                           "result = total + avg + mn + mx + cs0 + cs2 + rs0 + rs1 + "
                           "cm0 + cm2 + rm0 + rm1 + cmin0 + cmin2;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 86.5, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix row and column helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "Mcol = [1, 4, 2, 5, 3, 6]; "
                           "Mrow = [1, 2, 3, 4, 5, 6]; "
                           "r = matrix.row(Mcol, 2, 3, 1); "
                           "c = matrix.col(Mrow, 2, 3, 2, 1); "
                           "result = r[0] + r[1] + r[2] + c[0] + c[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 24.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for extended matrix helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "M = [1, 2, 3, 4, 5, 6]; "
                           "S = matrix.slice(M, 2, 3, 0, 2, 1, 2); "
                           "E = matrix.eye_like(M, 2, 3); "
                           "O = matrix.outer([1, 2], [10, 20, 30]); "
                           "AR = matrix.add_row(M, [10, 20, 30], 2, 3); "
                           "MC = matrix.mul_col(M, [2, 3], 2, 3); "
                           "result = S[0] + S[3] + E[0] + E[3] + O[5] + AR[4] + MC[1] + "
                           "matrix.dot([1, 2, 3], [4, 5, 6]);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 144.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for matrix stats linalg and distributions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "M = [1, 4, 2, 5, 3, 6]; "
                           "cv = mat_var(M, 2, 3, \"col\"); "
                           "QR = linalg.qr([1, 0, 0, 1], 2, 2); "
                           "Q = QR[0]; R = QR[1]; "
                           "P = linalg.pinv([1, 0, 0, 1], 2, 2); "
                           "X = linalg.lstsq([1, 0, 0, 1], [7, 9], 2, 2); "
                           "SV = linalg.svd([3, 0, 0, 4], 2, 2); "
                           "T = stats.t_test_1samp([1, 2, 3, 4], 2.5); "
                           "mv = round(mat_var(M, 2, 3) * 1000); "
                           "mx = matrix.argmax(M, 2, 3); "
                           "cv0 = round(cv[0] * 1000); "
                           "q0 = Q[0]; r3 = R[3]; p0 = P[0]; x0 = X[0]; x1 = X[1]; "
                           "sv0 = SV[0]; sv1 = SV[1]; nc = round(stats.normal_cdf(0) * 1000); "
                           "tdf = T.df; "
                           "result = mv + mx + cv0 + q0 + r3 + p0 + x0 + x1 + sv0 + sv1 + nc + tdf;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "mv"), ts_get_num(ctx_interp, "mv"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "mx"), ts_get_num(ctx_interp, "mx"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "cv0"), ts_get_num(ctx_interp, "cv0"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "q0"), ts_get_num(ctx_interp, "q0"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "r3"), ts_get_num(ctx_interp, "r3"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "p0"), ts_get_num(ctx_interp, "p0"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "x0"), ts_get_num(ctx_interp, "x0"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "x1"), ts_get_num(ctx_interp, "x1"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "sv0"), ts_get_num(ctx_interp, "sv0"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "sv1"), ts_get_num(ctx_interp, "sv1"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "nc"), ts_get_num(ctx_interp, "nc"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "tdf"), ts_get_num(ctx_interp, "tdf"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 5701.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for extended linear algebra helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "A = [4, 2, 2, 3]; "
                           "LU = linalg.lu(A, 2); "
                           "L = LU[0]; U = LU[1]; P = LU[2]; "
                           "X = linalg.lu_solve(A, [8, 8], 2, 1); "
                           "C = linalg.solve_cholesky(A, [8, 8], 2, 1); "
                           "E = linalg.eigh([2, 1, 1, 2], 2); "
                           "S = linalg.svd([1, 0, 0, 0, 2, 0], 3, 2); "
                           "SL = matrix.slogdet(A, 2); "
                           "RLU = linalg.lu([0, 1, 2, 3], 2, 1); "
                           "RP = RLU[2]; "
                           "RX = linalg.lu_solve([0, 1, 2, 3], [1, 5], 2, 1, 1); "
                           "result = L[0] + L[1] + U[0] + U[3] + P[0] + P[1] + "
                           "X[0] + X[1] + C[0] + C[1] + matrix.rank(A, 2, 2) + "
                           "matrix.rank([1, 2, 2, 4], 2, 2) + matrix.cond([2, 0, 0, 4], 2, 2) + "
                           "E[0] + E[1] + S[0] + S[1] + linalg.det_lu(A, 2) + SL[0] + "
                           "round(SL[1] * 1000) + RP[0] + RP[1] + RX[0] + RX[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 2117.5, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for table helpers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "rows = list(map{id:1, group:\"a\", value:10}, "
                           "map{id:2, group:\"a\", value:20}, map{id:3, group:\"b\", value:5}); "
                           "right = list(map{id:1, name:\"one\"}, map{id:3, name:\"three\"}); "
                           "func keep_big(row) { return row.value > 9; } "
                           "sel = table.select(rows, \"id\", \"value\"); "
                           "flt = table.filter(rows, \"keep_big\"); "
                           "grp = table.groupby(rows, \"group\", \"value\", \"mean\"); "
                           "joined = table.join(rows, right, \"id\"); "
                           "result = sel.length() + sel[0].value + flt.length() + grp.a.value + "
                           "grp.b.value + joined.length() + joined[1].id;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 40.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for DataBind JSON and CSV bindings") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx_interp, "data_bind"), 0);
      check_int_eq(turbo_script_load_plugin(ctx_jit, "data_bind"), 0);
      const char *script =
          "schema = \"message Trade { double price; uint32 qty; string symbol; }\";"
          "codec = data_bind.create_from_text(schema);"
          "one = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2}\";"
          "many = \"[{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2},"
          "{\\\"symbol\\\":\\\"MSFT\\\",\\\"price\\\":20,\\\"qty\\\":3}]\";"
          "csv_text = \"symbol,price,qty\\nAAPL,10.5,2\\nMSFT,20,3\";"
          "trade = data_bind.json(codec, \"Trade\", one);"
          "rows = data_bind.json_all(codec, \"Trade\", many);"
          "csv_row = data_bind.csv(codec, \"Trade\", csv_text, 0);"
          "result = trade.price * trade.qty + rows[1].price * rows[1].qty + csv_row.qty;"
          "data_bind.close(codec);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 83.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 5: Unsupported-node rejection tests ===== */

  describe("unsupported nodes") {

    it("should match interpreter for string literal assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = \"text\"; result = x.length;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_str_eq(ts_get_str(ctx_jit, "x"), ts_get_str(ctx_interp, "x"));
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for template string assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "name = \"Ada\"; x = `Hi ${name} ${40 + 2}`; result = x.length;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_str_eq(ts_get_str(ctx_jit, "x"), ts_get_str(ctx_interp, "x"));
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for dynamic variable value copies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = \"text\"; y = x; result = y.length;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_str_eq(ts_get_str(ctx_jit, "y"), ts_get_str(ctx_interp, "y"));
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map literal assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 2, b: 3}; result = m.a * 10 + m.b;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for array destructuring with rest") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "[a, null, ...rest] = [10, 20, 30, 40];"
                           "result = a + rest[0] + rest[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 80.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map destructuring with rest") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 1, b: 2, c: 3};"
                           "map {a, ...rest} = m;"
                           "result = a * 10 + rest.b + rest.c;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 15.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for function expressions with closures") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "base = 40; add = (x) => base + x; result = add(2);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for arrow functions as first-class values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func apply(f, x) { return f(x); }"
                           "block = (x) => { y = x * x; return y + 1; };"
                           "result = apply(x => x * 3, 7) + block(5);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 47.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for function calls with value arguments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add([a, b]) { a + b }; result = add([20, 22]);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for destructured function parameters") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func score(prefix, [a, b, ...rest], map {bonus: extra}) {"
                           "  return prefix + a + b + rest[0] + extra;"
                           "}"
                           "result = score(1, [10, 20, 9], map {bonus: 2});";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for destructured arrow parameters") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "combine = ([a, b], map {x: y}) => a + b + y;"
                           "result = combine([10, 20], map {x: 12});";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for spread call arguments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func sum(a, b, c) { a + b + c }; args = [10, 20, 12]; result = sum(...args);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for try catch throw") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = try { throw 40; } catch(e) { e + 2; }; check = result;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "check"), ts_get_num(ctx_interp, "check"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "check"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for structured thrown values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "msg = try { throw \"bad\"; } catch(e) { typeof(e); };"
                           "result = try { throw map {code: 40}; } catch(err) { err.code + 2; };";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_str_eq(ts_get_str(ctx_jit, "msg"), ts_get_str(ctx_interp, "msg"));
      check_str_eq(ts_get_str(ctx_jit, "msg"), "string");
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for caught runtime errors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "missing = try { no_such_fn(1); } catch(e) { typeof(e); };"
                           "member = try { x = 1; x.length; } catch(e) { typeof(e); };"
                           "result = (missing == \"string\") * 10 + (member == \"string\");";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 11.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 6: Vector indexing tests ===== */

  describe("vector indexing") {

    it("should access vector elements by index") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      ts_bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "a = v[0]; b = v[2]; c = v[4];"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 10.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 30.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 50.0, EPS);
      turbo_script_free(ctx);
    }

    it("should access vector in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "arr", data, 5);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 5; i += 1) { sum += arr[i]; }"), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 15.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter vector indexing") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {2.0, 4.0, 6.0, 8.0};
      ts_bind_vec(ctx_interp, "v", data, 4);
      ts_bind_vec(ctx_jit, "v", data, 4);
      const char *script = "sum = 0; for (i = 0; i < 4; i += 1) { sum += v[i]; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "sum"), ts_get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for non-variable array indexing") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = ([10, 20, 30])[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 6: User-defined function tests ===== */

  describe("user-defined functions") {

    it("should define and call a function") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "func double(x) { return x * 2; } r = double(21);"),
                   0);
      check_double_eq(ts_get_num(ctx, "r"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compile more than 64 script functions into MIR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char script[8192];
      size_t len = 0;

      memset(script, 0, sizeof(script));
      for (int i = 0; i < 70; ++i) {
        len += (size_t)snprintf(script + len, sizeof(script) - len,
                                "func f%d(x) { return x + %d; } ", i, i);
      }
      len += (size_t)snprintf(script + len, sizeof(script) - len, "result = f69(1);");

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "_ts_f"), 70);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 70.0, EPS);
      turbo_script_free(ctx);
    }

    it("should specialize monomorphic higher-order function calls into MIR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add1(x) { return x + 1; }"
                           "func apply(f, x) { return f(x); }"
                           "result = apply(add1, 41);";

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "hof_apply_add1"), 1);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 42.0, EPS);

      turbo_script_free(ctx);
    }

    it("should specialize repeated flat monomorphic higher-order calls") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add1(x) { return x + 1; }"
                           "func both(f, x, y) { return f(x) + f(y); }"
                           "result = both(add1, 20, 20);";

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "hof_both_add1"), 1);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 42.0, EPS);

      turbo_script_free(ctx);
    }

    it("should specialize nested monomorphic higher-order calls") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add1(x) { return x + 1; }"
                           "func twice(f, x) { return f(f(x)); }"
                           "result = twice(add1, 40);";

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "hof_twice_add1"), 1);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 42.0, EPS);

      turbo_script_free(ctx);
    }

    it("should specialize composed monomorphic higher-order calls") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func inc(x) { return x + 1; }"
                           "func triple(x) { return x * 3; }"
                           "func compose(f, g, x) { return f(g(x)); }"
                           "result = compose(inc, triple, 13);";

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "hof_compose_inc_triple"), 1);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 40.0, EPS);

      turbo_script_free(ctx);
    }

    it("should specialize monomorphic higher-order calls with local temporaries") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add1(x) { return x + 1; }"
                           "func stage(f, x) { y = f(x); return f(y); }"
                           "result = stage(add1, 40);";

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "hof_stage_add1"), 1);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 42.0, EPS);

      turbo_script_free(ctx);
    }

    it("should specialize inline monomorphic higher-order lambdas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func apply(f, x) { return f(x); }"
                           "result = apply(x => x + 1, 41);";

      check_int_eq(turbo_script_compile_mir(ctx, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx, "hof_apply___lambda"), 1);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "result"), 42.0, EPS);

      turbo_script_free(ctx);
    }

    it("should call native MIR functions that read outer variables") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "base = 40;"
                           "func addBase(x) { return base + x; }"
                           "result = addBase(2);";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_compile_mir(ctx_jit, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx_jit, "_ts_addBase"), 1);
      check_int_eq(turbo_script_exec_jit(ctx_jit), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should specialize captured inline higher-order lambdas") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func apply(f, x) { return f(x); }"
                           "base = 40;"
                           "result = apply(x => base + x, 2);";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_compile_mir(ctx_jit, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx_jit, "hof_apply___lambda"), 1);
      check_int_eq(turbo_script_exec_jit(ctx_jit), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should specialize nested captured inline higher-order lambdas") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func twice(f, x) { return f(f(x)); }"
                           "base = 1;"
                           "result = twice(x => base + x, 40);";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_compile_mir(ctx_jit, script), 0);
      check_int_eq(count_last_module_functions_containing(ctx_jit, "hof_twice___lambda"), 1);
      check_int_eq(turbo_script_exec_jit(ctx_jit), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should call user function in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "func add(a, b) { return a + b; }"
                                    "sum = 0; for (i = 0; i < 10; i += 1) { sum = add(sum, i); }"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 45.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter user function results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func square(x) { return x * x; }"
                           "sum = 0; for (i = 1; i <= 5; i += 1) { sum += square(i); }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "sum"), ts_get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for default function arguments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add(x, y = 10) { return x + y; }"
                           "func greet(name = \"world\") { return name; }"
                           "result = add(5) + add(5, 20);"
                           "label = greet();";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 40.0, EPS);
      check_str_eq(ts_get_str(ctx_jit, "label"), ts_get_str(ctx_interp, "label"));
      check_str_eq(ts_get_str(ctx_jit, "label"), "world");
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for pipe operator calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add1(x) { return x + 1; }"
                           "func mul2(x) { return x * 2; }"
                           "result = 3 |> add1() |> mul2();";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 8.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for rest parameters") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func sum(...args) {"
                           "  total = 0;"
                           "  for (v in args) { total += v; }"
                           "  return total;"
                           "}"
                           "mul = (m, ...rest) => {"
                           "  total = 0;"
                           "  for (v in rest) { total += v; }"
                           "  return total * m;"
                           "};"
                           "result = sum(1, 2, 3, 4) + mul(10, 1, 2, 3);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 70.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for mixed rest values and list spread") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func inspect(...args) {"
                           "  return is_list(args) * 1000 + (args[0] == \"x\") * 100 + "
                           "         args[1].code * 10 + args.length();"
                           "}"
                           "func unpack(a, b, c) { return (a == \"x\") * 100 + b.code * 10 + c; }"
                           "values = list(\"x\", map {code: 4}, 2);"
                           "result = inspect(\"x\", map {code: 4}) + unpack(...values);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 1284.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for while in script function bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func calc(n) {"
                           "  total = 0;"
                           "  i = 0;"
                           "  while (i <= n) {"
                           "    if (i == 2) { i += 1; continue; }"
                           "    total += i;"
                           "    if (total > 8) { break; }"
                           "    i += 1;"
                           "  }"
                           "  return total;"
                           "}"
                           "result = calc(6);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 13.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for for and do-while in script function bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func calc(n) {"
                           "  total = 0;"
                           "  for (i = 0; i < n; i += 1) { total += i; }"
                           "  j = 0;"
                           "  do { total += 2; j += 1; } while (j < 3);"
                           "  return total;"
                           "}"
                           "result = calc(5);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 16.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for for-in in script function bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func collect(v, l, m) {"
                           "  total = 0;"
                           "  for (x in v) { total += x; }"
                           "  for (y in l) { total += y; }"
                           "  for (k in m) { total += m[k]; }"
                           "  return total;"
                           "}"
                           "result = collect([1, 2], list(3, 4), map {a: 5, b: 6});";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 21.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for switch in script function bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func choose(x) {"
                           "  switch (x) {"
                           "    case 1: { return 10; }"
                           "    case \"a\": { return 20; }"
                           "    default: { return 30; }"
                           "  }"
                           "}"
                           "result = choose(1) + choose(\"a\") + choose(0);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 60.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for destructuring in script function bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func unpack() {"
                           "  [a, b, ...rest] = [10, 20, 30, 40];"
                           "  map {x, ...extra} = map {x: 1, y: 2, z: 3};"
                           "  return a + b + rest[0] + rest[1] + x + extra.y + extra.z;"
                           "}"
                           "result = unpack();";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 106.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for nested named functions in script bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func outer(base) {"
                           "  func inner(x) { return base + x; }"
                           "  return inner(2);"
                           "}"
                           "result = outer(40);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for nested closures returned from script bodies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make(base) {"
                           "  return (x) => {"
                           "    func adjust(y) { return base + y; }"
                           "    return adjust(x);"
                           "  };"
                           "}"
                           "add = make(40);"
                           "result = add(2);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("OOP helper lowering") {

    it("should match interpreter for bound methods stored in variables") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(value) { this.value = value; }"
                           "  next() { this.value = this.value + 1; return this.value; }"
                           "};"
                           "counter = Counter(41);"
                           "next = counter.next;"
                           "result = next();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for isolated instance fields") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  constructor(value) { this.value = value; }"
                           "  set(value) { this.value = value; }"
                           "  get() { return this.value; }"
                           "};"
                           "a = Box(1);"
                           "b = Box(2);"
                           "a.set(40);"
                           "result = a.get() + b.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter when constructors return explicit values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(value) { this.value = value; return 999; }"
                           "  get() { return this.value; }"
                           "};"
                           "c = Counter(42);"
                           "result = is_instance(c) * 100 + c.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 142.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for instance field slots after shape growth") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Pair {"
                           "  constructor(a) { this.a = a; }"
                           "  add_b(b) { this.b = b; }"
                           "  set_a(a) { this.a = a; }"
                           "  sum() { return this.a + this.b; }"
                           "};"
                           "p = Pair(1);"
                           "p.add_b(2);"
                           "p.set_a(40);"
                           "result = p.sum();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited field slots after child shape growth") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  constructor(value) { this.base = value; }"
                           "  get_base() { return this.base; }"
                           "};"
                           "seed = Base(1);"
                           "class Child extends Base {"
                           "  constructor(value) { this.extra = value + 1; super(value); }"
                           "  set_base(value) { this.base = value; }"
                           "  set_extra(value) { this.extra = value; }"
                           "  get_extra() { return this.extra; }"
                           "};"
                           "child = Child(20);"
                           "child.set_extra(2);"
                           "child.set_base(40);"
                           "result = child.get_base() + child.get_extra();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded OOP methods by arity") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  value() { return 1; }"
                           "  value(x) { return x + 1; }"
                           "  value(x, y) { return x + y + 1; }"
                           "  static seed() { return 1; }"
                           "  static seed(x) { return x + 1; }"
                           "};"
                           "class Child extends Base {"
                           "  value(x) { return super.value() + super.value(x) + super.value(x, 20); }"
                           "};"
                           "c = Child();"
                           "b = Base();"
                           "result = c.value(10) + b.value(10, 20) + "
                           "Base.seed() + Base.seed(8);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 84.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded OOP methods by argument type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Token {};"
                           "class Counter {"
                           "  value(x) { return 0; }"
                           "  value(x: number) { return 1; }"
                           "  value(x: string) { return 2; }"
                           "  value(x: list) { return 4; }"
                           "  value(x: map) { return 8; }"
                           "  value(x: class) { return 16; }"
                           "  value(x: function) { return 32; }"
                           "  value(x: null) { return 64; }"
                           "};"
                           "c = Counter();"
                           "fn = () => 1;"
                           "result = c.value(41) + "
                           "c.value(\"x\") + "
                           "c.value(list(1)) + "
                           "c.value(map {x: 1}) + "
                           "c.value(Token) + "
                           "c.value(fn) + "
                           "c.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 127.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded OOP methods by class type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Shape {};"
                           "class Circle extends Shape {};"
                           "class Square extends Shape {};"
                           "class Picker {"
                           "  value(x) { return 1; }"
                           "  value(x: Shape) { return 20; }"
                           "  value(x: Circle) { return 400; }"
                           "};"
                           "p = Picker();"
                           "c = Circle();"
                           "s = Square();"
                           "result = p.value(c) * 100 + p.value(s) * 10 + p.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 40201.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded OOP constructors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Token {};"
                           "class Counter {"
                           "  constructor(value) { this.value = 0; }"
                           "  constructor(value: number) { this.value = 1; }"
                           "  constructor(value: string) { this.value = 2; }"
                           "  constructor(value: list) { this.value = 4; }"
                           "  constructor(value: map) { this.value = 8; }"
                           "  constructor(value: class) { this.value = 16; }"
                           "  constructor(value: function) { this.value = 32; }"
                           "  constructor(value: null) { this.value = 64; }"
                           "  get() { return this.value; }"
                           "};"
                           "fn = () => 1;"
                           "result = Counter(41).get() + "
                           "Counter(\"x\").get() + "
                           "Counter(list(1)).get() + "
                           "Counter(map {x: 1}).get() + "
                           "Counter(Token).get() + "
                           "Counter(fn).get() + "
                           "Counter(null).get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 127.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP class aliases") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Point {"
                           "  constructor(x, y) { this.x = x; this.y = y; }"
                           "  sum() { return this.x + this.y; }"
                           "};"
                           "Alias = Point;"
                           "p = Alias(20, 1);"
                           "q = new Alias(10, 11);"
                           "result = p.sum() + q.sum() + (p instanceof Alias);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 43.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP class closures") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter_class(offset) {"
                           "  class Counter {"
                           "    constructor(base) { this.base = base; }"
                           "    value() { return this.base + offset; }"
                           "    static add(base) { return base + offset; }"
                           "  };"
                           "  return Counter;"
                           "};"
                           "CounterClass = make_counter_class(2);"
                           "counter = CounterClass(20);"
                           "result = counter.value() + CounterClass.add(18);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for stored static methods with super") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  static value(x) { return x + 1; }"
                           "};"
                           "class Child extends Base {"
                           "  static value(x) { return super.value(x) * 2; }"
                           "};"
                           "value = Child.value;"
                           "result = value(20);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded static super methods by type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Token {};"
                           "class Base {"
                           "  static value(x) { return 0; }"
                           "  static value(x: number) { return 1; }"
                           "  static value(x: string) { return 2; }"
                           "  static value(x: list) { return 4; }"
                           "  static value(x: map) { return 8; }"
                           "  static value(x: class) { return 16; }"
                           "  static value(x: function) { return 32; }"
                           "  static value(x: null) { return 64; }"
                           "};"
                           "class Child extends Base {"
                           "  static value(x) { return super.value(x); }"
                           "};"
                           "fn = () => 1;"
                           "result = Child.value(41) + "
                           "Child.value(\"x\") + "
                           "Child.value(list(1)) + "
                           "Child.value(map {x: 1}) + "
                           "Child.value(Token) + "
                           "Child.value(fn) + "
                           "Child.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 127.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited static methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  static value() { return 40; }"
                           "};"
                           "class Child extends Base {};"
                           "result = Child.value() + 2;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for stored static methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Math2 {"
                           "  static add(a, b) { return a + b; }"
                           "};"
                           "add = Math2.add;"
                           "result = add(20, 22);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for bound methods returned from functions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(value) { this.value = value; }"
                           "  add(delta) { return this.value + delta; }"
                           "};"
                           "func make_add(value) {"
                           "  counter = Counter(value);"
                           "  return counter.add;"
                           "};"
                           "add = make_add(40);"
                           "result = add(2);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for bound methods of local classes returned from functions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_add(value) {"
                           "  class Counter {"
                           "    constructor(value) { this.value = value; }"
                           "    add(delta) { return this.value + delta; }"
                           "  };"
                           "  counter = Counter(value);"
                           "  return counter.add;"
                           "};"
                           "add = make_add(40);"
                           "result = add(2);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded static methods by class type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Shape {};"
                           "class Circle extends Shape {};"
                           "class Square extends Shape {};"
                           "class Picker {"
                           "  static value(x) { return 1; }"
                           "  static value(x: Shape) { return 20; }"
                           "  static value(x: Circle) { return 400; }"
                           "};"
                           "result = Picker.value(Circle()) * 100 + "
                           "Picker.value(Square()) * 10 + Picker.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 40201.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded static methods by primitive type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Token {};"
                           "class Picker {"
                           "  static value(x) { return 0; }"
                           "  static value(x: number) { return 1; }"
                           "  static value(x: string) { return 2; }"
                           "  static value(x: list) { return 4; }"
                           "  static value(x: map) { return 8; }"
                           "  static value(x: class) { return 16; }"
                           "  static value(x: function) { return 32; }"
                           "  static value(x: null) { return 64; }"
                           "};"
                           "fn = () => 1;"
                           "result = Picker.value(41) + "
                           "Picker.value(\"x\") + "
                           "Picker.value(list(1)) + "
                           "Picker.value(map {x: 1}) + "
                           "Picker.value(Token) + "
                           "Picker.value(fn) + "
                           "Picker.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 127.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for static methods returned from closures") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_value() {"
                           "  offset = 2;"
                           "  class Counter { static value(base) { return base + offset; } };"
                           "  return Counter.value;"
                           "};"
                           "value = make_value();"
                           "result = value(40);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited super owner context") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  value() { return 1; }"
                           "};"
                           "class Middle extends Base {"
                           "  value() { return super.value() + 1; }"
                           "};"
                           "class Child extends Middle {};"
                           "child = Child();"
                           "result = child.value() * 21;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for constructors capturing enclosing variables") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter(value) {"
                           "  offset = 2;"
                           "  class Counter {"
                           "    constructor(start) { this.value = start + offset; }"
                           "    get() { return this.value; }"
                           "  };"
                           "  return Counter(value);"
                           "};"
                           "counter = make_counter(40);"
                           "result = counter.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for instance methods capturing enclosing variables") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter(value) {"
                           "  offset = 2;"
                           "  class Counter {"
                           "    constructor(value) { this.value = value; }"
                           "    get() { return this.value + offset; }"
                           "  };"
                           "  return Counter(value);"
                           "};"
                           "counter = make_counter(40);"
                           "result = counter.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited static fields") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "Base.value = 40;"
                           "class Child extends Base {};"
                           "Child.extra = 1;"
                           "result = Child.value + Child.extra + 1;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP type predicates") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  get() { return 1; }"
                           "};"
                           "c = Counter();"
                           "result = is_class(Counter) * 100 + is_instance(c) * 10 + "
                           "is_function(c.get);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP typeof comparisons") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  get() { return 1; }"
                           "};"
                           "c = Counter();"
                           "result = (typeof(Counter) == \"class\") * 1000 + "
                           "(typeof(c) == \"instance\") * 100 + "
                           "(typeof(c.get) == \"function\") * 10 + "
                           "(typeof(c) != \"class\");";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 1111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP predicates on returned values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter_class() {"
                           "  class Counter {"
                           "    get() { return 1; }"
                           "  };"
                           "  return Counter;"
                           "};"
                           "CounterClass = make_counter_class();"
                           "counter = CounterClass();"
                           "OtherClass = make_counter_class();"
                           "other = OtherClass();"
                           "result = is_class(CounterClass) * 100 + "
                           "is_instance(other) * 10 + "
                           "is_function(counter.get);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP predicates on map member values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_bundle() {"
                           "  class Counter { get() { return 1; } };"
                           "  return map{ Counter: Counter, make: func() { return Counter(); } };"
                           "};"
                           "bundle = make_bundle();"
                           "result = is_class(bundle.Counter) * 100 + "
                           "is_function(bundle.make) * 10 + "
                           "(typeof(bundle.Counter) == \"class\");";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP predicates on chain member values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Maker {"
                           "  make_class() {"
                           "    class Counter { get() { return 1; } };"
                           "    return Box(Counter);"
                           "  }"
                           "  make_instance() {"
                           "    class Counter { get() { return 1; } };"
                           "    return Box(Counter());"
                           "  }"
                           "  make_method() {"
                           "    class Counter { get() { return 1; } };"
                           "    counter = Counter();"
                           "    return Box(counter.get);"
                           "  }"
                           "};"
                           "result = is_class(Maker().make_class().value) * 100 + "
                           "is_instance(Maker().make_instance().value) * 10 + "
                           "is_function(Maker().make_method().value);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for list spread in instance method calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  value(a, b, c) { return (a == \"x\") * 100 + b.code * 10 + c; }"
                           "};"
                           "args = list(\"x\", map {code: 4}, 2);"
                           "result = Counter().value(...args);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 142.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for class aliases in extends clauses") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { value() { return 1; } };"
                           "class OtherBase { value() { return 42; } };"
                           "Base = OtherBase;"
                           "class Child extends Base {};"
                           "child = Child();"
                           "result = child.value();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super method calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  value(x) { return x + 1; }"
                           "};"
                           "class Child extends Base {"
                           "  value(x) { return super.value(x) * 2; }"
                           "};"
                           "c = Child();"
                           "result = c.value(10);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 22.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded super methods by type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Token {};"
                           "class Base {"
                           "  value(x) { return 0; }"
                           "  value(x: number) { return 1; }"
                           "  value(x: string) { return 2; }"
                           "  value(x: list) { return 4; }"
                           "  value(x: map) { return 8; }"
                           "  value(x: class) { return 16; }"
                           "  value(x: function) { return 32; }"
                           "  value(x: null) { return 64; }"
                           "};"
                           "class Child extends Base {"
                           "  value(x) { return super.value(x); }"
                           "};"
                           "c = Child();"
                           "fn = () => 1;"
                           "result = c.value(41) + "
                           "c.value(\"x\") + "
                           "c.value(list(1)) + "
                           "c.value(map {x: 1}) + "
                           "c.value(Token) + "
                           "c.value(fn) + "
                           "c.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 127.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for list spread in super method calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  value(a, b, c) { return (a == \"x\") * 100 + b.code * 10 + c; }"
                           "};"
                           "class Child extends Base {"
                           "  value(args) { return super.value(...args); }"
                           "};"
                           "result = Child().value(list(\"x\", map {code: 4}, 2));";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 142.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super instance methods stored in locals") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  value(x) { return x + 1; }"
                           "};"
                           "class Child extends Base {"
                           "  value(x) {"
                           "    parent = super.value;"
                           "    return parent(x) * 2;"
                           "  }"
                           "};"
                           "child = Child();"
                           "result = child.value(20);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super instance field reads") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Child extends Base {"
                           "  constructor(value) { super(value); this.extra = 2; }"
                           "  get() { return super.value + this.extra; }"
                           "};"
                           "result = Child(40).get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super instance field assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  constructor(value) { this.value = value; }"
                           "  get() { return this.value; }"
                           "};"
                           "class Child extends Base {"
                           "  set(value) { super.value = value; }"
                           "};"
                           "c = Child(1);"
                           "c.set(42);"
                           "result = c.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super static methods stored in locals") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  static value(x) { return x + 1; }"
                           "};"
                           "class Child extends Base {"
                           "  static value(x) {"
                           "    parent = super.value;"
                           "    return parent(x) * 2;"
                           "  }"
                           "};"
                           "result = Child.value(20);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for class call receivers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Child { value() { return 42; } };"
                           "result = Child().value();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for direct instanceof receivers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "class Child extends Base {};"
                           "Alias = Child;"
                           "BaseAlias = Base;"
                           "result = (Child() instanceof Child) * 100 + "
                           "(new Alias() instanceof BaseAlias) * 10 + "
                           "(Alias() instanceof Base);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for class aliases with instanceof") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Animal {};"
                           "class Dog extends Animal {};"
                           "AnimalAlias = Animal;"
                           "DogAlias = Dog;"
                           "dog = DogAlias();"
                           "result = (dog instanceof DogAlias) * 10 + "
                           "(dog instanceof AnimalAlias);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 11.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for parent constructors with instanceof") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Animal {"
                           "  constructor(base) { this.base = base; }"
                           "  base_score() { return this.base + 1; }"
                           "};"
                           "class Dog extends Animal {"
                           "  constructor(base, weight) { super(base); this.weight = weight; }"
                           "  score() { return this.base_score() + this.weight; }"
                           "};"
                           "dog = Dog(8, 1);"
                           "result = dog.score() + (dog instanceof Dog) * 10 + "
                           "(dog instanceof Animal) * 100;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 120.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for override and final method checks") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { final seed() { return 1; } value() { return 40; } };"
                           "class Child extends Base {"
                           "  override value() { return super.value() + 2; }"
                           "};"
                           "result = Child().value();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for typed override methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { value(x: map) { return 40; } value(x: null) { return 1; } };"
                           "class Child extends Base {"
                           "  override value(x: map) { return super.value(x) + 2; }"
                           "};"
                           "result = Child().value(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject invalid override methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "class Child extends Base { override value() { return 1; } };"
                           "result = Child().value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "override"));

      turbo_script_free(ctx);
    }

    it("should reject typed override methods without matching parent signatures in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { value(x: null) { return 1; } };"
                           "class Child extends Base { override value(x: map) { return 2; } };"
                           "result = Child().value(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "override"));

      turbo_script_free(ctx);
    }

    it("should reject extending final classes in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "final class Base {};"
                           "class Child extends Base {};"
                           "result = Child();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "final class"));

      turbo_script_free(ctx);
    }

    it("should reject overriding final methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { final value() { return 1; } };"
                           "class Child extends Base { value() { return 2; } };"
                           "result = Child().value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "final method"));

      turbo_script_free(ctx);
    }

    it("should reject overriding final typed methods with matching signatures in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { final value(x: map) { return 1; } value(x: null) { return 2; } };"
                           "class Child extends Base { value(x: map) { return 3; } };"
                           "result = Child().value(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "final method"));

      turbo_script_free(ctx);
    }

    it("should match interpreter for overloaded parent constructors through super") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  constructor() { this.value = 1; }"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Child extends Base {"
                           "  constructor(value) { super(value); }"
                           "  get() { return this.value; }"
                           "};"
                           "result = Child(42).get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded parent constructors through super by type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Token {};"
                           "class Base {"
                           "  constructor(value) { this.value = 0; }"
                           "  constructor(value: number) { this.value = 1; }"
                           "  constructor(value: string) { this.value = 2; }"
                           "  constructor(value: list) { this.value = 4; }"
                           "  constructor(value: map) { this.value = 8; }"
                           "  constructor(value: class) { this.value = 16; }"
                           "  constructor(value: function) { this.value = 32; }"
                           "  constructor(value: null) { this.value = 64; }"
                           "};"
                           "class Child extends Base {"
                           "  constructor(value) { super(value); }"
                           "  get() { return this.value; }"
                           "};"
                           "fn = () => 1;"
                           "result = Child(41).get() + "
                           "Child(\"x\").get() + "
                           "Child(list(1)).get() + "
                           "Child(map {x: 1}).get() + "
                           "Child(Token).get() + "
                           "Child(fn).get() + "
                           "Child(null).get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 127.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for list spread in constructor calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(a, b, c) {"
                           "    this.value = (a == \"x\") * 100 + b.code * 10 + c;"
                           "  }"
                           "  get() { return this.value; }"
                           "};"
                           "args = list(\"x\", map {code: 4}, 2);"
                           "result = Counter(...args).get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 142.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited default constructors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Animal {"
                           "  constructor(base, weight) { this.base = base; this.weight = weight; }"
                           "  score() { return this.base + this.weight; }"
                           "};"
                           "class Dog extends Animal {};"
                           "dog = Dog(20, 22);"
                           "result = dog.score() + (dog instanceof Animal);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 43.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for functions returning external OOP instances") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "class Child extends Base { value() { return 1; } };"
                           "func make_child() { return Child(); }"
                           "result = (make_child() instanceof Child) * 100 + "
                           "(make_child() instanceof Base) * 10 + "
                           "make_child().value();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for functions returning external class values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Child { value() { return 42; } };"
                           "func make_child_class() { return Child; }"
                           "ChildClass = make_child_class();"
                           "child = ChildClass();"
                           "result = child.value() + is_class(ChildClass);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 43.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for function-returned instance receivers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter(start) {"
                           "  class Counter {"
                           "    constructor(value) { this.value = value; }"
                           "    next() { this.value = this.value + 1; return this.value; }"
                           "  };"
                           "  return Counter(start);"
                           "};"
                           "result = make_counter(41).next();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super through inherited default constructors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Middle extends Base {};"
                           "class Child extends Middle {"
                           "  constructor(value) { super(value + 1); }"
                           "  get() { return this.value; }"
                           "};"
                           "c = Child(41);"
                           "result = c.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for chained super constructors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Middle extends Base {"
                           "  constructor(value) { super(value + 1); }"
                           "};"
                           "class Child extends Middle {"
                           "  constructor(value) { super(value + 1); }"
                           "  get() { return this.value; }"
                           "};"
                           "c = Child(40);"
                           "result = c.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for concrete subclasses of abstract classes") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Shape { abstract area(); };"
                           "class Square extends Shape {"
                           "  constructor(size) { this.size = size; }"
                           "  area() { return this.size * this.size; }"
                           "};"
                           "s = Square(6);"
                           "result = s.area() + 6;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for keyword typed abstract method implementations") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Handler {"
                           "  abstract accept(x: map);"
                           "  abstract make(x: null);"
                           "};"
                           "class Good extends Handler {"
                           "  accept(x: map) { return 40; }"
                           "  make(x: null) { return 2; }"
                           "};"
                           "g = Good();"
                           "result = g.accept(map {x: 1}) + g.make(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for classes implementing interfaces") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface HasArea { area(); };"
                           "interface HasPerimeter { perimeter(); };"
                           "class Square implements HasArea, HasPerimeter {"
                           "  constructor(size) { this.size = size; }"
                           "  area() { return this.size * this.size; }"
                           "  perimeter() { return this.size * 4; }"
                           "};"
                           "s = Square(6);"
                           "result = s.area() + s.perimeter();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 60.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for static interface method implementations") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Factory { static make(); };"
                           "class Base { static seed() { return 20; } };"
                           "class Counter extends Base implements Factory {"
                           "  static make() { return Counter.seed() + 22; }"
                           "};"
                           "result = Counter.make();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited static interface methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Factory { static make(); };"
                           "class Base { static make() { return 42; } };"
                           "class Child extends Base implements Factory {};"
                           "result = Child.make();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for classes implementing inherited interfaces") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface Colored { color(); };"
                           "interface ColoredShape extends Shape, Colored { label(); };"
                           "class Square implements ColoredShape {"
                           "  constructor(size) { this.size = size; }"
                           "  area() { return this.size * this.size; }"
                           "  color() { return 2; }"
                           "  label() { return 4; }"
                           "};"
                           "s = Square(6);"
                           "result = s.area() + s.color() + s.label();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for instanceof with implemented interfaces") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface Colored { color(); };"
                           "interface ColoredShape extends Shape, Colored { label(); };"
                           "class Square implements ColoredShape {"
                           "  area() { return 1; }"
                           "  color() { return 2; }"
                           "  label() { return 3; }"
                           "};"
                           "s = Square();"
                           "result = (s instanceof Shape) * 100 + "
                           "(s instanceof Colored) * 10 + "
                           "(s instanceof ColoredShape);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited interface instanceof") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "class Base implements Shape { area() { return 1; } };"
                           "class Child extends Base {};"
                           "c = Child();"
                           "result = (c instanceof Child) * 100 + "
                           "(c instanceof Base) * 10 + "
                           "(c instanceof Shape);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited methods satisfying interfaces") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "class Base { area() { return 42; } };"
                           "class Child extends Base implements Shape {};"
                           "c = Child();"
                           "result = c.area() + (c instanceof Shape);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 43.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for inherited typed methods satisfying interfaces") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface Handler { handle(x: Shape); };"
                           "class Square implements Shape { area() { return 1; } };"
                           "class Base { handle(x: Shape) { return 42; } };"
                           "class Child extends Base implements Handler {};"
                           "result = Child().handle(Square());";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for overloaded OOP methods by interface type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface ColoredShape extends Shape { color(); };"
                           "class Square implements ColoredShape {"
                           "  area() { return 1; }"
                           "  color() { return 2; }"
                           "};"
                           "class Triangle implements Shape { area() { return 3; } };"
                           "class Picker {"
                           "  value(x) { return 1; }"
                           "  value(x: Shape) { return 20; }"
                           "  value(x: ColoredShape) { return 400; }"
                           "};"
                           "p = Picker();"
                           "result = p.value(Square()) * 100 + "
                           "p.value(Triangle()) * 10 + p.value(null);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 40201.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for exact typed interface method implementations") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface Handler { handle(x: Shape); };"
                           "class Square implements Shape { area() { return 1; } };"
                           "class Good implements Handler {"
                           "  handle(x: Shape) { return 42; }"
                           "};"
                           "result = Good().handle(Square());";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for keyword typed interface method implementations") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Handler {"
                           "  accept_map(x: map);"
                           "  accept_class(x: class);"
                           "  accept_function(x: function);"
                           "  accept_null(x: null);"
                           "  static make_map(x: map);"
                           "};"
                           "class Good implements Handler {"
                           "  accept_map(x: map) { return 1; }"
                           "  accept_class(x: class) { return 2; }"
                           "  accept_function(x: function) { return 4; }"
                           "  accept_null(x: null) { return 8; }"
                           "  static make_map(x: map) { return 16; }"
                           "};"
                           "fn = () => 1;"
                           "g = Good();"
                           "result = g.accept_map(map {x: 1}) + "
                           "g.accept_class(Good) + "
                           "g.accept_function(fn) + "
                           "g.accept_null(null) + "
                           "Good.make_map(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 31.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for typed implementations of untyped abstract methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Base { abstract value(x); };"
                           "class Child extends Base { value(x: number) { return x + 1; } };"
                           "result = Child().value(41);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for returned interface class identity") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_bundle() {"
                           "  interface Shape { area(); };"
                           "  class Square implements Shape { area() { return 1; } };"
                           "  return map{ Shape: Shape, Square: Square };"
                           "};"
                           "bundle = make_bundle();"
                           "ShapeAlias = bundle.Shape;"
                           "SquareAlias = bundle.Square;"
                           "square = SquareAlias();"
                           "result = square instanceof ShapeAlias;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 1.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for instanceof with class value member expressions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_bundle() {"
                           "  interface Shape { area(); };"
                           "  class Square implements Shape { area() { return 1; } };"
                           "  return map{ Shape: Shape, Square: Square, nested: map{ Shape: Shape } };"
                           "};"
                           "bundle = make_bundle();"
                           "SquareAlias = bundle.Square;"
                           "square = SquareAlias();"
                           "result = (square instanceof bundle.Shape) * 10 + "
                           "(square instanceof bundle.nested.Shape);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 11.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for callable map member values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_bundle() {"
                           "  class Counter {"
                           "    constructor(value) { this.value = value; }"
                           "    get() { return this.value; }"
                           "  };"
                           "  return map{ Counter: Counter, add_one: func(x) { return x + 1; } };"
                           "};"
                           "bundle = make_bundle();"
                           "result = bundle.Counter(41).get() + bundle.add_one(0);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for returned interface overload identity") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_bundle() {"
                           "  interface Shape { area(); };"
                           "  class Square implements Shape { area() { return 1; } };"
                           "  class Picker {"
                           "    value(x) { return 1; }"
                           "    value(x: Shape) { return 42; }"
                           "  };"
                           "  return map{ Shape: Shape, Square: Square, Picker: Picker };"
                           "};"
                           "bundle = make_bundle();"
                           "SquareAlias = bundle.Square;"
                           "PickerAlias = bundle.Picker;"
                           "result = PickerAlias().value(SquareAlias());";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for same-name interface overload identity") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { global_area(); };"
                           "class Other implements Shape { global_area() { return 1; } };"
                           "func make_bundle() {"
                           "  interface Shape { local_area(); };"
                           "  class Square implements Shape { local_area() { return 1; } };"
                           "  class Picker {"
                           "    value(x) { return 1; }"
                           "    value(x: Shape) { return 42; }"
                           "  };"
                           "  return map{ Square: Square, Picker: Picker };"
                           "};"
                           "bundle = make_bundle();"
                           "SquareAlias = bundle.Square;"
                           "PickerAlias = bundle.Picker;"
                           "result = PickerAlias().value(SquareAlias()) * 100 + "
                           "PickerAlias().value(Other());";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 4201.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for typed interface requirement identity") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { global_area(); };"
                           "func make_bundle() {"
                           "  interface Shape { local_area(); };"
                           "  interface Handler { handle(x: Shape); };"
                           "  class Square implements Shape { local_area() { return 1; } };"
                           "  return map{ Handler: Handler, Shape: Shape, Square: Square };"
                           "};"
                           "bundle = make_bundle();"
                           "HandlerAlias = bundle.Handler;"
                           "LocalShape = bundle.Shape;"
                           "SquareAlias = bundle.Square;"
                           "class Good implements HandlerAlias {"
                           "  handle(x: LocalShape) { return 42; }"
                           "};"
                           "result = Good().handle(SquareAlias());";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for duplicate typed interface requirement identities") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_left() {"
                           "  interface Shape { left(); };"
                           "  interface Handler { handle(x: Shape); };"
                           "  class LeftShape implements Shape { left() { return 1; } };"
                           "  return map{ Shape: Shape, Handler: Handler, LeftShape: LeftShape };"
                           "};"
                           "func make_right() {"
                           "  interface Shape { right(); };"
                           "  interface Handler { handle(x: Shape); };"
                           "  class RightShape implements Shape { right() { return 1; } };"
                           "  return map{ Shape: Shape, Handler: Handler, RightShape: RightShape };"
                           "};"
                           "left = make_left();"
                           "right = make_right();"
                           "LeftHandler = left.Handler;"
                           "RightHandler = right.Handler;"
                           "LeftShape = left.Shape;"
                           "RightShape = right.Shape;"
                           "LeftClass = left.LeftShape;"
                           "RightClass = right.RightShape;"
                           "interface Both extends LeftHandler, RightHandler {};"
                           "class Good implements Both {"
                           "  handle(x: LeftShape) { return 20; }"
                           "  handle(x: RightShape) { return 40; }"
                           "};"
                           "result = Good().handle(LeftClass()) + Good().handle(RightClass());";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 60.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for interface aliases in implements clauses") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "Alias = Shape;"
                           "class Square implements Alias { area() { return 42; } };"
                           "s = Square();"
                           "result = s.area() + (s instanceof Alias);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 43.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for interface aliases in interface extends clauses") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "Alias = Shape;"
                           "interface LabeledShape extends Alias { label(); };"
                           "class Square implements LabeledShape {"
                           "  area() { return 40; }"
                           "  label() { return 2; }"
                           "};"
                           "s = Square();"
                           "result = s.area() + s.label() + (s instanceof Alias);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 43.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for OOP instances returned from functions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter(start) {"
                           "  class Counter {"
                           "    constructor(value) { this.value = value; }"
                           "    next() { this.value = this.value + 1; return this.value; }"
                           "  };"
                           "  return Counter(start);"
                           "};"
                           "counter = make_counter(41);"
                           "result = counter.next();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for class value variables shadowing class names") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { get() { return 1; } };"
                           "class OtherCounter { get() { return 14; } };"
                           "Counter = OtherCounter;"
                           "a = Counter();"
                           "b = new Counter();"
                           "c = OtherCounter();"
                           "result = a.get() + b.get() + (c instanceof Counter) * 14;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject class calls after the class variable is overwritten in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { get() { return 1; } };"
                           "var Counter = 2;"
                           "result = Counter().get();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);

      turbo_script_free(ctx);
    }

    it("should reject new expressions after the class variable is overwritten in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { get() { return 1; } };"
                           "var Counter = 2;"
                           "result = new Counter();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);

      turbo_script_free(ctx);
    }

    it("should match interpreter for instanceof after the class variable is overwritten") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {};"
                           "c = Counter();"
                           "var Counter = 2;"
                           "result = c instanceof Counter;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 0.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for static fields on returned class values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter_class() {"
                           "  class Base {};"
                           "  Base.value = 40;"
                           "  class Counter extends Base {};"
                           "  Counter.extra = 1;"
                           "  return Counter;"
                           "};"
                           "CounterClass = make_counter_class();"
                           "result = CounterClass.value + CounterClass.extra + 1;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for member calls returning class values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Maker {"
                           "  make() {"
                           "    class Counter {"
                           "      constructor(value) { this.value = value; }"
                           "      get() { return this.value; }"
                           "    };"
                           "    return Counter;"
                           "  }"
                           "};"
                           "CounterClass = Maker().make();"
                           "counter = CounterClass(42);"
                           "result = counter.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for member calls returning instances") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Maker {"
                           "  make(value) {"
                           "    class Counter {"
                           "      constructor(value) { this.value = value; }"
                           "      next() { this.value = this.value + 1; return this.value; }"
                           "    };"
                           "    return Counter(value);"
                           "  }"
                           "};"
                           "counter = Maker().make(41);"
                           "result = counter.next();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for member calls returning bound methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Maker {"
                           "  make(value) {"
                           "    class Counter {"
                           "      constructor(value) { this.value = value; }"
                           "      add(delta) { return this.value + delta; }"
                           "    };"
                           "    counter = Counter(value);"
                           "    return counter.add;"
                           "  }"
                           "};"
                           "add = Maker().make(40);"
                           "result = add(2);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for member access returning class values from chain receivers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Maker {"
                           "  make() {"
                           "    class Counter {"
                           "      constructor(value) { this.value = value; }"
                           "      get() { return this.value; }"
                           "    };"
                           "    box = Box(Counter);"
                           "    return box;"
                           "  }"
                           "};"
                           "CounterClass = Maker().make().value;"
                           "counter = CounterClass(42);"
                           "result = counter.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for member access returning instances from chain receivers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Maker {"
                           "  make(value) {"
                           "    class Counter {"
                           "      constructor(value) { this.value = value; }"
                           "      next() { this.value = this.value + 1; return this.value; }"
                           "    };"
                           "    box = Box(Counter(value));"
                           "    return box;"
                           "  }"
                           "};"
                           "counter = Maker().make(41).value;"
                           "result = counter.next();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for member access returning bound methods from chain receivers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Maker {"
                           "  make(value) {"
                           "    class Counter {"
                           "      constructor(value) { this.value = value; }"
                           "      add(delta) { return this.value + delta; }"
                           "    };"
                           "    counter = Counter(value);"
                           "    box = Box(counter.add);"
                           "    return box;"
                           "  }"
                           "};"
                           "add = Maker().make(40).value;"
                           "result = add(2);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for internal private member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(value) { this._value = value; }"
                           "  _add(delta) { this._value = this._value + delta; return this._value; }"
                           "  add2() { return this._add(2); }"
                           "  static init(value) { Counter._seed = value; }"
                           "  static _seed_plus(delta) { return Counter._seed + delta; }"
                           "  static seed_plus(delta) { return Counter._seed_plus(delta); }"
                           "};"
                           "Counter.init(20);"
                           "counter = Counter(20);"
                           "result = counter.add2() + Counter.seed_plus(0);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for declared field access control") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  protected value = 20;"
                           "  protected static seed = 10;"
                           "};"
                           "class Child extends Base {"
                           "  private extra = 2;"
                           "  private static bonus = 10;"
                           "  add() { this.value = this.value + this.extra; return this.value; }"
                           "  static total() { Child.bonus = Child.bonus + 0; return super.seed + Child.bonus; }"
                           "};"
                           "c = Child();"
                           "result = c.add() + Child.total();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for declared fields without initializers") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  value;"
                           "};"
                           "b = Box();"
                           "result = is_null(b.value) * 40 + ((b.value == null) * 2);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for declared private fields on returned class values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_counter() {"
                           "  class Counter {"
                           "    private value = 40;"
                           "    get() { return this.value + 2; }"
                           "  };"
                           "  return Counter;"
                           "};"
                           "CounterAlias = make_counter();"
                           "result = CounterAlias().get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for private static member access through class aliases") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  static init(value) { Alias._seed = value; }"
                           "  static get() { return Alias._seed; }"
                           "  static _plus(delta) { return Alias._seed + delta; }"
                           "  static value(delta) { return Alias._plus(delta); }"
                           "};"
                           "Alias = Counter;"
                           "Counter.init(20);"
                           "result = Counter.get() + Counter.value(2);";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for static field shadowing and aliases") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "Base.value = 1;"
                           "class Child extends Base {};"
                           "Child.value = 20;"
                           "Alias = Child;"
                           "Alias.extra = 1;"
                           "result = Child.value * 2 + Base.value + Alias.extra;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for static fields shared through class aliases") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Store {};"
                           "Alias = Store;"
                           "Alias.value = 42;"
                           "result = Store.value;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super static field reads") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "Base.value = 40;"
                           "class Child extends Base {"
                           "  static value() { return super.value + 2; }"
                           "};"
                           "result = Child.value();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for super static field assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "Base.value = 1;"
                           "class Child extends Base {"
                           "  static set(value) { super.value = value; }"
                           "};"
                           "Child.set(42);"
                           "result = Base.value;";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject missing parent static fields read through super in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "class Child extends Base {"
                           "  static value() { return super.missing; }"
                           "};"
                           "result = Child.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "static field"));

      turbo_script_free(ctx);
    }

    it("should reject direct abstract class instantiation in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Shape { abstract area(); };"
                           "shape = Shape();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject subclasses missing abstract method implementations in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Shape { abstract area(); };"
                           "class Broken extends Shape {};"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject classes missing interface method implementations in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "class Broken implements Shape {};"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject instance methods for static interface requirements in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Factory { static make(); };"
                           "class Broken implements Factory { make() { return 1; } };"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject classes missing inherited interface methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface ColoredShape extends Shape { label(); };"
                           "class Broken implements ColoredShape { label() { return 1; } };"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject classes with wrong typed interface method implementations in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { area(); };"
                           "interface ColoredShape extends Shape { color(); };"
                           "interface Handler { handle(x: Shape); };"
                           "class Broken implements Handler {"
                           "  handle(x: ColoredShape) { return 1; }"
                           "};"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject same-name wrong typed interface implementations in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "interface Shape { global_area(); };"
                           "func make_handler() {"
                           "  interface Shape { local_area(); };"
                           "  interface Handler { handle(x: Shape); };"
                           "  return Handler;"
                           "};"
                           "HandlerAlias = make_handler();"
                           "class Bad implements HandlerAlias {"
                           "  handle(x: Shape) { return 1; }"
                           "};"
                           "bad = Bad();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject incomplete duplicate typed interface requirements in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func make_left() {"
                           "  interface Shape { left(); };"
                           "  interface Handler { handle(x: Shape); };"
                           "  return map{ Shape: Shape, Handler: Handler };"
                           "};"
                           "func make_right() {"
                           "  interface Shape { right(); };"
                           "  interface Handler { handle(x: Shape); };"
                           "  return map{ Shape: Shape, Handler: Handler };"
                           "};"
                           "left = make_left();"
                           "right = make_right();"
                           "LeftHandler = left.Handler;"
                           "RightHandler = right.Handler;"
                           "LeftShape = left.Shape;"
                           "interface Both extends LeftHandler, RightHandler {};"
                           "class Bad implements Both {"
                           "  handle(x: LeftShape) { return 10; }"
                           "};"
                           "bad = Bad();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject non-interface aliases in implements clauses in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class NotInterface {};"
                           "Alias = NotInterface;"
                           "class Broken implements Alias {};"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not an interface"));

      turbo_script_free(ctx);
    }

    it("should reject abstract method implementations with the wrong arity in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Shape { abstract area(scale); };"
                           "class Broken extends Shape { area() { return 1; } };"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject wrong keyword typed abstract method implementations in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "abstract class Handler { abstract accept(x: map); };"
                           "class Broken extends Handler { accept(x: null) { return 1; } };"
                           "broken = Broken();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "abstract"));

      turbo_script_free(ctx);
    }

    it("should reject unknown parent classes in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Child extends MissingParent {};"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Undefined parent class"));

      turbo_script_free(ctx);
    }

    it("should reject overwritten class variables in extends clauses in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { value() { return 1; } };"
                           "var Base = 2;"
                           "class Child extends Base {};"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Undefined parent class"));

      turbo_script_free(ctx);
    }

    it("should reject this inside static methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { static value() { return this; } };"
                           "result = Counter.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "this"));

      turbo_script_free(ctx);
    }

    it("should reject this outside instance methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = this;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_JIT);

      turbo_script_free(ctx);
    }

    it("should reject invalid super calls in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { value() { return super.value(); } };"
                           "base = Base();"
                           "result = base.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "no parent"));

      turbo_script_free(ctx);
    }

    it("should reject super constructor calls outside constructors in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { constructor(value) { this.value = value; } };"
                           "class Child extends Base {"
                           "  value() { super(42); return this.value; }"
                           "};"
                           "child = Child();"
                           "result = child.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "constructors"));

      turbo_script_free(ctx);
    }

    it("should reject missing parent methods called through super in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {};"
                           "class Child extends Base {"
                           "  value() { return super.missing(); }"
                           "};"
                           "child = Child();"
                           "result = child.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "no method"));

      turbo_script_free(ctx);
    }

    it("should reject captured this inside static methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Maker {"
                           "  make() {"
                           "    class Counter { static value() { return this; } };"
                           "    return Counter;"
                           "  }"
                           "};"
                           "CounterClass = Maker().make();"
                           "result = CounterClass.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "this"));

      turbo_script_free(ctx);
    }

    it("should reject unknown classes in new expressions in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "missing = new MissingClass();"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Undefined class"));

      turbo_script_free(ctx);
    }

    it("should reject external private instance field access in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(value) { this._value = value; }"
                           "};"
                           "c = Counter(1);"
                           "result = c._value;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should reject external declared private instance field access in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { private value = 1; };"
                           "c = Counter();"
                           "result = c.value;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private field"));

      turbo_script_free(ctx);
    }

    it("should reject external declared protected static field access in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { protected static value = 1; };"
                           "result = Counter.value;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Protected field"));

      turbo_script_free(ctx);
    }

    it("should reject external private instance method calls in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  _value() { return 1; }"
                           "};"
                           "c = Counter();"
                           "result = c._value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should reject external private instance field assignment in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  constructor(value) { this._value = value; }"
                           "};"
                           "c = Counter(1);"
                           "c._value = 2;"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should reject external private static field access in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { static init() { Counter._value = 1; } };"
                           "Counter.init();"
                           "result = Counter._value;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should reject external private static field access through aliases in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { static init() { Counter._value = 1; } };"
                           "Alias = Counter;"
                           "Counter.init();"
                           "result = Alias._value;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should reject external private static assignment in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {};"
                           "Counter._value = 1;"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should reject non-variable member assignment receivers in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Box {"
                           "  constructor(value) { this.value = value; }"
                           "};"
                           "class Maker {"
                           "  make() { return Box(1); }"
                           "};"
                           "Maker().make().value = 42;"
                           "result = 0;";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_JIT);

      turbo_script_free(ctx);
    }

    it("should reject external private static method calls in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { static _value() { return 1; } };"
                           "result = Counter._value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "Private"));

      turbo_script_free(ctx);
    }

    it("should match interpreter for declared private methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  private value() { return 42; }"
                           "  get() { return this.value(); }"
                           "};"
                           "result = Counter().get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for private overloaded methods by keyword type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  private value(x: map) { return 40; }"
                           "  private value(x: null) { return 2; }"
                           "  get() { return this.value(map {x: 1}) + this.value(null); }"
                           "};"
                           "result = Counter().get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject external declared private methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { private value() { return 1; } };"
                           "result = Counter().value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should reject external private overloaded methods selected by keyword type in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { private value(x: map) { return 1; } };"
                           "result = Counter().value(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should match interpreter for declared protected methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { protected value() { return 40; } };"
                           "class Child extends Base { get() { return super.value() + 2; } };"
                           "result = Child().get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for protected overloaded methods by keyword type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  protected value(x: map) { return 40; }"
                           "  protected value(x: null) { return 1; }"
                           "};"
                           "class Child extends Base {"
                           "  get() { return super.value(map {x: 1}) + super.value(null) + 1; }"
                           "};"
                           "result = Child().get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject external declared protected methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { protected value() { return 1; } };"
                           "result = Base().value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should match interpreter for declared private static methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  private static value() { return 40; }"
                           "  static get() { return Counter.value() + 2; }"
                           "};"
                           "result = Counter.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for private static overloaded methods by keyword type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {"
                           "  private static value(x: map) { return 40; }"
                           "  private static value(x: null) { return 2; }"
                           "  static get() { return Counter.value(map {x: 1}) + Counter.value(null); }"
                           "};"
                           "result = Counter.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject external declared private static methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { private static value() { return 1; } };"
                           "result = Counter.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should reject external private static overloaded methods selected by keyword type in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter { private static value(x: map) { return 1; } };"
                           "result = Counter.value(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should match interpreter for declared protected static methods") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { protected static value() { return 40; } };"
                           "class Child extends Base { static get() { return super.value() + 2; } };"
                           "result = Child.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for protected static overloaded methods by keyword type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base {"
                           "  protected static value(x: map) { return 40; }"
                           "  protected static value(x: null) { return 1; }"
                           "};"
                           "class Child extends Base {"
                           "  static get() { return super.value(map {x: 1}) + super.value(null) + 1; }"
                           "};"
                           "result = Child.get();";

      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reject external declared protected static methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { protected static value() { return 1; } };"
                           "result = Base.value();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should reject external protected static overloaded methods selected by keyword type in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Base { protected static value(x: map) { return 1; } };"
                           "result = Base.value(map {x: 1});";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "not accessible"));

      turbo_script_free(ctx);
    }

    it("should reject missing instance methods in JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Counter {};"
                           "c = Counter();"
                           "result = c.missing();";

      check_int_eq(turbo_script_run_jit(ctx, script), -1);
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_RUNTIME);
      check_not_null(strstr(turbo_script_get_error(ctx), "no method"));

      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 6: Constant declaration tests ===== */

  describe("constant declaration") {

    it("should compile and run const declaration") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "const PI = 3.14159; r = PI * 2;"), 0);
      check_double_eq(ts_get_num(ctx, "r"), 6.28318, 1e-4);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 6: For-in tests ===== */

  describe("for-in loop") {

    it("should iterate over a vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "nums", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (x in nums) { sum += x; }"), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 15.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for-in results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx_interp, "v", data, 3);
      ts_bind_vec(ctx_jit, "v", data, 3);
      const char *script = "sum = 0; for (x in v) { sum += x; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "sum"), ts_get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 7: Null and member access ===== */

  describe("null literal") {

    it("should compile and return 0 for null") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = null;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use null in expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10 + null;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should preserve null type for is_null") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = is_null(null);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should compare null by value type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = (null == null) * 100 + (null != 0) * 10 + (null == 0);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 110.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for dynamic typeof and predicates") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "l = list(1, \"x\", 3);"
                           "m = map {a: 1};"
                           "b = bytes(\"Az\");"
                           "result = (typeof(\"hello\") == \"string\") * 1000 + "
                           "(typeof([1, 2]) == \"vector\") * 100 + "
                           "(typeof(m) == \"map\") * 10 + "
                           "is_list(l) + "
                           "(typeof(42) == \"int64\") + "
                           "(typeof(true) == \"bool\") + "
                           "(typeof(b) == \"bytes\") + "
                           "is_int64(42) + is_bool(true) + is_bytes(b) + "
                           "b.length() + b[0] + b[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 1306.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for native date time and duration values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "d = date.parse(\"2026-06-28\");"
          "t = time.parse(\"09:30:05.123\");"
          "dur = duration.parse(\"1h30m5s250ms\");"
          "result = (typeof(d) == \"date\") * 100000 + "
          "(typeof(t) == \"time\") * 10000 + "
          "(typeof(dur) == \"duration\") * 1000 + "
          "d.month * 100 + t.hour + (dur.seconds == 5405.25);";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 111610.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("member access") {

    it("should get vector length") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "n = v.length;"), 0);
      check_double_eq(ts_get_num(ctx, "n"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use vector length in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx, "arr", data, 3);
      check_int_eq(turbo_script_run_jit(
                       ctx, "sum = 0; for (i = 0; i < arr.length; i += 1) { sum += arr[i]; }"),
                   0);
      check_double_eq(ts_get_num(ctx, "sum"), 60.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0};
      ts_bind_vec(ctx_interp, "v", data, 4);
      ts_bind_vec(ctx_jit, "v", data, 4);
      const char *script = "n = v.length;";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "n"), ts_get_num(ctx_interp, "n"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for optional chaining") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "missing = null;"
                           "present = map {name: 42};"
                           "missing_value = missing?.name;"
                           "present_value = present?.name;"
                           "result = is_null(missing_value) * 100 + present_value;";
      int interp_res = turbo_script_run(ctx_interp, script);
      if (interp_res != 0)
        printf("Optional chaining interpreter Error: %s\n", turbo_script_get_error(ctx_interp));
      check_int_eq(interp_res, 0);
      int res = turbo_script_run_jit(ctx_jit, script);
      if (res != 0) printf("Optional chaining MIR Error: %s\n", turbo_script_get_error(ctx_jit));
      check_int_eq(res, 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 142.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map keys and values lists") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 10, b: 20};"
                           "k = m.keys();"
                           "v = m.values();"
                           "k.push(\"c\");"
                           "result = is_list(k) * 100 + k.length() * 10 + v[0] + v[1];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 160.0, EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("ternary operator") {

    it("should evaluate ternary true branch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 1 ? 42 : 0;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should evaluate ternary false branch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 0 ? 42 : 99;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 99.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use ternary in expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10; b = a > 5 ? a * 2 : a / 2;"), 0);
      check_double_eq(ts_get_num(ctx, "b"), 20.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("env-owned vector sync") {

    it("should use pre-bound vectors through MIR lowering") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0};
      ts_bind_vec(ctx, "v", data, 3);
      check_int_eq(
          turbo_script_run_jit(
              ctx, "sum = 0; for (i = 0; i < 3; i += 1) { sum += v[i]; } result = sum * 2;"),
          0);
      check_double_eq(ts_get_num(ctx, "result"), 12.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("unary plus") {

    it("should handle unary plus on variable") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5; b = +a;"), 0);
      check_double_eq(ts_get_num(ctx, "b"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle unary minus on variable") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5; b = -a;"), 0);
      check_double_eq(ts_get_num(ctx, "b"), -5.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("return statement") {

    it("should return from top-level script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; return x; x = 99;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should return with expression") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5; b = 10; return a + b;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 5.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should stop execution after return") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0;"
                                             "for (i = 0; i < 100; i += 1) {"
                                             "  sum += i;"
                                             "  if (sum > 10) { return sum; }"
                                             "}"),
                   0);
      /* sum should be 15 (0+1+2+3+4+5) when it exceeds 10 */
      check_double_eq(ts_get_num(ctx, "sum"), 15.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("switch statement") {

    it("should match first case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 1;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  case 3: { result = 30; }"
                                             "}"),
                   0);
      check_double_eq(ts_get_num(ctx, "result"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match middle case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 2;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  case 3: { result = 30; }"
                                             "}"),
                   0);
      check_double_eq(ts_get_num(ctx, "result"), 20.0, EPS);
      turbo_script_free(ctx);
    }

    it("should execute default case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 99;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  default: { result = -1; }"
                                             "}"),
                   0);
      check_double_eq(ts_get_num(ctx, "result"), -1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use switch with expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 3; b = 0;"
                                             "switch (a * 2) {"
                                             "  case 4: { b = 100; }"
                                             "  case 6: { b = 200; }"
                                             "  case 8: { b = 300; }"
                                             "}"),
                   0);
      check_double_eq(ts_get_num(ctx, "b"), 200.0, EPS);
      turbo_script_free(ctx);
    }
  }

  describe("unsupported literal nodes") {

    it("should match interpreter for vector literal assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [10, 20, 30]; result = v[0] + v[2];";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map literal assignments in arithmetic") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {x: 5, y: 10}; result = m.x + m.y;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("compile-time map key resolution") {

    it("should access pre-bound map fields by index") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Create a map in env before JIT compile */
      turbo_script_run(ctx, "config = map {width: 800, height: 600, depth: 32};");
      check_int_eq(
          turbo_script_run_jit(ctx, "result = config.width + config.height + config.depth;"), 0);
      check_double_eq(ts_get_num(ctx, "result"), 1432.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use map fields in arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_run(ctx, "pt = map {x: 3, y: 4};");
      check_int_eq(turbo_script_run_jit(ctx, "dist = (pt.x * pt.x + pt.y * pt.y) ^ 0.5;"), 0);
      check_double_eq(ts_get_num(ctx, "dist"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should use map fields in loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_run(ctx, "params = map {start: 0, stop: 100, step: 1};");
      check_int_eq(
          turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = params.start; i < params.stop; i += params.step) { sum += i; }"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for runtime map literals before member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 10, b: 20}; result = m.a + m.b;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for non-variable map member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = (map {x: 7, y: 9}).x + 1;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reuse assigned non-variable map member access results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = (map {x: 7, y: 9}).x; result = x + 35;";
      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("native vector indexing") {

    it("should read pre-bound vector elements") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {100.0, 200.0, 300.0};
      ts_bind_vec(ctx, "v", data, 3);
      check_int_eq(turbo_script_run_jit(ctx, "result = v[0] + v[1] + v[2];"), 0);
      check_double_eq(ts_get_num(ctx, "result"), 600.0, EPS);
      turbo_script_free(ctx);
    }

    it("should index with computed expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      ts_bind_vec(ctx, "arr", data, 5);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 5; i += 1) { sum += arr[i]; }"), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 150.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle multiple vectors") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double a[] = {1.0, 2.0, 3.0};
      double b[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx, "a", a, 3);
      ts_bind_vec(ctx, "b", b, 3);
      check_int_eq(
          turbo_script_run_jit(ctx, "dot = 0; for (i = 0; i < 3; i += 1) { dot += a[i] * b[i]; }"),
          0);
      check_double_eq(ts_get_num(ctx, "dot"), 140.0, EPS);
      turbo_script_free(ctx);
    }

    it("should work in nested loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Simulate a small matrix-vector multiply: y = M * x
       * M is 3x3 stored row-major in flat array, x is 3-element vector */
      double M[] = {1, 0, 0, 0, 2, 0, 0, 0, 3}; /* diagonal matrix */
      double x[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx, "M", M, 9);
      ts_bind_vec(ctx, "x", x, 3);
      check_int_eq(turbo_script_run_jit(ctx, "y0 = 0; y1 = 0; y2 = 0;"
                                             "for (j = 0; j < 3; j += 1) {"
                                             "  y0 += M[0 * 3 + j] * x[j];"
                                             "  y1 += M[1 * 3 + j] * x[j];"
                                             "  y2 += M[2 * 3 + j] * x[j];"
                                             "}"),
                   0);
      check_double_eq(ts_get_num(ctx, "y0"), 10.0, EPS);
      check_double_eq(ts_get_num(ctx, "y1"), 40.0, EPS);
      check_double_eq(ts_get_num(ctx, "y2"), 90.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 13: Direct math function imports ===== */

  describe("direct math functions") {

    it("should call sin directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sin(0);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call cos directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = cos(0);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call sqrt directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sqrt(144);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 12.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call abs directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = abs(-42);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call floor and ceil directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = floor(3.7); b = ceil(3.2);"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 3.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 4.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call log and exp directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = exp(0); y = log(1);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "y"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call tan directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = tan(0);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call max and min directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = max(10, 20); b = min(10, 20);"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 20.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call math functions in a tight loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += abs(-1); }"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 1000.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for combined math") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = sqrt(144) + floor(3.7) + ceil(2.1) + abs(-5);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "x"), ts_get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 14: Native for-in compilation ===== */

  describe("native for-in") {

    it("should iterate pre-bound vector natively") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      ts_bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (x in v) { sum += x; }"), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 150.0, EPS);
      turbo_script_free(ctx);
    }

    it("should support break in native for-in") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "v", data, 5);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (x in v) { if (x > 3) { break; } sum += x; }"),
          0);
      /* sum = 1+2+3 = 6 */
      check_double_eq(ts_get_num(ctx, "sum"), 6.0, EPS);
      turbo_script_free(ctx);
    }

    it("should support continue in native for-in") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "v", data, 5);
      check_int_eq(turbo_script_run_jit(
                       ctx, "sum = 0; for (x in v) { if (x == 3) { continue; } sum += x; }"),
                   0);
      /* sum = 1+2+4+5 = 12 */
      check_double_eq(ts_get_num(ctx, "sum"), 12.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for-in results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx_interp, "v", data, 3);
      ts_bind_vec(ctx_jit, "v", data, 3);
      const char *script = "sum = 0; for (x in v) { sum += x * 2; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "sum"), ts_get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 15: Gen reuse + fn pointer cache ===== */

  describe("gen reuse and fn cache") {

    it("should compile and exec multiple times without crash") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 1;"), 0);
      check_int_eq(turbo_script_run_jit(ctx, "x = 2;"), 0);
      check_int_eq(turbo_script_run_jit(ctx, "x = 3;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 3.0, EPS);
      turbo_script_free(ctx);
    }

    it("should exec cached fn pointer repeatedly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"), 0);
      for (int n = 0; n < 500; n++) {
        check_int_eq(turbo_script_exec_jit(ctx), 0);
      }
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should recompile and update cached fn") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 10;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "x"), 10.0, EPS);
      check_int_eq(turbo_script_compile_mir(ctx, "x = 99;"), 0);
      check_int_eq(turbo_script_exec_jit(ctx), 0);
      check_double_eq(ts_get_num(ctx, "x"), 99.0, EPS);
      turbo_script_free(ctx);
    }

    it("should return -1 when no module compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_exec_jit(ctx), -1);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 16: Condition branch optimization ===== */

  describe("optimized condition branches") {

    it("should handle all comparison operators in for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }"),
                   0);
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle GT condition in while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10; while (x > 0) { x -= 1; }"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle LE condition in do-while") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; i = 1; do { sum += i; i += 1; } while (i <= 10);"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 55.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle EQ/NE in if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 5; if (x == 5) { r = 1; } else { r = 0; }"), 0);
      check_double_eq(ts_get_num(ctx, "r"), 1.0, EPS);
      check_int_eq(turbo_script_run_jit(ctx, "x = 3; if (x != 5) { r = 1; } else { r = 0; }"), 0);
      check_double_eq(ts_get_num(ctx, "r"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle AND short-circuit in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "count = 0;"
                                             "for (i = 0; i < 20; i += 1) {"
                                             "  if (i > 5 && i < 15) { count += 1; }"
                                             "}"),
                   0);
      /* i in [6..14] = 9 values */
      check_double_eq(ts_get_num(ctx, "count"), 9.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle OR short-circuit in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "count = 0;"
                                             "for (i = 0; i < 20; i += 1) {"
                                             "  if (i < 3 || i > 17) { count += 1; }"
                                             "}"),
                   0);
      /* i in [0,1,2,18,19] = 5 values */
      check_double_eq(ts_get_num(ctx, "count"), 5.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle NOT in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 0; if (!x) { r = 1; } else { r = 0; }"), 0);
      check_double_eq(ts_get_num(ctx, "r"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for complex conditions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "count = 0;"
                           "for (i = 0; i < 100; i += 1) {"
                           "  if (i >= 10 && i <= 90 && i != 50) { count += 1; }"
                           "}";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "count"), ts_get_num(ctx_interp, "count"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 17: Direct native function dispatch ===== */

  describe("direct native dispatch") {

    it("should call builtin sqrt directly (no bridge lookup)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = sqrt(256);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 16.0, EPS);
      turbo_script_free(ctx);
    }

    it("should call builtin functions in loop via direct dispatch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 1; i <= 100; i += 1) { sum += abs(-1); }"),
          0);
      check_double_eq(ts_get_num(ctx, "sum"), 100.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for mixed function calls") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0;"
                           "for (i = 0; i < 50; i += 1) {"
                           "  sum += sqrt(i * i) + floor(i + 0.5) + ceil(i - 0.5);"
                           "}";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "sum"), ts_get_num(ctx_interp, "sum"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should resolve env-registered native functions directly") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* sin/cos are registered as builtins — test they resolve correctly */
      const char *script = "x = sin(0) + cos(0) + max(3, 7) + min(3, 7);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "x"), ts_get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 18: Compile cache ===== */

  describe("compile cache") {

    it("should cache and reuse compiled script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }";
      /* First call: compiles */
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);
      /* Second call: cache hit, no recompile */
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 4950.0, EPS);
      turbo_script_free(ctx);
    }

    it("should handle different scripts correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 10;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 10.0, EPS);
      check_int_eq(turbo_script_run_jit(ctx, "x = 99;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 99.0, EPS);
      /* Re-run first script from cache */
      check_int_eq(turbo_script_run_jit(ctx, "x = 10;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 10.0, EPS);
      turbo_script_free(ctx);
    }

    it("should run cached script 1000 times") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = 42;";
      for (int n = 0; n < 1000; n++) {
        check_int_eq(turbo_script_run_jit(ctx, script), 0);
      }
      check_double_eq(ts_get_num(ctx, "x"), 42.0, EPS);
      turbo_script_free(ctx);
    }

    it("should produce correct results with pre-bound variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_num(ctx, "rate", 0.05);
      const char *script = "result = rate * 1000;";
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(ts_get_num(ctx, "result"), 50.0, EPS);
      /* Change input, re-run from cache */
      ts_bind_num(ctx, "rate", 0.10);
      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_double_eq(ts_get_num(ctx, "result"), 100.0, EPS);
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 21: Constant folding ===== */

  describe("constant folding") {

    it("should fold simple arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = 2 * 3.14159;"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 6.28318, 1e-4);
      turbo_script_free(ctx);
    }

    it("should fold nested constant expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "x = (2 + 3) * (10 - 4);"), 0);
      check_double_eq(ts_get_num(ctx, "x"), 30.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold power and modulo") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 2 ^ 10; b = 10 % 3;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1024.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 1.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold comparisons") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 5 > 3; b = 2 == 2; c = 1 != 1;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold unary operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = -5; b = !0; c = !1;"), 0);
      check_double_eq(ts_get_num(ctx, "a"), -5.0, EPS);
      check_double_eq(ts_get_num(ctx, "b"), 1.0, EPS);
      check_double_eq(ts_get_num(ctx, "c"), 0.0, EPS);
      turbo_script_free(ctx);
    }

    it("should not fold expressions with variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run_jit(ctx, "a = 10; b = a * 2 + 3;"), 0);
      check_double_eq(ts_get_num(ctx, "b"), 23.0, EPS);
      turbo_script_free(ctx);
    }

    it("should fold constants used in loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* The constant 2 * 3.14159 should be folded, loop runs normally */
      check_int_eq(
          turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += 2 * 3; }"), 0);
      check_double_eq(ts_get_num(ctx, "sum"), 600.0, EPS);
      turbo_script_free(ctx);
    }

    it("should match interpreter for folded expressions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = (100 / 4 + 3 * 2) ^ 2 - 10 % 3;";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check_double_eq(ts_get_num(ctx_jit, "x"), ts_get_num(ctx_interp, "x"), EPS);
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("async await syntax") {
    it("should match interpreter for awaited plain values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = await 42;";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for async functions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "async func add_one(x) { return x + 1; }"
                           "result = await add_one(41);";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for awaited coroutine-style maps") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "packet = map {status: \"suspended\", value: 41};"
                           "result = (await packet) + 1;";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }
}
