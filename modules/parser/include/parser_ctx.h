/**
 * @file parser_ctx.h
 * @brief Parser module context - CSV/JSON/XML parsing via TurboNet::Parser
 */
#ifndef PARSER_CTX_H
#define PARSER_CTX_H

#include "exprtk.h"
#include "turbo_parser.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DataBind DataBind;

#define PARSER_MAX_DOCS 16
#define PARSER_MAX_SCHEMAS 16
#define PARSER_MAX_JSON_DOCS 16

/**
 * @brief Parser module context
 */
typedef struct parser_ctx_s {
    turbo_csv_doc_t *csv_docs[PARSER_MAX_DOCS];
    char **csv_header_names[PARSER_MAX_DOCS];
    size_t csv_header_counts[PARSER_MAX_DOCS];
    json_value_t *json_docs[PARSER_MAX_JSON_DOCS];
    struct turbo_node_s *schemas[PARSER_MAX_SCHEMAS];
    DataBind *schema_codecs[PARSER_MAX_SCHEMAS];
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
