#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"

#include <stdio.h>
#include <stdlib.h>
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

static char *make_rule_path(const char *path) {
    size_t i;
    size_t len = strlen(path);
    char *result = (char *)malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, path, len + 1);
    for (i = 0; i < len; i++) {
        if (result[i] == '\\') result[i] = '/';
    }
    return result;
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
            check_not_null(find_native(&env, "rules_forge.session_add_fact_yaml_schema"));
            check_not_null(find_native(&env, "rules_forge.session_add_fact_yaml_path_schema"));
            check_not_null(find_native(&env, "rules_forge.session_add_facts_yaml_path_schema"));
            check_not_null(find_native(&env, "rules_forge.session_add_facts_csv_path_schema"));
            check_not_null(find_native(&env, "rules_forge.stream_json_create"));
            check_not_null(find_native(&env, "rules_forge.stream_yaml_create"));
            check_not_null(find_native(&env, "rules_forge.stream_yaml_all_create"));
            check_not_null(find_native(&env, "rules_forge.stream_yaml_path_create"));
            check_not_null(find_native(&env, "rules_forge.stream_yaml_path_all_create"));
            check_not_null(find_native(&env, "rules_forge.stream_finish"));
            check_not_null(find_native(&env, "rules_forge.continuous_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_json_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_json_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_yaml_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_yaml_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_csv_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_push_xml_path_schema"));
            check_not_null(find_native(&env, "rules_forge.continuous_metrics"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_json_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_json_path_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_yaml_create"));
            check_not_null(find_native(&env, "rules_forge.continuous_stream_yaml_path_create"));
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
            exprtk_value_t yaml_push_args[8];
            exprtk_value_t yaml_stream_args[7];
            exprtk_value_t push_result;
            exprtk_value_t stream_result;
            exprtk_value_t yaml_push_result;
            exprtk_value_t yaml_stream_result;

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

            memcpy(yaml_push_args, push_args, sizeof(yaml_push_args));
            yaml_push_args[3] = make_string(&env,
                "events:\n  - event_id: event-1\n    event_time: 1720000000000\n");
            yaml_push_args[4] = make_string(&env, "/events/*");
            yaml_push_result = call_native(&env,
                "rules_forge.continuous_push_yaml_path_schema", 8, yaml_push_args);

            yaml_stream_args[0] = yaml_push_args[0];
            yaml_stream_args[1] = yaml_push_args[1];
            yaml_stream_args[2] = yaml_push_args[2];
            yaml_stream_args[3] = yaml_push_args[4];
            yaml_stream_args[4] = yaml_push_args[5];
            yaml_stream_args[5] = yaml_push_args[6];
            yaml_stream_args[6] = yaml_push_args[7];
            yaml_stream_result = call_native(&env,
                "rules_forge.continuous_stream_yaml_path_create", 7, yaml_stream_args);

            check_int_eq(exprtk_map_get(&push_result, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&push_result, "result").data.integer, -1);
            check_float_eq(stream_result.data.number, -1.0, 0.001);
            check_int_eq(exprtk_map_get(&yaml_push_result, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&yaml_push_result, "result").data.integer, -1);
            check_float_eq(yaml_stream_result.data.number, -1.0, 0.001);

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
            exprtk_value_t yaml_args[5];
            exprtk_value_t one;
            exprtk_value_t many;
            exprtk_value_t yaml_one;
            exprtk_value_t yaml_many;
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

            memcpy(yaml_args, json_args, sizeof(yaml_args));
            yaml_args[3] = make_string(&env, "events:\n  - name: launch\n");
            yaml_args[4] = make_string(&env, "/events/*");
            yaml_one = call_native(&env, "rules_forge.session_add_fact_yaml_path_schema",
                                   5, yaml_args);
            yaml_many = call_native(&env, "rules_forge.session_add_facts_yaml_path_schema",
                                    5, yaml_args);

            memcpy(csv_args, json_args, sizeof(csv_args));
            csv_args[3] = make_string(&env, "name_s,age_n\nAlice,30\n");
            csv_args[4] = make_string(&env, "age > 18");
            csv = call_native(&env, "rules_forge.session_add_facts_csv_path_schema",
                              5, csv_args);

            check_float_eq(one.data.number, -1.0, 0.001);
            check_int_eq(exprtk_map_get(&many, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&many, "loaded").data.integer, 0);
            check_float_eq(yaml_one.data.number, -1.0, 0.001);
            check_int_eq(exprtk_map_get(&yaml_many, "status").data.integer, 2);
            check_int_eq(exprtk_map_get(&yaml_many, "loaded").data.integer, 0);
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

        it("should bind all DataBind value kinds from YAML selected by YPATH") {
            static const char schema_text[] =
                "schema Market [id(12), version(1), byte_order(little)]; "
                "composite Header { uint32 seq; uint64 ts; } "
                "message FullFact { Header header; list<uint32> values; "
                "set<string> tags; map<string,int32> attrs; bytes raw; uuid id; "
                "datetime at; date trade_date; time trade_time; duration latency; "
                "decimal price; bigint sequence; money total; bool active; }";
            static const char yaml_text[] =
                "events:\n"
                "  - header: {seq: 7, ts: 99}\n"
                "    values: [3, 4]\n"
                "    tags: [alpha, beta]\n"
                "    attrs: {x: 30, y: 40}\n"
                "    raw: Az\n"
                "    id: 01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001\n"
                "    at: 'Sat, 04 Mar 2006 13:27:54 GMT'\n"
                "    trade_date: '2026-06-28'\n"
                "    trade_time: '09:30:05.123'\n"
                "    latency: 1h30m5s250ms\n"
                "    price: '123.4500'\n"
                "    sequence: '000123456789012345678901234567890'\n"
                "    total: {amount: '123.4500', currency: USD}\n"
                "    active: true\n";
            ts_plugin_handle_t *h = ts_plugin_load(RULES_FORGE_PLUGIN_DLL);
            exprtk_env_t env;
            mem_pool_t scratch;
            char *schema_path = tt_make_temp_file("rfg", ".schema");
            char *rule_path = NULL;
            char rules[2048];
            int rules_len = -1;
            exprtk_value_t kb = exprtk_val_num(-1.0);
            exprtk_value_t session = exprtk_val_num(-1.0);
            exprtk_value_t query = exprtk_val_num(-1.0);
            exprtk_value_t fact = exprtk_val_num(-1.0);

            check_not_null(h);
            check_not_null(schema_path);
            if (!h || !schema_path) {
                free(schema_path);
                if (h) ts_plugin_unload(h);
                return;
            }
            check_int_eq(tt_write_file(schema_path, schema_text, sizeof(schema_text) - 1), 0);
            rule_path = make_rule_path(schema_path);
            check_not_null(rule_path);
            if (!rule_path) {
                tt_remove_file(schema_path);
                free(schema_path);
                ts_plugin_unload(h);
                return;
            }

            rules_len = snprintf(
                rules, sizeof(rules),
                "import \"%s\";\n"
                "query \"FindFullFact\"\n"
                "  $f : FullFact("
                "id == \"01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001\", "
                "trade_date == \"2026-06-28\", trade_time == \"09:30:05.123\", "
                "latency == 5405250, price == \"123.45\", "
                "sequence == \"123456789012345678901234567890\", "
                "total == \"USD 123.45\")\n"
                "end\n",
                rule_path);
            check(rules_len > 0 && (size_t)rules_len < sizeof(rules));

            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

            kb = call_native(&env, "rules_forge.kb_create", 0, NULL);
            check(kb.data.number >= 0.0);
            if (kb.data.number >= 0.0) {
                exprtk_value_t load_args[2] = {kb, make_string(&env, rules)};
                exprtk_value_t create_args[1] = {kb};
                exprtk_value_t load_status = call_native(
                    &env, "rules_forge.kb_load", 2, load_args);
                check_float_eq(load_status.data.number, 0.0, 0.001);
                session = call_native(&env, "rules_forge.session_create", 1, create_args);
            }
            check(session.data.number >= 0.0);

            if (session.data.number >= 0.0) {
                exprtk_value_t add_args[5] = {
                    session,
                    make_string(&env, schema_path),
                    make_string(&env, "FullFact"),
                    make_string(&env, yaml_text),
                    make_string(&env, "/events/*")
                };
                exprtk_value_t query_args[2] = {session, make_string(&env, "FindFullFact")};
                exprtk_value_t loaded = call_native(
                    &env, "rules_forge.session_add_facts_yaml_path_schema", 5, add_args);
                check_int_eq(exprtk_map_get(&loaded, "status").data.integer, 0);
                check_int_eq(exprtk_map_get(&loaded, "loaded").data.integer, 1);

                query = call_native(&env, "rules_forge.session_query", 2, query_args);
                check(query.data.number >= 0.0);
            }

            if (query.data.number >= 0.0) {
                exprtk_value_t size_args[1] = {query};
                exprtk_value_t fact_args[3] = {
                    query, exprtk_val_int(0), make_string(&env, "f")
                };
                check_int_eq(call_native(&env, "rules_forge.query_size", 1, size_args).data.integer,
                             1);
                fact = call_native(&env, "rules_forge.query_fact", 3, fact_args);
                check(fact.data.number >= 0.0);
            }

            if (fact.data.number >= 0.0) {
                exprtk_value_t field_args[2] = {fact, make_string(&env, "at")};
                exprtk_value_t at = call_native(&env, "rules_forge.fact_string", 2, field_args);
                check_int_eq(at.type, EXPRTK_VAL_STRING);
                check(at.data.string.len > 0);
                field_args[1] = make_string(&env, "latency");
                check_int_eq(call_native(&env, "rules_forge.fact_int", 2, field_args).data.integer,
                             5405250);
                field_args[1] = make_string(&env, "total");
                check_str_eq(call_native(&env, "rules_forge.fact_string", 2, field_args)
                                 .data.string.data,
                             "USD 123.45");
            }

            if (query.data.number >= 0.0) {
                exprtk_value_t args[1] = {query};
                check_float_eq(call_native(&env, "rules_forge.query_destroy", 1, args).data.number,
                               0.0, 0.001);
            }
            if (session.data.number >= 0.0) {
                exprtk_value_t args[1] = {session};
                check_float_eq(call_native(&env, "rules_forge.session_destroy", 1, args).data.number,
                               0.0, 0.001);
            }
            if (kb.data.number >= 0.0) {
                exprtk_value_t args[1] = {kb};
                check_float_eq(call_native(&env, "rules_forge.kb_destroy", 1, args).data.number,
                               0.0, 0.001);
            }

            ts_plugin_unload(h);
            exprtk_env_free(&env);
            mem_destroy(&scratch);
            check_int_eq(tt_remove_file(schema_path), 0);
            free(rule_path);
            free(schema_path);
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
