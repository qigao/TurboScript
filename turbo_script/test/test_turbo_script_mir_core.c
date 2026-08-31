#include "../src/turbo_script_internal.h"
#include "tinytest.h"
#include "turbo_script.h"
#include "../src/host/turbo_script_host_api_internal.h"
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

static exprtk_value_t legacy_native_conflict(size_t arg_count,
                                             exprtk_value_t *args,
                                             exprtk_env_t *env,
                                             void *user_data) {
  (void)arg_count;
  (void)args;
  (void)env;
  if (user_data) (*(size_t *)user_data)++;
  return exprtk_val_num(7.0);
}

static turbo_script_status_t legacy_host_conflict(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder) {
  turbo_script_value_view_t value = {.kind = TURBO_SCRIPT_VALUE_NUMBER};
  (void)args;
  (void)arg_count;
  if (user_data) (*(size_t *)user_data)++;
  value.as.number = 90.0;
  return turbo_script_host_result_set_value(builder, &value);
}


spec("turbo_script_mir_core") {
  describe("compile_mir") {

    it("should compile a simple assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);
      check((turbo_script_compile_mir(ctx, "x = 42;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile arithmetic expressions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "a = 10; b = 20; c = a + b;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile all binary operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(
                       ctx, "a = 10; b = 3; c = a + b; d = a - b; e = a * b; f = a / b;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile compound assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 10; x += 5; x -= 2; x *= 3; x /= 2;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile comparison operators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(
                       ctx, "a = 10; b = 20; c = a < b; d = a > b; e = a == b; f = a != b;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile a while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "i = 0; while (i < 10) { i += 1; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile a for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile an if statement") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 10; if (x > 5) { y = 1; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 3; if (x > 5) { y = 1; } else { y = 0; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile logical AND/OR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "a = 1; b = 0; c = a && b; d = a || b;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile unary NOT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "a = 1; b = !a;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile modulo") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "a = 10; b = 3; c = a % b;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile power") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "a = 2; b = a ^ 10;")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile do-while") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "i = 0; do { i += 1; } while (i < 10);")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile break/continue") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 20; i += 1) { if (i == 15) { "
                                        "break; } if (i % 2 == 0) { continue; } sum += i; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should compile function calls") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = sin(3.14);")) == (0));
      turbo_script_free(ctx);
    }

    it("matches interpreter assignment capture only when an outer binding exists") {
      static const char *const local_script =
          "func next(){x=x+1;return x;};first=next();second=next();";
      static const char *const outer_script =
          "x=0;func next(){x=x+1;return x;};first=next();second=next();";
      for (int with_outer = 0; with_outer <= 1; ++with_outer) {
        const char *script = with_outer ? outer_script : local_script;
        turbo_script_ctx_t *tree = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        turbo_script_ctx_t *interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        turbo_script_ctx_t *jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        check_equal(turbo_script_run(tree, script), 0);
        check_equal(turbo_script_run_mir_interp(interp, script), 0);
        check_equal(turbo_script_run_jit(jit, script), 0);
        check_equal(ts_get_num(interp, "first"), ts_get_num(tree, "first"));
        check_equal(ts_get_num(interp, "second"), ts_get_num(tree, "second"));
        check_equal(ts_get_num(jit, "first"), ts_get_num(tree, "first"));
        check_equal(ts_get_num(jit, "second"), ts_get_num(tree, "second"));
        check_equal(ts_get_num(tree, "first"), 1.0);
        check_equal(ts_get_num(tree, "second"), with_outer ? 2.0 : 1.0);
        turbo_script_free(jit);
        turbo_script_free(interp);
        turbo_script_free(tree);
      }
    }

    it("keeps legacy callable precedence independent from host slot reorder") {
      static const char *const host_names[] = {
          "legacy_conflict", "abs", "script_conflict", "host_filler"};
      for (int use_interp = 0; use_interp <= 1; ++use_interp) {
        size_t native_calls = 0;
        size_t host_calls = 0;
        turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        turbo_script_result_t *result = NULL;
        turbo_script_host_function_descriptor_t descriptor = {
            .struct_size = sizeof(descriptor), .min_arity = 1, .max_arity = 1};
        const char *script =
            "script_conflict=(a)=>a+2;"
            "native_result=legacy_conflict(5);"
            "builtin_result=abs(-4);"
            "script_result=script_conflict(5);";
        check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
        ts_bind_func(ctx, "legacy_conflict", legacy_native_conflict, &native_calls);
        for (size_t i = 0; i < sizeof(host_names) / sizeof(host_names[0]); ++i) {
          descriptor.name = (turbo_script_string_view_t){host_names[i], strlen(host_names[i])};
          check_equal(turbo_script_context_register_host_function(
                          ctx, &descriptor, legacy_host_conflict, &host_calls, result),
                      TURBO_SCRIPT_STATUS_OK);
        }
        check_equal(use_interp ? turbo_script_compile_mir_interp(ctx, script)
                               : turbo_script_compile_mir(ctx, script),
                    0);
        check_equal(ctx->active_host_modules, (size_t)0);
        descriptor.name = (turbo_script_string_view_t){host_names[0], strlen(host_names[0])};
        check_equal(turbo_script_context_unregister_host_function(
                        ctx, descriptor.name, result),
                    TURBO_SCRIPT_STATUS_OK);
        check_equal(turbo_script_context_register_host_function(
                        ctx, &descriptor, legacy_host_conflict, &host_calls, result),
                    TURBO_SCRIPT_STATUS_OK);
        check_equal(use_interp ? turbo_script_exec_mir_interp(ctx)
                               : turbo_script_exec_jit(ctx),
                    0);
        check_equal(ts_get_num(ctx, "native_result"), 7.0);
        check_equal(ts_get_num(ctx, "builtin_result"), 4.0);
        check_equal(ts_get_num(ctx, "script_result"), 7.0);
        check_equal(native_calls, (size_t)1);
        check_equal(host_calls, (size_t)0);
        turbo_script_result_destroy(result);
        turbo_script_free(ctx);
      }
    }

    it("should return -1 on NULL ctx") {
      check((turbo_script_compile_mir(NULL, "x = 1;")) == (-1));
    }

    it("should return -1 on NULL script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, NULL)) == (-1));
      check(((int)turbo_script_get_error_code(ctx)) == ((int)TURBO_SCRIPT_ERROR_ARGUMENT));
      turbo_script_free(ctx);
    }

    it("does not freeze or reuse host slots in legacy lowering") {
      for (int use_interp = 0; use_interp <= 1; ++use_interp) {
        turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
        size_t host_calls = 0;
        turbo_script_result_t *result = NULL;
        turbo_script_host_function_descriptor_t descriptor = {
            .struct_size = sizeof(descriptor),
            .name = {"legacy_host_only", sizeof("legacy_host_only") - 1u},
            .min_arity = 1,
            .max_arity = 1};

        check_not_null(ctx);
        check_equal(turbo_script_result_create(ctx, &result), TURBO_SCRIPT_STATUS_OK);
        check_equal(turbo_script_context_register_host_function(
                        ctx, &descriptor, legacy_host_conflict, &host_calls, result),
                    TURBO_SCRIPT_STATUS_OK);
        check_equal(use_interp
                        ? turbo_script_compile_mir_interp(
                              ctx, "legacy_host_only_result=legacy_host_only(5);")
                        : turbo_script_compile_mir(
                              ctx, "legacy_host_only_result=legacy_host_only(5);"),
                    0);
        check_equal(ctx->active_host_modules, (size_t)0);

        check_equal(turbo_script_context_unregister_host_function(
                        ctx, descriptor.name, result),
                    TURBO_SCRIPT_STATUS_OK);
        descriptor.name = (turbo_script_string_view_t){
            "legacy_replacement", sizeof("legacy_replacement") - 1u};
        check_equal(turbo_script_context_register_host_function(
                        ctx, &descriptor, legacy_host_conflict, &host_calls, result),
                    TURBO_SCRIPT_STATUS_OK);
        check_equal(use_interp ? turbo_script_exec_mir_interp(ctx)
                               : turbo_script_exec_jit(ctx),
                    -1);
        check_equal(host_calls, (size_t)0);

        turbo_script_result_destroy(result);
        turbo_script_free(ctx);
      }
    }

    it("should return -1 on invalid syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "??? +++")) == (-1));
      check(((int)turbo_script_get_error_code(ctx)) == ((int)TURBO_SCRIPT_ERROR_PARSE));
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

      check((turbo_script_compile_mir(ctx, script)) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      check_null(strstr(turbo_script_get_error(ctx), "variable limit exceeded"));
      check(fabs((double)(ts_get_num(ctx, "v0")) - (double)(0.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "v64")) - (double)(64.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "v128")) - (double)(128.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("run_jit") {

    it("should run a simple assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 42;")) == (0));
      turbo_script_free(ctx);
    }

    it("should run arithmetic") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 10.5; b = 20.3; c = (a * b) + (a / b) - (a + b);")) == (0));
      turbo_script_free(ctx);
    }

    it("should run a while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; i = 0; while (i < 100) { sum += i; i += 1; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should run a for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 1000; i += 1) { sum += i; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should run nested for loops") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { for (j = 0; j < 10; j += 1) { sum += 1; } }")) == (0));
      turbo_script_free(ctx);
    }

    it("should run if-else") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 10; if (x > 5) { y = 1; } else { y = 0; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should run compound assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 100; x += 50; x -= 20; x *= 2; x /= 4;")) == (0));
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") { check((turbo_script_run_jit(NULL, "x = 1;")) == (-1)); }

    it("should return -1 on NULL script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, NULL)) == (-1));
      check(((int)turbo_script_get_error_code(ctx)) == ((int)TURBO_SCRIPT_ERROR_ARGUMENT));
      turbo_script_free(ctx);
    }

    it("should return -1 on invalid syntax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "??? +++")) == (-1));
      turbo_script_free(ctx);
    }
  }

  describe("exec_jit") {

    it("should execute a pre-compiled module") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 42;")) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      turbo_script_free(ctx);
    }

    it("should execute repeatedly without crash") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }")) == (0));
      for (int n = 0; n < 100; n++) {
        check((turbo_script_exec_jit(ctx)) == (0));
      }
      turbo_script_free(ctx);
    }

    it("should return -1 on NULL ctx") { check((turbo_script_exec_jit(NULL)) == (-1)); }

    it("should return -1 when no module compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_exec_jit(ctx)) == (-1));
      check(((int)turbo_script_get_error_code(ctx)) == ((int)TURBO_SCRIPT_ERROR_STATE));
      turbo_script_free(ctx);
    }
  }

  describe("mir interpreter") {
    it("should compile and execute a MIR interpreted module") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check((turbo_script_compile_mir_interp(ctx, "x = 41; x += 1;")) == (0));
      check((turbo_script_exec_mir_interp(ctx)) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(42.0)) <= (double)(EPS));

      turbo_script_free(ctx);
    }

    it("should run a script through the MIR interpreter") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check((turbo_script_run_mir_interp(
                       ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));

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
      check((turbo_script_run(ctx_run, script)) == (0));
      check((turbo_script_run_mir_interp(ctx_mir, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_mir, "sum")) - (double)(ts_get_num(ctx_run, "sum"))) <= (double)(EPS));

      turbo_script_free(ctx_run);
      turbo_script_free(ctx_mir);
    }

    it("should return -1 when no MIR interpreted module is compiled") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check((turbo_script_exec_mir_interp(ctx)) == (-1));

      turbo_script_free(ctx);
    }
  }

  describe("compile then exec separation") {

    it("should compile once and exec many times") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 1; x += 1;")) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      turbo_script_free(ctx);
    }

    it("should handle recompilation") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_compile_mir(ctx, "x = 1;")) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      check((turbo_script_compile_mir(ctx, "y = 2;")) == (0));
      check((turbo_script_exec_jit(ctx)) == (0));
      turbo_script_free(ctx);
    }

    it("run_jit should equal compile + exec") {
      turbo_script_ctx_t *ctx1 = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx2 = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "a = 5; b = 10; c = a + b;";
      check((turbo_script_run_jit(ctx1, script)) == (0));
      check((turbo_script_compile_mir(ctx2, script)) == (0));
      check((turbo_script_exec_jit(ctx2)) == (0));
      turbo_script_free(ctx1);
      turbo_script_free(ctx2);
    }
  }

  describe("for loop correctness") {

    it("should handle for loop with large iteration count") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100000; i += 1) { sum += i; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should handle for loop with step > 1") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 10; i += 2) { sum += i; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should handle for loop with zero iterations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 10; i < 0; i += 1) { sum += i; }")) == (0));
      turbo_script_free(ctx);
    }

    it("should handle nested for + if") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "count = 0;"
                           "for (i = 0; i < 20; i += 1) {"
                           "  if (i > 10) { count += 1; }"
                           "}";
      check((turbo_script_run_jit(ctx, script)) == (0));
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 1: New operator + control flow tests ===== */

  describe("logical operators") {

    it("should compute AND correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 1 && 1; b = 1 && 0; c = 0 && 1; d = 0 && 0;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(0.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(0.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "d")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should compute OR correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 1 || 1; b = 1 || 0; c = 0 || 1; d = 0 || 0;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "d")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should compute NOT correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = !0; b = !1; c = !5;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(0.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should short-circuit AND") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* If AND short-circuits, the right side (x += 1) should not execute when left is 0 */
      check((turbo_script_run_jit(ctx, "x = 10; y = 0 && (x = 99); r = x;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should short-circuit OR") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      /* If OR short-circuits, the right side should not execute when left is truthy */
      check((turbo_script_run_jit(ctx, "x = 10; y = 1 || (x = 99); r = x;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("modulo and power") {

    it("should compute modulo correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 10 % 3; b = 7 % 2; c = 15 % 5;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(1.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should compute power correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 2 ^ 10; b = 3 ^ 3; c = 10 ^ 0;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1024.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(27.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("do-while loop") {

    it("should execute body at least once") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 0; do { x += 1; } while (x < 0);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should loop correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; i = 1; do { sum += i; i += 1; } while (i <= 10);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(55.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  describe("break and continue") {

    it("should break out of for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(
              ctx, "sum = 0; for (i = 0; i < 100; i += 1) { if (i == 5) { break; } sum += i; }")) == (0));
      /* sum = 0+1+2+3+4 = 10 */
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should continue in for loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { if (i % 2 == 0) { continue; } sum += i; }")) == (0));
      /* sum = 1+3+5+7+9 = 25 */
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(25.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should break out of while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "i = 0; while (i < 100) { if (i == 3) { break; } i += 1; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "i")) - (double)(3.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should break out of do-while loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(
                       ctx, "i = 0; do { if (i == 3) { break; } i += 1; } while (i < 100);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "i")) - (double)(3.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }
  }

  /* ===== Phase 3: Variable bridge tests ===== */

  describe("variable bridge") {

    it("should read variables back via ts_get_num after run_jit") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 42;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should compute and store arithmetic results") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "a = 10; b = 20; c = a + b;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(10.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(20.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(30.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should compute for loop sum correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 100; i += 1) { sum += i; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(4950.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should read pre-set variables inside JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_num(ctx, "input", 100.0);
      check((turbo_script_run_jit(ctx, "result = input * 2;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(200.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle compound assignments correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 100; x += 50; x -= 20; x *= 2; x /= 4;")) == (0));
      /* (100+50-20)*2/4 = 130*2/4 = 65 */
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(65.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle if-else variable assignment") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = 10; if (x > 5) { y = 1; } else { y = 0; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "y")) - (double)(1.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should handle nested loops with variable bridge") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(
              ctx,
              "sum = 0; for (i = 0; i < 10; i += 1) { for (j = 0; j < 10; j += 1) { sum += 1; } }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(100.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should match interpreter results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; for (i = 1; i <= 50; i += 1) { sum += i * i; }";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check(fabs((double)(ts_get_num(ctx_jit, "sum")) - (double)(ts_get_num(ctx_interp, "sum"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 4: Function call bridge tests ===== */

  describe("function call bridge") {

    it("should call sin from JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = sin(0);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(0.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call abs from JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = abs(-42);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call max with 2 args") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = max(10, 20);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(20.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call min with 2 args") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "x = min(10, 20);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "x")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should call functions in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 10; i += 1) { sum += abs(-1); }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(10.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should match interpreter function call results") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = sqrt(144) + floor(3.7) + ceil(2.1);";
      turbo_script_run(ctx_interp, script);
      turbo_script_run_jit(ctx_jit, script);
      check(fabs((double)(ts_get_num(ctx_jit, "x")) - (double)(ts_get_num(ctx_interp, "x"))) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(12.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(5503.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(20.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(30.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(171.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(86.5)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(24.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(144.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "mv")) - (double)(ts_get_num(ctx_interp, "mv"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "mx")) - (double)(ts_get_num(ctx_interp, "mx"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "cv0")) - (double)(ts_get_num(ctx_interp, "cv0"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "q0")) - (double)(ts_get_num(ctx_interp, "q0"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "r3")) - (double)(ts_get_num(ctx_interp, "r3"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "p0")) - (double)(ts_get_num(ctx_interp, "p0"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "x0")) - (double)(ts_get_num(ctx_interp, "x0"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "x1")) - (double)(ts_get_num(ctx_interp, "x1"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "sv0")) - (double)(ts_get_num(ctx_interp, "sv0"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "sv1")) - (double)(ts_get_num(ctx_interp, "sv1"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "nc")) - (double)(ts_get_num(ctx_interp, "nc"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "tdf")) - (double)(ts_get_num(ctx_interp, "tdf"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(5701.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(2117.5)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(40.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

  }

  describe("unsupported nodes") {

    it("should match interpreter for string literal assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = \"text\"; result = x.length;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(strcmp((ts_get_str(ctx_jit, "x")), (ts_get_str(ctx_interp, "x"))) == 0);
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for template string assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "name = \"Ada\"; x = `Hi ${name} ${40 + 2}`; result = x.length;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(strcmp((ts_get_str(ctx_jit, "x")), (ts_get_str(ctx_interp, "x"))) == 0);
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for dynamic variable value copies") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = \"text\"; y = x; result = y.length;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(strcmp((ts_get_str(ctx_jit, "y")), (ts_get_str(ctx_interp, "y"))) == 0);
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map literal assignments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 2, b: 3}; result = m.a * 10 + m.b;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for array destructuring with rest") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "[a, null, ...rest] = [10, 20, 30, 40];"
                           "result = a + rest[0] + rest[1];";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(80.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for map destructuring with rest") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "m = map {a: 1, b: 2, c: 3};"
                           "map {a, ...rest} = m;"
                           "result = a * 10 + rest.b + rest.c;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(15.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for function expressions with closures") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "base = 40; add = (x) => base + x; result = add(2);";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for arrow functions as first-class values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func apply(f, x) { return f(x); }"
                           "block = (x) => { y = x * x; return y + 1; };"
                           "result = apply(x => x * 3, 7) + block(5);";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(47.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for function calls with value arguments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func add([a, b]) { a + b }; result = add([20, 22]);";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
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
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for destructured arrow parameters") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "combine = ([a, b], map {x: y}) => a + b + y;"
                           "result = combine([10, 20], map {x: 12});";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for spread call arguments") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func sum(a, b, c) { a + b + c }; args = [10, 20, 12]; result = sum(...args);";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for try catch throw") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = try { throw 40; } catch(e) { e + 2; }; check = result;";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "check")) - (double)(ts_get_num(ctx_interp, "check"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "check")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for structured thrown values") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "msg = try { throw \"bad\"; } catch(e) { typeof(e); };"
                           "result = try { throw map {code: 40}; } catch(err) { err.code + 2; };";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(strcmp((ts_get_str(ctx_jit, "msg")), (ts_get_str(ctx_interp, "msg"))) == 0);
      check(strcmp((ts_get_str(ctx_jit, "msg")), ("string")) == 0);
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(42.0)) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for caught runtime errors") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "missing = try { no_such_fn(1); } catch(e) { typeof(e); };"
                           "member = try { x = 1; x.length; } catch(e) { typeof(e); };"
                           "result = (missing == \"string\") * 10 + (member == \"string\");";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(11.0)) <= (double)(EPS));
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
      check((turbo_script_run_jit(ctx, "a = v[0]; b = v[2]; c = v[4];")) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(10.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(30.0)) <= (double)(EPS));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(50.0)) <= (double)(EPS));
      turbo_script_free(ctx);
    }

    it("should access vector in a loop") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      ts_bind_vec(ctx, "arr", data, 5);
      check((turbo_script_run_jit(ctx, "sum = 0; for (i = 0; i < 5; i += 1) { sum += arr[i]; }")) == (0));
      check(fabs((double)(ts_get_num(ctx, "sum")) - (double)(15.0)) <= (double)(EPS));
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
      check(fabs((double)(ts_get_num(ctx_jit, "sum")) - (double)(ts_get_num(ctx_interp, "sum"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }

    it("should match interpreter for non-variable array indexing") {
      turbo_script_ctx_t *ctx_interp = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_ctx_t *ctx_jit = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "result = ([10, 20, 30])[1];";
      check((turbo_script_run(ctx_interp, script)) == (0));
      check((turbo_script_run_jit(ctx_jit, script)) == (0));
      check(fabs((double)(ts_get_num(ctx_jit, "result")) - (double)(ts_get_num(ctx_interp, "result"))) <= (double)(EPS));
      turbo_script_free(ctx_interp);
      turbo_script_free(ctx_jit);
    }
  }

  /* ===== Phase 6: User-defined function tests ===== */

}
