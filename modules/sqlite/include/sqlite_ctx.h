/**
 * @file sqlite_ctx.h
 * @brief SQLite plugin context — handle table + error reporting.
 */
#ifndef SQLITE_CTX_H
#define SQLITE_CTX_H

#include "exprtk.h"
#include "sqlite_provider.h"
#include "salts_buffer.h"
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SQLITE_MAX_HANDLES 16

typedef struct {
    sqlite3 *db;
    char error_msg[256];
} sqlite_handle_t;

typedef struct {
    sqlite_handle_t *handles[SQLITE_MAX_HANDLES];
} sqlite_ctx_t;

typedef struct {
    sqlite_provider *provider;
    sqlite_ctx_t *ctx; /* provider session; direct access remains for advanced migration-only helpers */
    exprtk_env_t *env;
    mem_pool_t *scratch;
} sqlite_ud_t;

#define SQLITE_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

#define SQLITE_CTX_ERROR(ud, msg) do {                                  \
    (ud)->env->aborted = 1;                                             \
} while (0)

void *sqlite_ctx_create(void);
void sqlite_ctx_destroy(void *ctx);

/* Bind the TurboScript sqlite.* facade to one provider-owned session. */
int sqlite_load_provider(sqlite_provider *provider, void *session,
                         void *env, void *scratch);

#endif /* SQLITE_CTX_H */
