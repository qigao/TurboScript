/**
 * @file net_ctx.h
 * @brief Net plugin context — HTTP client lifecycle.
 */
#ifndef NET_CTX_H
#define NET_CTX_H

#include "exprtk.h"
#include "salts_buffer.h"
#include <http_client/http.h>

#include <string.h>
#include <stdlib.h>

#define NET_WS_TASK_CONNECTION_CAPACITY 256

typedef struct net_ws_task_connection_s {
    const void                *owner;
    chttp_websocket_client    *client;
} net_ws_task_connection_t;

typedef struct net_ctx_s {
    /* CHTTP is sequential and caller-driven. The context owns its one
     * requests client and bounded WebSocket clients. */
    chttp_client *client;
    chttp_websocket_client *ws_client;
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

chttp_client *net_ctx_ensure_client(net_ctx_t *ctx);

static inline char *net_arena_cstr(mem_pool_t *a, vstr sv) {
    char *buf = mem_alloc(a, sv.len + 1);
    if (buf) { memcpy(buf, sv.data, sv.len); buf[sv.len] = '\0'; }
    return buf;
}

void *net_ctx_create(void);
void net_ctx_destroy(void *ctx);
void net_load(void *ctx, void *env, void *scratch);

#endif /* NET_CTX_H */
