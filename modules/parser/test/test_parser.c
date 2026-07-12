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

static int has_native(exprtk_env_t *env, const char *name) {
    exprtk_func_t *fn = env ? env->funcs : NULL;
    while (fn) {
        if (!fn->is_script && strcmp(fn->name, name) == 0) return 1;
        fn = fn->next;
    }
    return 0;
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
        
    }

    describe("module boundary") {
        it("should expose config parsers without structured data APIs") {
            exprtk_env_t env;
            mem_pool_t scratch;

            exprtk_env_init(&env);
            mem_init(&scratch, 4096);
            parser_load(g_ctx, &env, &scratch);

            check_true(has_native(&env, "parser.ini_get"));
            check_true(has_native(&env, "parser.toml_parse"));
            check_false(has_native(&env, "parser.toon_parse"));
            check_false(has_native(&env, "json.query"));
            check_false(has_native(&env, "csv.rows"));
            check_false(has_native(&env, "xml.query"));
            check_false(has_native(&env, "schema.parse"));

            exprtk_env_free(&env);
            mem_destroy(&scratch);
        }
    }
    
}
