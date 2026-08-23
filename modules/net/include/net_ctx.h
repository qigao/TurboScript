/**
 * @file net_ctx.h
 * @brief Net plugin context — HTTP client lifecycle.
 */
#ifndef NET_CTX_H
#define NET_CTX_H

#include "exprtk.h"
#include "turbo_buffer.h"
#include <turbo_http.h>
#include <CoroNet.h>

#include <string.h>
#include <stdlib.h>

#define NET_WS_TASK_CONNECTION_CAPACITY 256

typedef struct net_ws_task_connection_s {
    const coro_cancel_token_t *owner;
    coro_socket_t             *socket;
} net_ws_task_connection_t;

typedef struct net_ctx_s {
    turbo_http_t *client;
    /* The root environment keeps the legacy singleton. Managed tasks use a
     * bounded owner-token registry so ws.* calls in different tasks cannot
     * replace one another's sockets. The context owns every socket and task
     * code only borrows its matching entry until ws.close() or teardown. */
    coro_socket_t *ws_client;
    net_ws_task_connection_t ws_task_connections[NET_WS_TASK_CONNECTION_CAPACITY];
    size_t ws_task_connection_count;
    char           error_msg[256];
} net_ctx_t;

typedef struct {
    net_ctx_t     *ctx;
    exprtk_env_t  *env;
    mem_pool_t *scratch;
} http_ud_t;

#define NET_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

static inline turbo_http_t *net_ctx_ensure_client(net_ctx_t *ctx) {
    if (!ctx) return NULL;
    if (!ctx->client && turbo_http_create_sync(NULL, &ctx->client) != TURBO_OK)
        return NULL;
    return ctx->client;
}

static inline char *net_arena_cstr(mem_pool_t *a, vstr sv) {
    char *buf = mem_alloc(a, sv.len + 1);
    if (buf) { memcpy(buf, sv.data, sv.len); buf[sv.len] = '\0'; }
    return buf;
}

void *net_ctx_create(void);
void net_ctx_destroy(void *ctx);
void net_load(void *ctx, void *env, void *scratch);

#endif /* NET_CTX_H */
