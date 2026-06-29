/**
 * @file parser_core.c
 * @brief Parser module core implementation
 */
#include "parser_ctx.h"
#include "data_bind.h"
#include "node_tree.h"
#include <stdlib.h>
#include <string.h>

void *parser_ctx_create(void) {
    parser_ctx_t *ctx = (parser_ctx_t *)calloc(1, sizeof(parser_ctx_t));
    if (!ctx) return NULL;
    
    /* Initialize all document handles to NULL */
    for (int i = 0; i < PARSER_MAX_DOCS; i++) {
        ctx->csv_docs[i] = NULL;
        ctx->csv_header_names[i] = NULL;
        ctx->csv_header_counts[i] = 0;
    }
    for (int i = 0; i < PARSER_MAX_JSON_DOCS; i++) {
        ctx->json_docs[i] = NULL;
    }
    for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
        ctx->schemas[i] = NULL;
        ctx->schema_codecs[i] = NULL;
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
        for (size_t j = 0; j < ctx->csv_header_counts[i]; j++) {
            free(ctx->csv_header_names[i][j]);
        }
        free(ctx->csv_header_names[i]);
        ctx->csv_header_names[i] = NULL;
        ctx->csv_header_counts[i] = 0;
    }
    for (int i = 0; i < PARSER_MAX_JSON_DOCS; i++) {
        if (ctx->json_docs[i]) {
            void *doc = ctx->json_docs[i];
            turbo_free_json(&doc);
            ctx->json_docs[i] = NULL;
        }
    }
    for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
        if (ctx->schema_codecs[i]) {
            data_bind_free(ctx->schema_codecs[i]);
            ctx->schema_codecs[i] = NULL;
        }
        if (ctx->schemas[i]) {
            node_free((Node *)ctx->schemas[i]);
            ctx->schemas[i] = NULL;
        }
    }
    
    free(ctx);
}
