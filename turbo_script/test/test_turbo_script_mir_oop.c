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


spec("turbo_script_mir_oop") {
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

    it("should assign to typed bool fields from to_bool") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Result {"
                           "  value: bool;"
                           "  get() { return this.value; }"
                           "};"
                           "result = Result();"
                           "result.value = to_bool(\"YES\");"
                           "flag = result.get();";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_float_eq(ts_get_num(ctx_interp, "flag"), 1.0, EPS);
      check_float_eq(ts_get_num(ctx_jit, "flag"), 1.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

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

    it("should preserve typed class field values through JIT assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "class Result {"
                           "  text: string;"
                           "  flag: bool;"
                           "  count: int64;"
                           "  details: map;"
                           "};"
                           "payload = Result();"
                           "payload.text = \"ok\";"
                           "payload.flag = true;"
                           "payload.count = 7;"
                           "payload.details = map { value: 42 };"
                           "text_value = payload.text;"
                           "result = payload.flag * 100 + payload.count + payload.details.value;";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_str_eq(ts_get_str(ctx_jit, "text_value"), ts_get_str(ctx_interp, "text_value"));
      check_str_eq(ts_get_str(ctx_jit, "text_value"), "ok");
      check_double_eq(ts_get_num(ctx_jit, "result"), ts_get_num(ctx_interp, "result"), EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 149.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should preserve values returned by dynamically imported OS functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "import(\"os\");"
                           "class Result { platform: string; };"
                           "payload = Result();"
                           "payload.platform = os.platform_name();"
                           "result = payload.platform;";

      check_int_eq(turbo_script_run_jit(ctx, script), 0);
      check_int_eq((int)(strlen(ts_get_str(ctx, "result")) > 0), 1);

      turbo_script_free(ctx);
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

    it("should retain observer identity in instance-owned lists") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "interface Observer { update(value); };"
          "class Subject {"
          "  observers = list();"
          "  constructor() { this.observers = list(); }"
          "  attach(observer: Observer) {"
          "    var updated = this.observers;"
          "    updated.push(observer);"
          "    this.observers = updated;"
          "  }"
          "  notify(value) {"
          "    for (var i = 0; i < this.observers.length(); i += 1) {"
          "      this.observers[i].update(value);"
          "    }"
          "  }"
          "};"
          "class Counter implements Observer {"
          "  value = 0;"
          "  update(value) { this.value = value + 1; }"
          "};"
          "subject = Subject();"
          "counter = Counter();"
          "subject.attach(counter);"
          "subject.notify(41);"
          "result = counter.value;";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_interp, "result"), 42.0, EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should preserve instance arguments stored by constructors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "class Counter {"
          "  value = 0;"
          "  update(value) { this.value = value + 1; }"
          "};"
          "class Command {"
          "  observer = null;"
          "  constructor(observer) { this.observer = observer; }"
          "  execute(value) { this.observer.update(value); }"
          "};"
          "counter = Counter();"
          "command = Command(counter);"
          "command.execute(41);"
          "result = counter.value;";

      check_int_eq(turbo_script_run(ctx_interp, script), 0);
      check_int_eq(turbo_script_run_jit(ctx_jit, script), 0);
      check_double_eq(ts_get_num(ctx_interp, "result"), 42.0, EPS);
      check_double_eq(ts_get_num(ctx_jit, "result"), 42.0, EPS);

      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
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

}
