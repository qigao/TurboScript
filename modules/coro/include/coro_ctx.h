/**
 * @file coro_ctx.h
 * @brief Coroutine context and registry for TurboScript.
 */
#ifndef CORO_CTX_H
#define CORO_CTX_H

#include "exprtk.h"
#include "turbo_coro.h"
#include "turbo_coro_pool.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CORO_CTX_MAGIC UINT64_C(0x5453434F524F4358)

/** @deprecated Compatibility alias for the former module-local coroutine handle. */
typedef coro_t mco_coro;

/**
 * @brief Coroutine context backed by TurboUtils::Core.
 */
typedef struct coro_ctx_s {
    mco_coro *coro;              /* TurboUtils coroutine object */
    exprtk_env_t *env;           /* exprtk environment */
    int id;                      /* unique coroutine ID */
    int status;                  /* compatibility mirror: 0=suspended, 1=running, 2=dead */
    
    /* Function to execute */
    char *func_name;             /* function name (owned) */
    
    /* Value passing */
    exprtk_value_t yield_value;  /* value yielded from coroutine */
    exprtk_value_t return_value; /* final return value */
    
    /* Resume arguments */
    exprtk_value_t *resume_args;
    size_t resume_argc;
    
    /* Error handling */
    char error_msg[256];

    /* Appended lifecycle metadata keeps existing field offsets stable. */
    turbo_coro_pool_t *pool;     /* non-NULL when the coroutine is pooled */
    int pooled;                  /* whether pool owns the reusable stack */
    uint64_t magic;              /* distinguishes manual generators from managed tasks */
} coro_ctx_t;

/**
 * @brief Global coroutine registry
 */
typedef struct coro_registry_s {
    coro_ctx_t **coroutines;     /* dynamic array of coroutines */
    size_t capacity;
    size_t count;
    int next_id;
    turbo_coro_pool_t *pool;     /* default-stack coroutine reuse */
} coro_registry_t;

/* Registry management */
coro_registry_t *coro_registry_create(void);
void coro_registry_destroy(coro_registry_t *registry);
int coro_registry_add(coro_registry_t *registry, coro_ctx_t *ctx);
coro_ctx_t *coro_registry_find(coro_registry_t *registry, int id);
void coro_registry_remove(coro_registry_t *registry, int id);

/* Coroutine context management */
coro_ctx_t *coro_ctx_create(exprtk_env_t *env, const char *func_name, size_t stack_size);
coro_ctx_t *coro_ctx_create_pooled(exprtk_env_t *env,
                                   const char *func_name,
                                   size_t stack_size,
                                   turbo_coro_pool_t *pool);
void coro_ctx_destroy(coro_ctx_t *ctx);
int coro_ctx_status(coro_ctx_t *ctx);

/* Get current running coroutine (stored in TurboUtils user_data) */
coro_ctx_t *coro_get_current(void);

#ifdef __cplusplus
}
#endif

#endif /* CORO_CTX_H */
