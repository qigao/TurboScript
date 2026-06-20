/**
 * @file net_ctx.h
 * @brief Net plugin context — HTTP client lifecycle.
 */
#ifndef NET_CTX_H
#define NET_CTX_H

#include "exprtk.h"
#include "turbo_buffer.h"
#include "http_client.h"
#include <CoroNet.h>

#include <string.h>
#include <stdlib.h>

typedef struct net_ctx_s {
    http_client_t *client;
    coro_socket_t *ws_client; 
    char           error_msg[256];
} net_ctx_t;

typedef struct {
    net_ctx_t     *ctx;
    exprtk_env_t  *env;
    mem_pool_t *scratch;
} http_ud_t;

#define NET_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

static inline http_client_t *net_ctx_ensure_client(net_ctx_t *ctx) {
    if (!ctx) return NULL;
    if (!ctx->client) ctx->client = http_client_create(NULL);
    return ctx->client;
}

static inline char *net_arena_cstr(mem_pool_t *a, tstr_v sv) {
    char *buf = mem_alloc(a, sv.len + 1);
    if (buf) { memcpy(buf, sv.data, sv.len); buf[sv.len] = '\0'; }
    return buf;
}

void *net_ctx_create(void);
void net_ctx_destroy(void *ctx);
void net_load(void *ctx, void *env, void *scratch);

#endif /* NET_CTX_H */
