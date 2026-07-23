/**
 * @file test_wasm.c
 * @brief Tests for wasm module — wasm.* functions via DLL plugin.
 */

#include "exprtk.h"
#include "fib32.wasm.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WASM_PLUGIN_DLL "tbs_wasm.dll"
#else
  #define WASM_PLUGIN_DLL "tbs_wasm.so"
#endif

typedef struct {
  ts_plugin_handle_t *wasm_plugin;
  exprtk_env_t env;
  mem_pool_t scratch;
} test_env_t;

static void test_env_init(test_env_t *t) {
  memset(t, 0, sizeof(*t));
  exprtk_env_init(&t->env);
  mem_init(&t->scratch, 4096);
  t->wasm_plugin = ts_plugin_load(WASM_PLUGIN_DLL);
  if (t->wasm_plugin)
    ts_plugin_init(t->wasm_plugin, &t->env, &t->scratch);
}

static void test_env_free(test_env_t *t) {
  ts_plugin_unload(t->wasm_plugin);
  exprtk_env_free(&t->env);
  mem_destroy(&t->scratch);
}

static exprtk_value_t call_fn(test_env_t *t, const char *name, size_t argc, exprtk_value_t *args) {
  exprtk_func_t *fn = t->env.funcs;
  while (fn) {
    if (strcmp(fn->name, name) == 0 && !fn->is_script) {
      return fn->data.native.fn(argc, args, &t->env, fn->data.native.user_data);
    }
    fn = fn->next;
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -999.0};
}

static exprtk_value_t make_str(test_env_t *t, const char *s) {
  size_t len = strlen(s);
  char *buf = (char *)mem_alloc(&t->env.arena, len + 1);
  memcpy(buf, s, len + 1);
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t make_num(double v) {
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = v};
}

static int write_fib32_wasm(const char *path) {
  FILE *fp = fopen(path, "wb");
  size_t wr;
  if (!fp)
    return -1;
  wr = fwrite(fib32_wasm, 1, fib32_wasm_len, fp);
  fclose(fp);
  return (wr == fib32_wasm_len) ? 0 : -1;
}

static int write_add2_wasm(const char *path) {
  /* Minimal module exporting: (func (export "add") (param i32 i32) (result i32) ... ) */
  static const unsigned char add2_wasm[] = {
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, /* header */
      0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, /* type */
      0x03, 0x02, 0x01, 0x00,                                     /* function */
      0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,       /* export */
      0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b /* code */
  };
  FILE *fp = fopen(path, "wb");
  size_t wr;
  if (!fp)
    return -1;
  wr = fwrite(add2_wasm, 1, sizeof(add2_wasm), fp);
  fclose(fp);
  return (wr == sizeof(add2_wasm)) ? 0 : -1;
}

spec("wasm_module") {

  describe("plugin") {
    it("should load and unload wasm_plugin via DLL") {
      ts_plugin_handle_t *h = ts_plugin_load(WASM_PLUGIN_DLL);
      check_not_null(h);
      check_not_null(h->plugin);
      check_str_eq(h->plugin->name, "wasm");

      exprtk_env_t env;
      mem_pool_t scratch;
      exprtk_env_init(&env);
      mem_init(&scratch, 4096);
      check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

      ts_plugin_unload(h);
      exprtk_env_free(&env);
      mem_destroy(&scratch);
    }
  }

  describe("wasm.open / wasm.call / wasm.close") {
    it("should execute fib(20)=6765 from wasm module") {
      const char *tmp_wasm = "test_fib32.wasm";
      test_env_t t;
      exprtk_value_t args_open[1];
      exprtk_value_t h;
      exprtk_value_t args_call[3];
      exprtk_value_t out;
      exprtk_value_t args_close[1];

      check_int_eq(write_fib32_wasm(tmp_wasm), 0);

      test_env_init(&t);
      check_not_null(t.wasm_plugin);

      args_open[0] = make_str(&t, tmp_wasm);
      h = call_fn(&t, "wasm.open", 1, args_open);
      check_int_eq(h.type, EXPRTK_VAL_NUMBER);
      check(h.data.number >= 0.0);

      args_call[0] = h;
      args_call[1] = make_str(&t, "fib");
      args_call[2] = make_num(20.0);
      out = call_fn(&t, "wasm.call", 3, args_call);
      check_int_eq(out.type, EXPRTK_VAL_NUMBER);
      check_float_eq(out.data.number, 6765.0, 0.01);

      args_close[0] = h;
      call_fn(&t, "wasm.close", 1, args_close);

      test_env_free(&t);
      remove(tmp_wasm);
    }

    it("should report errors for invalid call and invalid handle") {
      const char *tmp_wasm = "test_fib32.wasm";
      test_env_t t;
      exprtk_value_t args_open[1];
      exprtk_value_t h;
      exprtk_value_t args_bad_call[3];
      exprtk_value_t bad_res;
      exprtk_value_t err_handle_args[1];
      exprtk_value_t err_handle;
      exprtk_value_t err_ctx;
      exprtk_value_t args_bad_handle_call[3];
      exprtk_value_t args_close[1];

      check_int_eq(write_fib32_wasm(tmp_wasm), 0);

      test_env_init(&t);
      check_not_null(t.wasm_plugin);

      args_open[0] = make_str(&t, tmp_wasm);
      h = call_fn(&t, "wasm.open", 1, args_open);
      check(h.data.number >= 0.0);

      /* Invalid function name -> handle-level error */
      args_bad_call[0] = h;
      args_bad_call[1] = make_str(&t, "fib_not_found");
      args_bad_call[2] = make_num(20.0);
      bad_res = call_fn(&t, "wasm.call", 3, args_bad_call);
      check_int_eq(bad_res.type, EXPRTK_VAL_NUMBER);
      check_float_eq(bad_res.data.number, 0.0, 0.01);

      /* Plugin sets env->aborted on error; clear so we can continue testing */
      t.env.aborted = 0;
      err_handle_args[0] = h;
      err_handle = call_fn(&t, "wasm.last_error", 1, err_handle_args);
      check_int_eq(err_handle.type, EXPRTK_VAL_STRING);
      check(err_handle.data.string.len > 0);
      {
        char *s = tstr_v_to_cstr(err_handle.data.string);
        check_not_null(s);
        check_str_contains(s, "function");
        free(s);
      }

      /* Invalid handle -> context-level error */
      args_bad_handle_call[0] = make_num(999.0);
      args_bad_handle_call[1] = make_str(&t, "fib");
      args_bad_handle_call[2] = make_num(20.0);
      (void)call_fn(&t, "wasm.call", 3, args_bad_handle_call);

      t.env.aborted = 0;
      err_ctx = call_fn(&t, "wasm.last_error", 0, NULL);
      check_int_eq(err_ctx.type, EXPRTK_VAL_STRING);
      check(err_ctx.data.string.len > 0);
      {
        char *s = tstr_v_to_cstr(err_ctx.data.string);
        check_not_null(s);
        check_str_contains(s, "invalid handle");
        free(s);
      }

      args_close[0] = h;
      call_fn(&t, "wasm.close", 1, args_close);

      test_env_free(&t);
      remove(tmp_wasm);
    }

    it("should pass multiple numeric args to wasm function") {
      const char *tmp_wasm = "test_add2.wasm";
      test_env_t t;
      exprtk_value_t args_open[1];
      exprtk_value_t h;
      exprtk_value_t args_call[4];
      exprtk_value_t out;
      exprtk_value_t args_close[1];

      check_int_eq(write_add2_wasm(tmp_wasm), 0);

      test_env_init(&t);
      check_not_null(t.wasm_plugin);

      args_open[0] = make_str(&t, tmp_wasm);
      h = call_fn(&t, "wasm.open", 1, args_open);
      check_int_eq(h.type, EXPRTK_VAL_NUMBER);
      check(h.data.number >= 0.0);

      args_call[0] = h;
      args_call[1] = make_str(&t, "add");
      args_call[2] = make_num(20.0);
      args_call[3] = make_num(22.0);
      out = call_fn(&t, "wasm.call", 4, args_call);
      check_int_eq(out.type, EXPRTK_VAL_NUMBER);
      check_float_eq(out.data.number, 42.0, 0.01);

      args_close[0] = h;
      call_fn(&t, "wasm.close", 1, args_close);

      test_env_free(&t);
      remove(tmp_wasm);
    }
  }
}
