/**
 * @file test_parser.c
 * @brief Parser module unit tests
 */
#include "parser_ctx.h"
#include "tinytest.h"
#include "exprtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global context for tests */
static parser_ctx_t *g_ctx = NULL;

/* Setup/teardown */
static void setup_context(void) {
    if (!g_ctx) {
        g_ctx = (parser_ctx_t *)parser_ctx_create();
    }
}

static void teardown_context(void) {
    if (g_ctx) {
        parser_ctx_destroy(g_ctx);
        g_ctx = NULL;
    }
}

static exprtk_value_t make_str(exprtk_env_t *env, const char *s) {
    size_t len = strlen(s);
    char *buf = mem_alloc(&env->arena, len + 1);
    check_not_null(buf);
    if (!buf) return exprtk_val_str(tstr_v_from_buf("", 0));
    memcpy(buf, s, len + 1);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static exprtk_value_t call_fn(exprtk_env_t *env, const char *name,
                              size_t argc, exprtk_value_t *args) {
    exprtk_func_t *fn = env ? env->funcs : NULL;
    while (fn) {
        if (strcmp(fn->name, name) == 0 && !fn->is_script) {
            return fn->data.native.fn(argc, args, fn->data.native.user_data);
        }
        fn = fn->next;
    }
    return exprtk_val_num(-999.0);
}

spec("parser_module") {
    before_each() {
        setup_context();
    }
    
    after_each() {
        teardown_context();
    }
    
    describe("context") {
        it("should create context") {
            check_not_null(g_ctx);
            check_int_eq(g_ctx->error_msg[0], '\0');
        }
        
        it("should have all document handles NULL") {
            for (int i = 0; i < PARSER_MAX_DOCS; i++) {
                check_null(g_ctx->csv_docs[i]);
            }
        }

        it("should have all schema handles NULL") {
            for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
                check_null(g_ctx->schemas[i]);
            }
        }
    }
    
    describe("csv_parsing") {
        it("should parse simple CSV") {
            const char *csv_data = "name,age\nAlice,30\nBob,25\n";
            
            turbo_csv_options_t opts = {1, ',', '"', 1};
            turbo_csv_doc_t *doc = NULL;
            
            int rc = turbo_parse_csv_opts((const uint8_t *)csv_data, strlen(csv_data), &opts, &doc);
            check_int_eq(rc, 0);
            check_not_null(doc);
            
            size_t rows = turbo_csv_row_count(doc);
            size_t cols = turbo_csv_column_count(doc);
            
            check_int_eq(rows, 2); /* 2 data rows (header not counted) */
            check_int_eq(cols, 2);
            
            /* Check values - row index starts from 0 for data rows */
            const char *val = turbo_csv_get(doc, 0, 0);
            check_not_null(val);
            check_str_eq(val, "Alice");
            
            val = turbo_csv_get(doc, 0, 1);
            check_not_null(val);
            check_str_eq(val, "30");
            
            val = turbo_csv_get(doc, 1, 0);
            check_not_null(val);
            check_str_eq(val, "Bob");
            
            /* Cleanup */
            void *ptr = doc;
            turbo_free_csv(&ptr);
        }
    }
    
    describe("json_parsing") {
        it("should query JSON") {
            const char *json = "{\"user\":\"alice\",\"score\":99}";
            
            json_value_t *root = NULL;
            int rc = turbo_parse_json((const uint8_t *)json, strlen(json), &root);
            
            check_int_eq(rc, 0);
            check_not_null(root);
            
            json_value_t *user_val = turbo_json_object_get(root, "user");
            check_not_null(user_val);
            
            const char *user = turbo_json_string(user_val);
            check_not_null(user);
            check_str_eq(user, "alice");
            
            void *ptr = root;
            turbo_free_json(&ptr);
        }
        
        it("should query JSON number") {
            const char *json = "{\"user\":\"alice\",\"score\":99}";
            
            json_value_t *root = NULL;
            int rc = turbo_parse_json((const uint8_t *)json, strlen(json), &root);
            
            check_int_eq(rc, 0);
            check_not_null(root);
            
            json_value_t *score_val = turbo_json_object_get(root, "score");
            check_not_null(score_val);
            
            double score = turbo_json_number(score_val);
            check(score == 99.0);
            
            void *ptr = root;
            turbo_free_json(&ptr);
        }
    }

    describe("schema_binding") {
        it("should bind validate and emit uuid schema fields") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[3];
            exprtk_value_t event;
            exprtk_value_t id;
            exprtk_value_t emitted;
            exprtk_value_t csv_text;
            exprtk_value_t valid;
            exprtk_value_t invalid;
            const char *schema = "message Event { uuid id; }\n";
            const char *json = "{\"id\":\"01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001\"}";
            const char *csv = "id\n01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001";
            const char *bad_json = "{\"id\":\"not-a-uuid\"}";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Event");
            event = call_fn(&env, "json.bind", 3, args);
            check_int_eq(event.type, EXPRTK_VAL_OBJECT);
            id = exprtk_map_get(&event, "id");
            check_int_eq(id.type, EXPRTK_VAL_UUID);

            args[1] = event;
            emitted = call_fn(&env, "json.emit", 3, args);
            check_int_eq(emitted.type, EXPRTK_VAL_STRING);
            check_int_eq(strncmp(emitted.data.string.data, json, emitted.data.string.len), 0);

            csv_text = call_fn(&env, "csv.emit", 3, args);
            check_int_eq(csv_text.type, EXPRTK_VAL_STRING);
            check_str_eq(csv_text.data.string.data, csv);

            args[1] = make_str(&env, csv);
            valid = call_fn(&env, "csv.validate_ex", 3, args);
            check_int_eq(valid.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, json);
            valid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(valid.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, bad_json);
            invalid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(invalid.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&invalid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&invalid, "ok").data.integer, 0);
            check_str_eq(exprtk_map_get(&invalid, "message").data.string.data,
                         "JSON validation failed for type: Event");

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should bind validate and emit bytes schema fields as native values") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[3];
            exprtk_value_t blob;
            exprtk_value_t raw;
            exprtk_value_t emitted;
            exprtk_value_t csv_text;
            exprtk_value_t valid;
            const char *schema = "message Blob { bytes raw; }\n";
            const char *json = "{\"raw\":\"Az\"}";
            const char *csv = "raw\nAz";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Blob");
            blob = call_fn(&env, "json.bind", 3, args);
            check_int_eq(blob.type, EXPRTK_VAL_OBJECT);
            raw = exprtk_map_get(&blob, "raw");
            check_int_eq(raw.type, EXPRTK_VAL_BYTES);
            check_size_eq(raw.data.bytes.len, 2);
            check_int_eq((unsigned char)raw.data.bytes.data[0], 'A');
            check_int_eq((unsigned char)raw.data.bytes.data[1], 'z');

            args[1] = blob;
            emitted = call_fn(&env, "json.emit", 3, args);
            check_int_eq(emitted.type, EXPRTK_VAL_STRING);
            check_str_eq(emitted.data.string.data, json);

            csv_text = call_fn(&env, "csv.emit", 3, args);
            check_int_eq(csv_text.type, EXPRTK_VAL_STRING);
            check_str_eq(csv_text.data.string.data, csv);

            args[1] = make_str(&env, json);
            valid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, csv);
            valid = call_fn(&env, "csv.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should bind datetime schema fields as native values") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[3];
            exprtk_value_t event;
            exprtk_value_t at;
            exprtk_value_t emitted;
            exprtk_value_t text_args[1];
            exprtk_value_t json_text;
            exprtk_value_t csv_text;
            exprtk_value_t valid;
            exprtk_value_t invalid;
            const char *schema = "message Event { datetime at; }\n";
            const char *json = "{\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\"}";
            const char *csv = "at\n\"Sat, 04 Mar 2006 13:27:54 GMT\"";
            const char *bad_json = "{\"at\":\"not-a-date\"}";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Event");
            event = call_fn(&env, "json.bind", 3, args);
            check_int_eq(event.type, EXPRTK_VAL_OBJECT);
            at = exprtk_map_get(&event, "at");
            check_int_eq(at.type, EXPRTK_VAL_DATETIME);
            check_int_eq(at.data.datetime.year, 2006);
            check_int_eq(at.data.datetime.month, 3);

            text_args[0] = at;
            json_text = call_fn(&env, "json.stringify", 1, text_args);
            check_int_eq(json_text.type, EXPRTK_VAL_STRING);
            check_str_eq(json_text.data.string.data, "\"Sat, 04 Mar 2006 13:27:54 GMT\"");

            args[1] = event;
            emitted = call_fn(&env, "json.emit", 3, args);
            check_int_eq(emitted.type, EXPRTK_VAL_STRING);
            check_str_eq(emitted.data.string.data, json);

            args[1] = make_str(&env, json);
            valid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, bad_json);
            invalid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&invalid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&invalid, "ok").data.integer, 0);
            check_str_eq(exprtk_map_get(&invalid, "message").data.string.data,
                         "JSON validation failed for type: Event");

            args[1] = event;
            csv_text = call_fn(&env, "csv.emit", 3, args);
            check_int_eq(csv_text.type, EXPRTK_VAL_STRING);
            check_str_eq(csv_text.data.string.data, csv);

            args[1] = make_str(&env, csv);
            valid = call_fn(&env, "csv.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should stringify native date time and duration values") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[1];
            exprtk_value_t values;
            exprtk_value_t json_text;

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            values = exprtk_val_list_empty();
            exprtk_list_push(&values, exprtk_val_date((exprtk_date_t){2026, 6, 28}));
            exprtk_list_push(&values, exprtk_val_time((exprtk_time_t){9, 30, 5, 123}));
            exprtk_list_push(&values, exprtk_val_duration(5405250));
            args[0] = values;
            json_text = call_fn(&env, "json.stringify", 1, args);
            check_int_eq(json_text.type, EXPRTK_VAL_STRING);
            check_str_eq(json_text.data.string.data,
                         "[\"2026-06-28\",\"09:30:05.123\",\"1:30:05.250\"]");

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should bind date time and duration schema fields as native values") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[4];
            exprtk_value_t event;
            exprtk_value_t d;
            exprtk_value_t t;
            exprtk_value_t span;
            exprtk_value_t emitted;
            exprtk_value_t csv_text;
            exprtk_value_t xml_event;
            exprtk_value_t valid;
            exprtk_value_t invalid;
            const char *schema = "message Event { date d; time t; duration span; }\n";
            const char *json =
                "{\"d\":\"2026-06-28\",\"t\":\"09:30:05.123\",\"span\":\"1h30m5s250ms\"}";
            const char *csv = "d,t,span\n2026-06-28,09:30:05.123,1:30:05.250";
            const char *xml =
                "<event><d>2026-06-28</d><t>09:30:05.123</t>"
                "<span>1h30m5s250ms</span></event>";
            const char *bad_json =
                "{\"d\":\"2026-02-31\",\"t\":\"09:30:05.123\",\"span\":\"1h30m5s250ms\"}";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Event");
            event = call_fn(&env, "json.bind", 3, args);
            check_int_eq(event.type, EXPRTK_VAL_OBJECT);
            d = exprtk_map_get(&event, "d");
            t = exprtk_map_get(&event, "t");
            span = exprtk_map_get(&event, "span");
            check_int_eq(d.type, EXPRTK_VAL_DATE);
            check_int_eq(d.data.date.year, 2026);
            check_int_eq(d.data.date.month, 6);
            check_int_eq(d.data.date.day, 28);
            check_int_eq(t.type, EXPRTK_VAL_TIME);
            check_int_eq(t.data.time.hour, 9);
            check_int_eq(t.data.time.millisecond, 123);
            check_int_eq(span.type, EXPRTK_VAL_DURATION);
            check_int_eq((int)span.data.duration_ms, 5405250);

            args[1] = event;
            emitted = call_fn(&env, "json.emit", 3, args);
            check_int_eq(emitted.type, EXPRTK_VAL_STRING);
            check_str_eq(emitted.data.string.data,
                         "{\"d\":\"2026-06-28\",\"t\":\"09:30:05.123\","
                         "\"span\":\"1:30:05.250\"}");

            csv_text = call_fn(&env, "csv.emit", 3, args);
            check_int_eq(csv_text.type, EXPRTK_VAL_STRING);
            check_str_eq(csv_text.data.string.data, csv);

            args[1] = make_str(&env, xml);
            xml_event = call_fn(&env, "xml.bind", 3, args);
            check_int_eq(xml_event.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&xml_event, "d").type, EXPRTK_VAL_DATE);
            check_int_eq(exprtk_map_get(&xml_event, "t").type, EXPRTK_VAL_TIME);
            check_int_eq(exprtk_map_get(&xml_event, "span").type, EXPRTK_VAL_DURATION);

            valid = call_fn(&env, "xml.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, bad_json);
            invalid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&invalid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&invalid, "ok").data.integer, 0);
            check_str_eq(exprtk_map_get(&invalid, "message").data.string.data,
                         "JSON validation failed for type: Event");

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should bind decimal schema fields as native values") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[4];
            exprtk_value_t quote;
            exprtk_value_t price;
            exprtk_value_t emitted;
            exprtk_value_t csv_text;
            exprtk_value_t xml_quote;
            exprtk_value_t valid;
            exprtk_value_t invalid;
            const char *schema = "message Quote { decimal price; }\n";
            const char *json = "{\"price\":\"123.4500\"}";
            const char *csv = "price\n123.45";
            const char *xml = "<quote><price>-0.1250</price></quote>";
            const char *bad_json = "{\"price\":\"bad\"}";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Quote");
            quote = call_fn(&env, "json.bind", 3, args);
            check_int_eq(quote.type, EXPRTK_VAL_OBJECT);
            price = exprtk_map_get(&quote, "price");
            check_int_eq(price.type, EXPRTK_VAL_DECIMAL);
            check_int_eq((int)price.data.decimal.mantissa, 12345);
            check_int_eq(price.data.decimal.scale, 2);

            args[1] = quote;
            emitted = call_fn(&env, "json.emit", 3, args);
            check_int_eq(emitted.type, EXPRTK_VAL_STRING);
            check_str_eq(emitted.data.string.data, "{\"price\":\"123.45\"}");

            csv_text = call_fn(&env, "csv.emit", 3, args);
            check_int_eq(csv_text.type, EXPRTK_VAL_STRING);
            check_str_eq(csv_text.data.string.data, csv);

            args[1] = make_str(&env, xml);
            xml_quote = call_fn(&env, "xml.bind", 3, args);
            price = exprtk_map_get(&xml_quote, "price");
            check_int_eq(price.type, EXPRTK_VAL_DECIMAL);
            check_int_eq((int)price.data.decimal.mantissa, -125);
            check_int_eq(price.data.decimal.scale, 3);

            valid = call_fn(&env, "xml.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, bad_json);
            invalid = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&invalid, "ok").data.integer, 0);
            check_str_eq(exprtk_map_get(&invalid, "message").data.string.data,
                         "JSON validation failed for type: Quote");

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should bind bigint and money schema fields as native values") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[4];
            exprtk_value_t invoice;
            exprtk_value_t id;
            exprtk_value_t total;
            exprtk_value_t amount;
            exprtk_value_t currency;
            exprtk_value_t fields;
            const char *schema =
                "message Invoice { bigint id; money total; [format(regex)] string pattern; }\n";
            const char *json =
                "{\"id\":\"000123456789012345678901234567890\","
                "\"total\":{\"amount\":\"123.4500\",\"currency\":\"USD\"},"
                "\"pattern\":\"^[A-Z]+$\"}";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Invoice");
            invoice = call_fn(&env, "json.bind", 3, args);
            check_int_eq(invoice.type, EXPRTK_VAL_OBJECT);
            id = exprtk_map_get(&invoice, "id");
            check_int_eq(id.type, EXPRTK_VAL_BIGINT);
            check_int_eq(id.data.bigint.text.len, strlen("123456789012345678901234567890"));
            check_int_eq(strncmp(id.data.bigint.text.data,
                                 "123456789012345678901234567890",
                                 id.data.bigint.text.len), 0);
            total = exprtk_map_get(&invoice, "total");
            check_int_eq(total.type, EXPRTK_VAL_MONEY);
            amount = exprtk_val_decimal(total.data.money.amount);
            currency = exprtk_val_str(tstr_v_from_buf(total.data.money.currency, 3));
            check_int_eq(amount.data.decimal.mantissa, 12345);
            check_int_eq(amount.data.decimal.scale, 2);
            check_int_eq(currency.type, EXPRTK_VAL_STRING);
            check_int_eq(currency.data.string.len, 3);
            check_int_eq(strncmp(currency.data.string.data, "USD", 3), 0);

            args[1] = make_str(&env, "Invoice");
            fields = call_fn(&env, "schema.fields", 2, args);
            check_int_eq(fields.type, EXPRTK_VAL_LIST);
            check_size_eq(fields.data.list.count, 3);
            check_str_eq(exprtk_map_get(&fields.data.list.items[2], "format").data.string.data,
                         "regex");

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should bind and validate XML schema data through DataBind") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[4];
            exprtk_value_t event;
            exprtk_value_t id;
            exprtk_value_t raw;
            exprtk_value_t at;
            exprtk_value_t valid;
            exprtk_value_t invalid;
            exprtk_value_t rows;
            const char *schema =
                "message Event { uuid id; bytes raw; datetime at; }\n";
            const char *xml =
                "<event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id>"
                "<raw>Az</raw><at>Sat, 04 Mar 2006 13:27:54 GMT</at></event>";
            const char *many_xml =
                "<events><event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id>"
                "<raw>Az</raw><at>Sat, 04 Mar 2006 13:27:54 GMT</at></event>"
                "<event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id>"
                "<raw>By</raw><at>Sat, 04 Mar 2006 13:27:54 GMT</at></event></events>";
            const char *bad_xml =
                "<event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id>"
                "<raw>Az</raw><at>not-a-date</at></event>";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, xml);
            args[2] = make_str(&env, "Event");
            event = call_fn(&env, "xml.bind", 3, args);
            check_int_eq(event.type, EXPRTK_VAL_OBJECT);

            id = exprtk_map_get(&event, "id");
            raw = exprtk_map_get(&event, "raw");
            at = exprtk_map_get(&event, "at");
            check_int_eq(id.type, EXPRTK_VAL_UUID);
            check_int_eq(raw.type, EXPRTK_VAL_BYTES);
            check_size_eq(raw.data.bytes.len, 2);
            check_int_eq((unsigned char)raw.data.bytes.data[0], 'A');
            check_int_eq((unsigned char)raw.data.bytes.data[1], 'z');
            check_int_eq(at.type, EXPRTK_VAL_DATETIME);
            check_int_eq(at.data.datetime.year, 2006);

            valid = call_fn(&env, "xml.validate_ex", 3, args);
            check_int_eq(valid.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            args[1] = make_str(&env, bad_xml);
            invalid = call_fn(&env, "xml.validate_ex", 3, args);
            check_int_eq(invalid.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&invalid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&invalid, "ok").data.integer, 0);

            args[1] = make_str(&env, many_xml);
            args[2] = make_str(&env, "//event");
            args[3] = make_str(&env, "Event");
            rows = call_fn(&env, "xml.bind_all", 4, args);
            check_int_eq(rows.type, EXPRTK_VAL_LIST);
            check_size_eq(rows.data.list.count, 2);

            valid = call_fn(&env, "xml.validate_ex", 4, args);
            check_int_eq(valid.type, EXPRTK_VAL_OBJECT);
            check_int_eq(exprtk_map_get(&valid, "ok").type, EXPRTK_VAL_INTEGER);
            check_int_eq(exprtk_map_get(&valid, "ok").data.integer, 1);

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }

        it("should validate formatted schema strings through DataBind") {
            exprtk_env_t env;
            mem_pool_t scratch;
            exprtk_value_t args[4];
            exprtk_value_t endpoint;
            exprtk_value_t ip;
            exprtk_value_t fields;
            exprtk_value_t first_field;
            exprtk_value_t ok;
            exprtk_value_t bad;
            const char *schema =
                "message Endpoint { "
                "[format(ipaddr)] string ip; "
                "[format(url)] string href; "
                "[format(email)] string owner; "
                "}\n";
            const char *json =
                "{\"ip\":\"127.0.0.1\",\"href\":\"https://example.com\","
                "\"owner\":\"dev@example.com\"}";
            const char *invalid =
                "{\"ip\":\"999.0.0.1\",\"href\":\"not a url\","
                "\"owner\":\"dev.example.com\"}";

            exprtk_env_init(&env);
            mem_init(&scratch, 65536);
            parser_load(g_ctx, &env, &scratch);

            args[0] = make_str(&env, schema);
            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Endpoint");
            endpoint = call_fn(&env, "json.bind", 3, args);
            check_int_eq(endpoint.type, EXPRTK_VAL_OBJECT);
            ip = exprtk_map_get(&endpoint, "ip");
            check_int_eq(ip.type, EXPRTK_VAL_STRING);
            check_str_eq(ip.data.string.data, "127.0.0.1");

            args[1] = make_str(&env, "Endpoint");
            fields = call_fn(&env, "schema.fields", 2, args);
            check_int_eq(fields.type, EXPRTK_VAL_LIST);
            check_size_eq(fields.data.list.count, 3);
            first_field = fields.data.list.items[0];
            check_str_eq(exprtk_map_get(&first_field, "format").data.string.data, "ipaddr");

            args[1] = make_str(&env, json);
            args[2] = make_str(&env, "Endpoint");
            ok = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&ok, "ok").data.integer, 1);

            args[1] = make_str(&env, invalid);
            bad = call_fn(&env, "json.validate_ex", 3, args);
            check_int_eq(exprtk_map_get(&bad, "ok").data.integer, 0);

            mem_destroy(&scratch);
            exprtk_env_free(&env);
        }
    }
}
