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
            check_not_null(find_native(&env, "rules_forge.session_add_fact_json_path_schema"));
            check_not_null(find_native(&env, "rules_forge.session_add_facts_json_path_schema"));
            check_not_null(find_native(&env, "rules_forge.session_add_facts_csv_path_schema"));
            check_not_null(find_native(&env, "rules_forge.stream_json_create"));
            check_not_null(find_native(&env, "rules_forge.stream_finish"));
            check_not_null(find_native(&env, "rules_forge.continuous_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_json_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_json_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_csv_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_xml_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_metrics"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_json_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_json_path_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_csv_path_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_xml_path_create"));
            check_null(find_native(&env, "rules_forge.kb_load_ts_plugin"));
            check_null(find_native(&env, "rules_forge.session_add_fact_json"));
            check_null(find_native(&env, "rules_forge.session_add_facts_csv"));

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

        it("should reject continuous path ingestion for an invalid session") {
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t push_args[8];
            exprtk_value_t stream_args[7];
            exprtk_value_t push_result;
            exprtk_value_t stream_result;

            check_not_null(h);
            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

            push_args[0] = exprtk_val_int(99);
            push_args[1] = make_string(&env, "schema.tbe");
            push_args[2] = make_string(&env, "Event");
            push_args[3] = make_string(&env, "{\"events\":[]}");
            push_args[4] = make_string(&env, "$.events[*]");
            push_args[5] = make_string(&env, "event_id");
            push_args[6] = make_string(&env, "event_time");
            push_args[7] = make_string(&env, "events");
            push_result = call_native(&env, "rules_forge.continuous_push_json_path_schema",
                                      8, push_args);

            stream_args[0] = push_args[0];
            stream_args[1] = push_args[1];
            stream_args[2] = push_args[2];
            stream_args[3] = push_args[4];
            stream_args[4] = push_args[5];
            stream_args[5] = push_args[6];
            stream_args[6] = push_args[7];
            stream_result = call_native(&env,
                "rules_forge.continuous_stream_json_path_create", 7, stream_args);

            check_int_eq(exprtk_map_get(&push_result, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&push_result, "result").data.integer, -1);
            check_float_eq(stream_result.data.number, -1.0, 0.001);

            ts_plugin_unload(h);
            exprtk_env_free(&env);
            mem_destroy(&scratch);
        }

        it("should preserve path-schema error result shapes") {
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t json_args[5];
            exprtk_value_t csv_args[5];
            exprtk_value_t one;
            exprtk_value_t many;
            exprtk_value_t csv;

            check_not_null(h);
            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

            json_args[0] = exprtk_val_int(99);
            json_args[1] = make_string(&env, "schema.tbe");
            json_args[2] = make_string(&env, "Customer");
            json_args[3] = make_string(&env, "{\"customers\":[]}");
            json_args[4] = make_string(&env, "$.customers[*]");
            one = call_native(&env, "rules_forge.session_add_fact_json_path_schema",
                              5, json_args);
            many = call_native(&env, "rules_forge.session_add_facts_json_path_schema",
                               5, json_args);

            memcpy(csv_args, json_args, sizeof(csv_args));
            csv_args[3] = make_string(&env, "name_s,age_n\nAlice,30\n");
            csv_args[4] = make_string(&env, "age > 18");
            csv = call_native(&env, "rules_forge.session_add_facts_csv_path_schema",
                              5, csv_args);

            check_float_eq(one.data.number, -1.0, 0.001);
            check_int_eq(exprtk_map_get(&many, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&many, "loaded").data.integer, 0);
            check_int_eq(exprtk_map_get(&csv, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&csv, "loaded").data.integer, 0);

            ts_plugin_unload(h);
            exprtk_env_free(&env);
            mem_destroy(&scratch);
        }

        it("should create and destroy a continuous session with default limits") {
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t kb;
            exprtk_value_t continuous;
            exprtk_value_t args[1];

            check_not_null(h);
            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

            kb = call_native(&env, "rules_forge.kb_create", 0, NULL);
            args[0] = kb;
            continuous = call_native(&env, "rules_forge.continuous_create", 1, args);
            check_int_eq(continuous.type, EXPRTK_VAL_INTEGER);
            check(continuous.data.integer >= 0);

            args[0] = continuous;
            check_float_eq(call_native(&env, "rules_forge.continuous_destroy", 1, args).data.number,
                           0.0, 0.001);
            args[0] = kb;
            check_float_eq(call_native(&env, "rules_forge.kb_destroy", 1, args).data.number,
                           0.0, 0.001);

            ts_plugin_unload(h);
            exprtk_env_free(&env);
            mem_destroy(&scratch);
        }

        it("should reject feed on an invalid stream handle") {
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[2];
            exprtk_value_t result;

            check_not_null(h);
            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

            args[0] = exprtk_val_int(99);
            args[1] = make_string(&env, "{}");
            result = call_native(&env, "rules_forge.stream_feed", 2, args);
            check_float_eq(result.data.number, 2.0, 0.001);

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
