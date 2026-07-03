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
}