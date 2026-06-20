/**
 * @file test_regex.c
 * @brief Regex module unit tests
 */
#include "regex_ctx.h"
#include "tinytest.h"
#include "exprtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fsm/fsm.h>
#include <re/re.h>

/* Global context for tests */
static regex_ctx_t *g_ctx = NULL;

/* Setup/teardown */
static void setup_context(void) {
    if (!g_ctx) {
        g_ctx = (regex_ctx_t *)regex_ctx_create();
    }
}

static void teardown_context(void) {
    if (g_ctx) {
        regex_ctx_destroy(g_ctx);
        g_ctx = NULL;
    }
}

spec("regex_module") {
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
        
        it("should have all pattern handles NULL") {
            for (int i = 0; i < REGEX_MAX_PATTERNS; i++) {
                check_null(g_ctx->patterns[i]);
            }
        }
    }
    
    describe("libfsm_basic") {
        it("should compile simple regex") {
            const char *pattern = "hello";
            enum re_flags flags = 0;
            struct re_err err;
            
            struct fsm *fsm = re_comp(RE_PCRE, fsm_sgetc, &pattern, NULL, flags, &err);
            check_not_null(fsm);
            
            /* Determinise and minimize FSM before execution */
            fsm_determinise(fsm);
            fsm_minimise(fsm);
            
            /* Test match */
            const char *text = "hello";
            fsm_state_t end_state;
            int result = fsm_exec(fsm, fsm_sgetc, &text, &end_state, NULL);
            
            check_int_eq(result, 1);
            
            fsm_free(fsm);
        }
        
        it("should match digit pattern") {
            const char *pattern = "[0-9]+";
            enum re_flags flags = 0;
            struct re_err err;
            
            struct fsm *fsm = re_comp(RE_PCRE, fsm_sgetc, &pattern, NULL, flags, &err);
            check_not_null(fsm);
            
            /* Determinise and minimize FSM before execution */
            fsm_determinise(fsm);
            fsm_minimise(fsm);
            
            /* Test match */
            const char *text = "123";
            fsm_state_t end_state;
            int result = fsm_exec(fsm, fsm_sgetc, &text, &end_state, NULL);
            
            check_int_eq(result, 1);
            
            fsm_free(fsm);
        }
    }
}
