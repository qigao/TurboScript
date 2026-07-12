/**
 * @file parser_ctx.h
 * @brief Parser module context for configuration and command-line parsing.
 */
#ifndef PARSER_CTX_H
#define PARSER_CTX_H

#include "exprtk.h"
#include "turbo_parser.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parser module context
 */
typedef struct parser_ctx_s {
    char error_msg[256];
} parser_ctx_t;

/**
 * @brief User data passed to parser functions
 */
typedef struct {
    parser_ctx_t *ctx;
    exprtk_env_t *env;
    mem_pool_t *scratch;
} parser_ud_t;

/* Lifecycle functions */
void *parser_ctx_create(void);
void parser_ctx_destroy(void *ctx);
void parser_load(void *ctx, void *env, void *scratch);

#ifdef __cplusplus
}
#endif

#endif /* PARSER_CTX_H */
