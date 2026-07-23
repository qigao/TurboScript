/**
 * @file mapper.h
 * @brief Class-first JSON, YAML, and XML mapping for TurboScript.
 */
#ifndef TURBOSCRIPT_MAPPER_H
#define TURBOSCRIPT_MAPPER_H

#include "exprtk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mapper_ctx_s {
    exprtk_env_t *env;
} mapper_ctx_t;

void *mapper_ctx_create(void);
void mapper_ctx_destroy(void *ctx);
void mapper_load(void *ctx, void *env, void *scratch);

#ifdef __cplusplus
}
#endif

#endif
