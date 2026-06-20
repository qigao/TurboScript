/**
 * @file parser_core.c
 * @brief Parser module core implementation
 */
#include "parser_ctx.h"
#include "node_tree.h"
#include <stdlib.h>
#include <string.h>

void *parser_ctx_create(void) {
    parser_ctx_t *ctx = (parser_ctx_t *)calloc(1, sizeof(parser_ctx_t));
    if (!ctx) return NULL;
    
    /* Initialize all document handles to NULL */
    for (int i = 0; i < PARSER_MAX_DOCS; i++) {
        ctx->csv_docs[i] = NULL;
    }
    for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
        ctx->schemas[i] = NULL;
    }
    
    ctx->error_msg[0] = '\0';
    return ctx;
}

void parser_ctx_destroy(void *p) {
    parser_ctx_t *ctx = (parser_ctx_t *)p;
    if (!ctx) return;
    
    /* Free all open CSV documents */
    for (int i = 0; i < PARSER_MAX_DOCS; i++) {
        if (ctx->csv_docs[i]) {
            void *doc = ctx->csv_docs[i];
            turbo_free_csv(&doc);
            ctx->csv_docs[i] = NULL;
        }
    }
    for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
        if (ctx->schemas[i]) {
            node_free((Node *)ctx->schemas[i]);
            ctx->schemas[i] = NULL;
        }
    }
    
    free(ctx);
}
