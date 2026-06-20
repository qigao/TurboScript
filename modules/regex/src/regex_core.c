/**
 * @file regex_core.c
 * @brief Regex module core implementation
 */
#include "regex_ctx.h"
#include <stdlib.h>
#include <string.h>
#include <fsm/fsm.h>
#include <re/re.h> 

void *regex_ctx_create(void) {
    regex_ctx_t *ctx = (regex_ctx_t *)calloc(1, sizeof(regex_ctx_t));
    if (!ctx) return NULL;
    
    /* Initialize all pattern handles to NULL */
    for (int i = 0; i < REGEX_MAX_PATTERNS; i++) {
        ctx->patterns[i] = NULL;
    }
    
    ctx->error_msg[0] = '\0';
    return ctx;
}

void regex_ctx_destroy(void *p) {
    regex_ctx_t *ctx = (regex_ctx_t *)p;
    if (!ctx) return;
    
    /* Free all compiled patterns */
    for (int i = 0; i < REGEX_MAX_PATTERNS; i++) {
        if (ctx->patterns[i]) {
            if (ctx->patterns[i]->fsm) {
                fsm_free(ctx->patterns[i]->fsm);
            }
            if (ctx->patterns[i]->pattern_str) {
                free(ctx->patterns[i]->pattern_str);
            }
            free(ctx->patterns[i]);
            ctx->patterns[i] = NULL;
        }
    }
    
    free(ctx);
}
