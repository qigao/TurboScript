/**
 * @file data_bind_ctx.h
 * @brief data_bind plugin context - codec and stream parser handle tables.
 *
 * Each codec handle holds a `DataBind *` from the tbe/data_bind library.
 * Each stream handle holds a stateful `data_bind_stream_t *` and the result
 * slot bound to that stream for its full lifetime.
 * Parse results are returned by the core data_bind library as DataBindValue
 * trees and converted at the module boundary into exprtk values.
 */
#ifndef DATA_BIND_CTX_H
#define DATA_BIND_CTX_H

#include "exprtk.h"
#include "turbo_buffer.h"
#include "turbo_hash.h"
#include "data_bind.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Maximum number of concurrently open codec handles per plugin instance */
#define DB_MAX_HANDLES 32

typedef struct {
    DataBind *codec;
    data_bind_stream_t *stream;
    DataBindValue *result;
    DataBindError error;
    exprtk_env_t *env;
    exprtk_value_t record_callback;
} db_stream_entry_t;

TURBO_HASH_MAP_DEFINE(db_handle_map_t, int, DataBind *)
TURBO_HASH_MAP_DEFINE(db_stream_map_t, int, db_stream_entry_t *)

/* ── Handle table ─────────────────────────────────────────────────────────── */

typedef struct {
    db_handle_map_t handles;
    db_stream_map_t streams;
    int next_handle;
    int next_stream_handle;
} db_ctx_t;

/* ── Plugin user-data (kept alive for the env lifetime) ──────────────────── */

typedef struct {
    db_ctx_t     *ctx;
    exprtk_env_t *env;
    mem_pool_t   *scratch;
} db_ud_t;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

#define DB_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

#define DB_ERROR(ud, msg) do { (ud)->env->aborted = 1; } while (0)

/* Allocate a NUL-terminated copy of a tstr_v string into the arena */
static inline char *db_arena_cstr(mem_pool_t *arena, const char *src, size_t len) {
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return NULL;
    memcpy(buf, src, len);
    buf[len] = '\0';
    return buf;
}

/* ── Forward declarations ─────────────────────────────────────────────────── */

void *db_ctx_create(void);
void  db_ctx_destroy(void *ctx);
void  db_plugin_load(void *ctx, void *env, void *scratch);

#endif /* DATA_BIND_CTX_H */
