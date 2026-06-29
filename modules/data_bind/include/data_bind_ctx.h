/**
 * @file data_bind_ctx.h
 * @brief data_bind plugin context - codec handle table.
 *
 * Each codec handle holds a `DataBind *` from the tbe/data_bind library.
 * Parse results are returned by the core data_bind library as DataBindValue
 * trees and converted at the module boundary into exprtk values.
 */
#ifndef DATA_BIND_CTX_H
#define DATA_BIND_CTX_H

#include "exprtk.h"
#include "turbo_buffer.h"
#include "data_bind.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Maximum number of concurrently open codec handles per plugin instance */
#define DB_MAX_HANDLES 32

/* ── Handle table ─────────────────────────────────────────────────────────── */

typedef struct {
    DataBind *codec;          /**< Underlying JIT codec (NULL = free slot) */
} db_handle_t;

typedef struct {
    db_handle_t handles[DB_MAX_HANDLES];
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
