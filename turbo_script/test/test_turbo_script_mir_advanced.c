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


spec("turbo_script_mir_advanced") {
  describe("constant declaration") {

    it("should compile and run const declaration") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "const PI = 3.14159; r = PI * 2;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(6.28318)) <= (double)(1e-4));
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 6: For-in tests ===== */

  describe("for-in loop") {

    it("should iterate over a vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "nums", data, 5);
      check((turbo_script_run_jit(ctx, "sum = 0; for (x in nums) { sum += x; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(15.0)) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "sum")) - (double)(ts_get_num(ctx_interp, "sum"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 7: Null and member access ===== */

  describe("null literal") {

    it("should compile and return 0 for null") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = null;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should use null in expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 10 + null;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should preserve null type for is_null") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = is_null(null);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should compare null by value type") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = (null == null) * 100 + (null != 0) * 10 + (null == 0);";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(110.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(1306.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(111610.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("member access") {

    it("should get vector length") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "v", data, 5);
      check((turbo_script_run_jit(ctx, "n = v.length;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(5.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should use vector length in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx, "arr", data, 3);
      check((turbo_script_run_jit(
                       ctx, "sum = 0; for (i = 0; i < arr.length; i += 1) { sum += arr[i]; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(60.0)) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "n")) - (double)(ts_get_num(ctx_interp, "n"))) <= (double)(EPS));
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
      check((interp_res) == (0));
      int res = turbo_script_run_jit(ctx_jit, script);
      if (res != 0) printf("Optional chaining MIR Error: %s\n", turbo_script_get_error(ctx_jit));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(142.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(160.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("ternary operator") {

    it("should evaluate ternary true branch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 1 ? 42 : 0;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should evaluate ternary false branch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 0 ? 42 : 99;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(99.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should use ternary in expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 10; b = a > 5 ? a * 2 : a / 2;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(20.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("env-owned vector sync") {

    it("should use pre-bound vectors through MIR lowering") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0};
      ts_bind_vec(ctx, "v", data, 3);
      check((turbo_script_run_jit(
              ctx, "sum = 0; for (i = 0; i < 3; i += 1) { sum += v[i]; } result = sum * 2;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(12.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("unary plus") {

    it("should handle unary plus on variable") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 5; b = +a;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(5.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle unary minus on variable") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 5; b = -a;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(-5.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("return statement") {

    it("should return from top-level script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 10; return x; x = 99;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should return with expression") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 5; b = 10; return a + b;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(5.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should stop execution after return") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0;"
                                             "for (i = 0; i < 100; i += 1) {"
                                             "  sum += i;"
                                             "  if (sum > 10) { return sum; }"
                                             "}")) == (0));
      /* sum should be 15 (0+1+2+3+4+5) when it exceeds 10 */
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(15.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("switch statement") {

    it("should match first case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 1;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  case 3: { result = 30; }"
                                             "}")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should match middle case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 2;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  case 3: { result = 30; }"
                                             "}")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(20.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should execute default case") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 99;"
                                             "switch (x) {"
                                             "  case 1: { result = 10; }"
                                             "  case 2: { result = 20; }"
                                             "  default: { result = -1; }"
                                             "}")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(-1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should use switch with expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 3; b = 0;"
                                             "switch (a * 2) {"
                                             "  case 4: { b = 100; }"
                                             "  case 6: { b = 200; }"
                                             "  case 8: { b = 300; }"
                                             "}")) == (0));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(200.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("unsupported literal nodes") {

    it("should match interpreter for vector literal assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [10, 20, 30]; result = v[0] + v[2];";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map literal assignments in arithmetic") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {x: 5, y: 10}; result = m.x + m.y;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("compile-time map key resolution") {

    it("should access pre-bound map fields by index") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* Create a map in env before JIT compile */
      turbo_script_run(ctx, "config = map {width: 800, height: 600, depth: 32};");
      check((turbo_script_run_jit(ctx, "result = config.width + config.height + config.depth;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(1432.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should use map fields in arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_run(ctx, "pt = map {x: 3, y: 4};");
      check((turbo_script_run_jit(ctx, "dist = (pt.x * pt.x + pt.y * pt.y) ^ 0.5;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "dist")) - (double)(5.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should use map fields in loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_run(ctx, "params = map {start: 0, stop: 100, step: 1};");
      check((turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = params.start; i < params.stop; i += params.step) { sum += i; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should match interpreter for runtime map literals before member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 10, b: 20}; result = m.a + m.b;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for non-variable map member access") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = (map {x: 7, y: 9}).x + 1;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should reuse assigned non-variable map member access results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = (map {x: 7, y: 9}).x; result = x + 35;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("native vector indexing") {

    it("should read pre-bound vector elements") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {100.0, 200.0, 300.0};
      ts_bind_vec(ctx, "v", data, 3);
      check((turbo_script_run_jit(ctx, "result = v[0] + v[1] + v[2];")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(600.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should index with computed expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {10.0, 20.0, 30.0, 40.0, 50.0};
      ts_bind_vec(ctx, "arr", data, 5);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 5; i += 1) { sum += arr[i]; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(150.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle multiple vectors") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double a[] = {1.0, 2.0, 3.0};
      double b[] = {10.0, 20.0, 30.0};
      ts_bind_vec(ctx, "a", a, 3);
      ts_bind_vec(ctx, "b", b, 3);
      check((turbo_script_run_jit(ctx, "dot = 0; for (i = 0; i < 3; i += 1) { dot += a[i] * b[i]; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "dot")) - (double)(140.0)) <= (double)(EPS));
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
      check((turbo_script_run_jit(ctx, "y0 = 0; y1 = 0; y2 = 0;"
                                             "for (j = 0; j < 3; j += 1) {"
                                             "  y0 += M[0 * 3 + j] * x[j];"
                                             "  y1 += M[1 * 3 + j] * x[j];"
                                             "  y2 += M[2 * 3 + j] * x[j];"
                                             "}")) == (0));
      check(fabs((double)(ts_get_num(ctx, "y0")) - (double)(10.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "y1")) - (double)(40.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "y2")) - (double)(90.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 13: Direct math function imports ===== */

  describe("direct math functions") {

    it("should call sin directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = sin(0);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call cos directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = cos(0);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call sqrt directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = sqrt(144);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(12.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call abs directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = abs(-42);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call floor and ceil directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = floor(3.7); b = ceil(3.2);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(3.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(4.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call log and exp directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = exp(0); y = log(1);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "y")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call tan directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = tan(0);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call max and min directly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = max(10, 20); b = min(10, 20);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(20.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call math functions in a tight loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += abs(-1); }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(1000.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should match interpreter for combined math") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = sqrt(144) + floor(3.7) + ceil(2.1) + abs(-5);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check(fabs((double)(ts_get_num(ctx_jit, "x")) - (double)(ts_get_num(ctx_interp, "x"))) <= (double)(EPS));
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
      check((turbo_script_run_jit(ctx, "sum = 0; for (x in v) { sum += x; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(150.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should support break in native for-in") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "v", data, 5);
      check((turbo_script_run_jit(ctx, "sum = 0; for (x in v) { if (x > 3) { break; } sum += x; }")) == (0));
      /* sum = 1+2+3 = 6 */
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(6.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should support continue in native for-in") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "v", data, 5);
      check((turbo_script_run_jit(
                       ctx, "sum = 0; for (x in v) { if (x == 3) { continue; } sum += x; }")) == (0));
      /* sum = 1+2+4+5 = 12 */
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(12.0)) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "sum")) - (double)(ts_get_num(ctx_interp, "sum"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 15: Gen reuse + fn pointer cache ===== */

  describe("gen reuse and fn cache") {

    it("should compile and exec multiple times without crash") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 1;")) == (0));
      check((turbo_script_run_jit(ctx, "x = 2;")) == (0));
      check((turbo_script_run_jit(ctx, "x = 3;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(3.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should exec cached fn pointer repeatedly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }")) == (0));
      for (int n = 0; n < 500; n++) {
        check((turbo_script_exec_jit(ctx)) == (0));
      }
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should recompile and update cached fn") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 10;")) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(10.0)) <= (double)(EPS));
      check((turbo_script_compile_mir(ctx, "x = 99;")) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(99.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should return -1 when no module compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_exec_jit(ctx)) == (-1));
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 16: Condition branch optimization ===== */

  describe("optimized condition branches") {

    it("should handle all comparison operators in for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle GT condition in while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 10; while (x > 0) { x -= 1; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle LE condition in do-while") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; i = 1; do { sum += i; i += 1; } while (i <= 10);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(55.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle EQ/NE in if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 5; if (x == 5) { r = 1; } else { r = 0; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(1.0)) <= (double)(EPS));
      check((turbo_script_run_jit(ctx, "x = 3; if (x != 5) { r = 1; } else { r = 0; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle AND short-circuit in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "count = 0;"
                                             "for (i = 0; i < 20; i += 1) {"
                                             "  if (i > 5 && i < 15) { count += 1; }"
                                             "}")) == (0));
      /* i in [6..14] = 9 values */
      check(fabs((double)(ts_get_num(ctx, "count")) - (double)(9.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle OR short-circuit in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "count = 0;"
                                             "for (i = 0; i < 20; i += 1) {"
                                             "  if (i < 3 || i > 17) { count += 1; }"
                                             "}")) == (0));
      /* i in [0,1,2,18,19] = 5 values */
      check(fabs((double)(ts_get_num(ctx, "count")) - (double)(5.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle NOT in condition") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 0; if (!x) { r = 1; } else { r = 0; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(1.0)) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "count")) - (double)(ts_get_num(ctx_interp, "count"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 17: Direct native function dispatch ===== */

  describe("direct native dispatch") {

    it("should call builtin sqrt directly (no bridge lookup)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = sqrt(256);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(16.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call builtin functions in loop via direct dispatch") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 1; i <= 100; i += 1) { sum += abs(-1); }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(100.0)) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "sum")) - (double)(ts_get_num(ctx_interp, "sum"))) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "x")) - (double)(ts_get_num(ctx_interp, "x"))) <= (double)(EPS));
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
      check((turbo_script_run_jit(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));
      /* Second call: cache hit, no recompile */
      check((turbo_script_run_jit(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle different scripts correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 10;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(10.0)) <= (double)(EPS));
      check((turbo_script_run_jit(ctx, "x = 99;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(99.0)) <= (double)(EPS));
      /* Re-run first script from cache */
      check((turbo_script_run_jit(ctx, "x = 10;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should run cached script 1000 times") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = 42;";
      for (int n = 0; n < 1000; n++) {
        check((turbo_script_run_jit(ctx, script)) == (0));
      }
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should produce correct results with pre-bound variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_num(ctx, "rate", 0.05);
      const char *script = "result = rate * 1000;";
      check((turbo_script_run_jit(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(50.0)) <= (double)(EPS));
      /* Change input, re-run from cache */
      ts_bind_num(ctx, "rate", 0.10);
      check((turbo_script_run_jit(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(100.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 21: Constant folding ===== */

  describe("constant folding") {

    it("should fold simple arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 2 * 3.14159;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(6.28318)) <= (double)(1e-4));
      turbo_script_free(ctx);
    }

    it("should fold nested constant expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = (2 + 3) * (10 - 4);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(30.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should fold power and modulo") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 2 ^ 10; b = 10 % 3;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1024.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should fold comparisons") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 5 > 3; b = 2 == 2; c = 1 != 1;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should fold unary operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = -5; b = !0; c = !1;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(-5.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should not fold expressions with variables") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 10; b = a * 2 + 3;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(23.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should fold constants used in loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* The constant 2 * 3.14159 should be folded, loop runs normally */
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += 2 * 3; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(600.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should match interpreter for folded expressions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = (100 / 4 + 3 * 2) ^ 2 - 10 % 3;";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check(fabs((double)(ts_get_num(ctx_jit, "x")) - (double)(ts_get_num(ctx_interp, "x"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  describe("async await syntax") {
    it("should match interpreter for awaited plain values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = await 42;";

      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for async functions") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "async func add_one(x) { return x + 1; }"
                           "result = await add_one(41);";

      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for awaited coroutine-style maps") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "packet = map {status: \"suspended\", value: 41};"
                           "result = (await packet) + 1;";

      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }
}
 