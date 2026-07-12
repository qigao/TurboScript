/**
 * @file parser_core.c
 * @brief Parser module core implementation
 */
#include "parser_ctx.h"
#include <stdlib.h>

void *parser_ctx_create(void) {
    parser_ctx_t *ctx = (parser_ctx_t *)calloc(1, sizeof(parser_ctx_t));
    if (!ctx) return NULL;

    ctx->error_msg[0] = '\0';
    return ctx;
}

void parser_ctx_destroy(void *p) {
    parser_ctx_t *ctx = (parser_ctx_t *)p;
    if (!ctx) return;
    free(ctx);
}
