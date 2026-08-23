/**
 * @file test_net_plugin.c
 * @brief DLL integration tests for net plugin loading/registration.
 */
#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"

#include <string.h>

#ifdef _WIN32
  #define NET_PLUGIN_DLL "tbs_net.dll"
#else
  #define NET_PLUGIN_DLL "tbs_net.so"
#endif

static exprtk_func_t *find_native(exprtk_env_t *env, const char *name) {
  exprtk_func_t *fn = env->funcs;
  while (fn) {
    if (!fn->is_script && strcmp(fn->name, name) == 0)
      return fn;
    fn = fn->next;
  }
  return NULL;
}

spec("net_plugin") {
  describe("dll") {
    it("should load and init net plugin via DLL") {
      ts_plugin_handle_t *h = ts_plugin_load(NET_PLUGIN_DLL);
      check_not_null(h);
      check_not_null(h->plugin);
      check(strcmp((h->plugin->name), ("net")) == 0);

      exprtk_env_t env;
      mem_pool_t scratch;
      exprtk_env_init(&env);
      mem_init(&scratch, 4096);

      check((ts_plugin_init(h, &env, &scratch)) == (0));
      check_not_null(h->instance);
      exprtk_func_t *fn_http_get = find_native(&env, "http.get");
      exprtk_func_t *fn_http_post = find_native(&env, "http.post");
      exprtk_func_t *fn_ws_connect = find_native(&env, "ws.connect");
      exprtk_func_t *fn_ws_send = find_native(&env, "ws.send");
      exprtk_func_t *fn_ws_consume = find_native(&env, "ws.consume");
      exprtk_func_t *fn_ws_close = find_native(&env, "ws.close");

      check_not_null(fn_http_get);
      check_not_null(fn_http_post);
      check_not_null(fn_ws_connect);
      check_not_null(fn_ws_send);
      check_not_null(fn_ws_consume);
      check_null(find_native(&env, "ws.recv"));
      check_not_null(fn_ws_close);

      /* Test http.get with invalid URL (no options) -> returns 0/null */
      exprtk_value_t args[3];
      args[0] = (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = { "http://invalid.url.local", 24 }};
      exprtk_value_t res =
          fn_http_get->data.native.fn(1, args, &env, fn_http_get->data.native.user_data);
      check((res.type) == (EXPRTK_VAL_NUMBER));
      check(res.data.number == 0.0);

      /* Test http.get with invalid URL (with options) -> returns map with error */
      args[1] = exprtk_val_map();
      exprtk_map_set(&args[1], "timeout", (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 100});
      res = fn_http_get->data.native.fn(2, args, &env, fn_http_get->data.native.user_data);
      check((res.type) == (EXPRTK_VAL_MAP));
      exprtk_value_t err = exprtk_map_get(&res, "error");
      check((err.type) == (EXPRTK_VAL_STRING));
      check(err.data.string.len > 0);
      check(!(err.data.string.len == strlen("invalid HTTP request options") &&
              memcmp(err.data.string.data, "invalid HTTP request options",
                     err.data.string.len) == 0));
      exprtk_map_free(&res);
      exprtk_map_free(&args[1]);

      /* Invalid facade transport names fail before network I/O. */
      args[1] = exprtk_val_map();
      exprtk_map_set(&args[1], "transport",
                     (exprtk_value_t){EXPRTK_VAL_STRING,
                                      .data.string = {"invalid", 7}});
      res = fn_http_get->data.native.fn(2, args, &env,
                                        fn_http_get->data.native.user_data);
      check((res.type) == (EXPRTK_VAL_MAP));
      err = exprtk_map_get(&res, "error");
      check((err.type) == (EXPRTK_VAL_STRING));
      check(err.data.string.len > 0);
      exprtk_map_free(&res);
      exprtk_map_free(&args[1]);

      /* Test ws.connect with invalid URL -> returns 0/null */
      args[0] = (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = { "ws://invalid.url.local", 22 }};
      res = fn_ws_connect->data.native.fn(1, args, &env,
                                          fn_ws_connect->data.native.user_data);
      check((res.type) == (EXPRTK_VAL_NUMBER));
      check(res.data.number == 0);

      /* Test http.get with a real mock URL: https://mockhttp.org/ */
      args[0] = (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = { "https://mockhttp.org/", 21 }};
      args[1] = exprtk_val_map();
      exprtk_map_set(&args[1], "timeout", (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 5000});
      exprtk_map_set(&args[1], "transport",
                     (exprtk_value_t){EXPRTK_VAL_STRING,
                                      .data.string = {"auto", 4}});
      res = fn_http_get->data.native.fn(2, args, &env, fn_http_get->data.native.user_data);
      check((res.type) == (EXPRTK_VAL_MAP));
      
      exprtk_value_t status = exprtk_map_get(&res, "status");
      check((status.type) == (EXPRTK_VAL_NUMBER));
      /* mockhttp.org should return 200 OK */
      check(status.data.number == 200);
      
      exprtk_map_free(&res);
      exprtk_map_free(&args[1]);

      ts_plugin_unload(h);
      exprtk_env_free(&env);
      mem_destroy(&scratch);
    }
  }
}
