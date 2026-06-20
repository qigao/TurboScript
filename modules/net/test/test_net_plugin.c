/**
 * @file test_net_plugin.c
 * @brief DLL integration tests for net plugin loading/registration.
 */
#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"

#include <string.h>

#ifdef _WIN32
  #define NET_PLUGIN_DLL "net.dll"
#else
  #define NET_PLUGIN_DLL "libnet_plugin.so"
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
      check_str_eq(h->plugin->name, "net");

      exprtk_env_t env;
      mem_pool_t scratch;
      exprtk_env_init(&env);
      mem_init(&scratch, 4096);

      check_int_eq(ts_plugin_init(h, &env, &scratch), 0);
      check_not_null(h->instance);
      check_not_null(find_native(&env, "http.get"));
      check_not_null(find_native(&env, "http.post"));
      check_not_null(find_native(&env, "ws.connect"));
      check_not_null(find_native(&env, "ws.send"));
      check_not_null(find_native(&env, "ws.recv"));
      check_not_null(find_native(&env, "ws.close"));

      ts_plugin_unload(h);
      exprtk_env_free(&env);
      mem_destroy(&scratch);
    }
  }
}
