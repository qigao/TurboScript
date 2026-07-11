#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"

#include <string.h>

#ifdef _WIN32
  #define RULES_FORGE_PLUGIN_DLL "tbs_rules_forge.dll"
#else
  #define RULES_FORGE_PLUGIN_DLL "tbs_rules_forge.so"
#endif

static exprtk_func_t *find_native(exprtk_env_t *env, const char *name) {
    exprtk_func_t *fn = env->funcs;
    while (fn) {
        if (!fn->is_script && strcmp(fn->name, name) == 0) return fn;
        fn = fn->next;
    }
    return NULL;
}

static exprtk_value_t make_string(exprtk_env_t *env, const char *s) {
    size_t len = strlen(s);
    char *buf = mem_alloc(&env->arena, len + 1);
    memcpy(buf, s, len + 1);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static exprtk_value_t call_native(exprtk_env_t *env, const char *name,
                                  size_t argc, exprtk_value_t *args) {
    exprtk_func_t *fn = find_native(env, name);
    if (!fn) return exprtk_val_num(-999.0);
    return fn->data.native.fn(argc, args, fn->data.native.user_data);
}

spec("rules_forge_plugin") {
    describe("dll") {
        it("should load and expose RulesForge functions") {
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            check_not_null(h);
            check_not_null(h->plugin);
            check_str_eq(h->plugin->name, "rules_forge");

            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_env_init(&env);
            mem_init(&scratch, 4096);

            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);
            check_not_null(h->instance);
            check_not_null(find_native(&env, "rules_forge.version"));
            check_not_null(find_native(&env, "rules_forge.kb_create"));
            check_not_null(find_native(&env, "rules_forge.session_create"));
            check_not_null(find_native(&env, "rules_forge.session_add_fact_json_schema"));

            exprtk_value_t version = call_native(&env, "rules_forge.version", 0, NULL);
            check_int_eq(version.type, EXPRTK_VAL_STRING);
            check(strlen(version.data.string.data) > 0);

            exprtk_value_t kb = call_native(&env, "rules_forge.kb_create", 0, NULL);
            check_int_eq(kb.type, EXPRTK_VAL_NUMBER);
            check(kb.data.number >= 0.0);

            exprtk_value_t destroy_args[1] = {kb};
            exprtk_value_t destroyed = call_native(&env, "rules_forge.kb_destroy", 1, destroy_args);
            check_float_eq(destroyed.data.number, 0.0, 0.001);

            ts_plugin_unload(h);
            exprtk_env_free(&env);
            mem_destroy(&scratch);
        }

        it("should report errors without crashing") {
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

            exprtk_value_t args[2] = {exprtk_val_num(99), make_string(&env, "x")};
            exprtk_value_t result = call_native(&env, "rules_forge.kb_load", 2, args);
            check_float_eq(result.data.number, 2.0, 0.001);

            exprtk_value_t error = call_native(&env, "rules_forge.error", 0, NULL);
            check_int_eq(error.type, EXPRTK_VAL_STRING);
            check(strlen(error.data.string.data) > 0);

            ts_plugin_unload(h);
            exprtk_env_free(&env);
            mem_destroy(&scratch);
        }
    }
}
