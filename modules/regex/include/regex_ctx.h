/**
 * @file regex_ctx.h
 * @brief Regex module context - pattern matching via libfsm
 */
#ifndef REGEX_CTX_H
#define REGEX_CTX_H

#include "exprtk.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REGEX_MAX_PATTERNS 32

/**
 * @brief Compiled regex pattern handle
 */
typedef struct regex_pattern_s {
    void *fsm;           /* libfsm FSM object */
    char *pattern_str;   /* Original pattern string */
} regex_pattern_t;

/**
 * @brief Regex module context
 */
typedef struct regex_ctx_s {
    regex_pattern_t *patterns[REGEX_MAX_PATTERNS];
    char error_msg[256];
} regex_ctx_t;

/**
 * @brief User data passed to regex functions
 */
typedef struct {
    regex_ctx_t *ctx;
    exprtk_env_t *env;
    mem_pool_t *scratch;
} regex_ud_t;

/* Lifecycle functions */
void *regex_ctx_create(void);
void regex_ctx_destroy(void *ctx);
void regex_load(void *ctx, void *env, void *scratch);

#ifdef __cplusplus
}
#endif

#endif /* REGEX_CTX_H */
