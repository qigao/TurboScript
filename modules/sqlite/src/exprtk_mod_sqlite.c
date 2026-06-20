/**
 * @file exprtk_mod_sqlite.c
 * @brief SQLite module for TurboScript — sqlite.* functions + handle management.
 */
#include "sqlite_ctx.h"

/* == Lifecycle ============================================================ */

void *sqlite_ctx_create(void) {
  return calloc(1, sizeof(sqlite_ctx_t));
}

void sqlite_ctx_destroy(void *p) {
  sqlite_ctx_t *ctx = (sqlite_ctx_t *)p;
  if (!ctx) return;
  for (int i = 0; i < SQLITE_MAX_HANDLES; i++) {
    if (ctx->handles[i]) {
      if (ctx->handles[i]->db)
        sqlite3_close(ctx->handles[i]->db);
      free(ctx->handles[i]);
    }
  }
  free(ctx);
}

/* == Handle management ==================================================== */

static int sqlite_handle_alloc(sqlite_ctx_t *ctx, sqlite3 *db) {
  for (int i = 0; i < SQLITE_MAX_HANDLES; i++) {
    if (!ctx->handles[i]) {
      sqlite_handle_t *h = calloc(1, sizeof(sqlite_handle_t));
      if (!h) return -1;
      h->db = db;
      ctx->handles[i] = h;
      return i;
    }
  }
  return -1;
}

static void sqlite_handle_free(sqlite_ctx_t *ctx, int handle) {
  if (handle < 0 || handle >= SQLITE_MAX_HANDLES) return;
  if (ctx->handles[handle]) {
    if (ctx->handles[handle]->db)
      sqlite3_close(ctx->handles[handle]->db);
    free(ctx->handles[handle]);
    ctx->handles[handle] = NULL;
  }
}

static sqlite_handle_t *sqlite_handle_get(sqlite_ctx_t *ctx, int handle) {
  if (handle < 0 || handle >= SQLITE_MAX_HANDLES) return NULL;
  return ctx->handles[handle];
}

/* == API functions ======================================================== */

static exprtk_value_t fn_sqlite_open(size_t argc, exprtk_value_t *args, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    SQLITE_CTX_ERROR(ud, "sqlite.open: expected string path");
    return SQLITE_ZERO;
  }

  char *path = mem_alloc(ud->scratch, args[0].data.string.len + 1);
  if (!path) {
    SQLITE_CTX_ERROR(ud, "sqlite.open: OOM");
    return SQLITE_ZERO;
  }
  memcpy(path, args[0].data.string.data, args[0].data.string.len);
  path[args[0].data.string.len] = '\0';

  sqlite3 *db = NULL;
  int rc = sqlite3_open(path, &db);
  if (rc != SQLITE_OK) {
    if (db) sqlite3_close(db);
    SQLITE_CTX_ERROR(ud, "sqlite.open: failed to open database");
    return SQLITE_ZERO;
  }

  int handle = sqlite_handle_alloc(ud->ctx, db);
  if (handle < 0) {
    sqlite3_close(db);
    SQLITE_CTX_ERROR(ud, "sqlite.open: too many open handles");
    return SQLITE_ZERO;
  }

  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = (double)handle};
}

static exprtk_value_t fn_sqlite_close(size_t argc, exprtk_value_t *args, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    SQLITE_CTX_ERROR(ud, "sqlite.close: expected number handle");
    return SQLITE_ZERO;
  }
  sqlite_handle_free(ud->ctx, (int)args[0].data.number);
  return SQLITE_ZERO;
}

static exprtk_value_t fn_sqlite_exec(size_t argc, exprtk_value_t *args, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
    SQLITE_CTX_ERROR(ud, "sqlite.exec: expected (number, string)");
    return SQLITE_ZERO;
  }

  sqlite_handle_t *h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h || !h->db) {
    SQLITE_CTX_ERROR(ud, "sqlite.exec: invalid handle");
    return SQLITE_ZERO;
  }

  char *sql = mem_alloc(ud->scratch, args[1].data.string.len + 1);
  if (!sql) {
    SQLITE_CTX_ERROR(ud, "sqlite.exec: OOM");
    return SQLITE_ZERO;
  }
  memcpy(sql, args[1].data.string.data, args[1].data.string.len);
  sql[args[1].data.string.len] = '\0';

  char *err_msg = NULL;
  int rc = sqlite3_exec(h->db, sql, NULL, NULL, &err_msg);
  if (rc != SQLITE_OK) {
    if (err_msg) {
      strncpy(h->error_msg, err_msg, sizeof(h->error_msg) - 1);
      h->error_msg[sizeof(h->error_msg) - 1] = '\0';
      sqlite3_free(err_msg);
    }
    SQLITE_CTX_ERROR(ud, "sqlite.exec: execution failed");
    return SQLITE_ZERO;
  }

  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = (double)sqlite3_changes(h->db)};
}

static exprtk_value_t fn_sqlite_query_col(size_t argc, exprtk_value_t *args, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc < 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: expected (number, string [, number])");
    return SQLITE_ZERO;
  }

  sqlite_handle_t *h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h || !h->db) {
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: invalid handle");
    return SQLITE_ZERO;
  }

  int col_idx = 0;
  if (argc >= 3 && args[2].type == EXPRTK_VAL_NUMBER)
    col_idx = (int)args[2].data.number;

  char *sql = mem_alloc(ud->scratch, args[1].data.string.len + 1);
  if (!sql) {
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: OOM");
    return SQLITE_ZERO;
  }
  memcpy(sql, args[1].data.string.data, args[1].data.string.len);
  sql[args[1].data.string.len] = '\0';

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    strncpy(h->error_msg, sqlite3_errmsg(h->db), sizeof(h->error_msg) - 1);
    h->error_msg[sizeof(h->error_msg) - 1] = '\0';
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: prepare failed");
    return SQLITE_ZERO;
  }

  size_t cap = 64;
  size_t len = 0;
  double *data = mem_alloc(&ud->env->arena, cap * sizeof(double));
  if (!data) {
    sqlite3_finalize(stmt);
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: OOM");
    return SQLITE_ZERO;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (len >= cap) {
      cap *= 2;
      double *new_data = mem_alloc(&ud->env->arena, cap * sizeof(double));
      if (!new_data) {
        sqlite3_finalize(stmt);
        SQLITE_CTX_ERROR(ud, "sqlite.query_col: OOM");
        return SQLITE_ZERO;
      }
      memcpy(new_data, data, len * sizeof(double));
      data = new_data;
    }
    data[len++] = sqlite3_column_double(stmt, col_idx);
  }

  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    strncpy(h->error_msg, sqlite3_errmsg(h->db), sizeof(h->error_msg) - 1);
    h->error_msg[sizeof(h->error_msg) - 1] = '\0';
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: step failed");
    return SQLITE_ZERO;
  }

  return (exprtk_value_t){EXPRTK_VAL_VECTOR, .data.vector = {.data = data, .size = len}};
}

static exprtk_value_t fn_sqlite_query_scalar(size_t argc, exprtk_value_t *args, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
    SQLITE_CTX_ERROR(ud, "sqlite.query_scalar: expected (number, string)");
    return SQLITE_ZERO;
  }

  sqlite_handle_t *h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h || !h->db) {
    SQLITE_CTX_ERROR(ud, "sqlite.query_scalar: invalid handle");
    return SQLITE_ZERO;
  }

  char *sql = mem_alloc(ud->scratch, args[1].data.string.len + 1);
  if (!sql) {
    SQLITE_CTX_ERROR(ud, "sqlite.query_scalar: OOM");
    return SQLITE_ZERO;
  }
  memcpy(sql, args[1].data.string.data, args[1].data.string.len);
  sql[args[1].data.string.len] = '\0';

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    strncpy(h->error_msg, sqlite3_errmsg(h->db), sizeof(h->error_msg) - 1);
    h->error_msg[sizeof(h->error_msg) - 1] = '\0';
    SQLITE_CTX_ERROR(ud, "sqlite.query_scalar: prepare failed");
    return SQLITE_ZERO;
  }

  double result = 0.0;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    result = sqlite3_column_double(stmt, 0);

  sqlite3_finalize(stmt);
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = result};
}

static exprtk_value_t fn_sqlite_error(size_t argc, exprtk_value_t *args, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    SQLITE_CTX_ERROR(ud, "sqlite.error: expected number handle");
    return SQLITE_ZERO;
  }

  sqlite_handle_t *h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h) {
    SQLITE_CTX_ERROR(ud, "sqlite.error: invalid handle");
    return SQLITE_ZERO;
  }

  size_t len = strlen(h->error_msg);
  if (len == 0)
    return SQLITE_ZERO;

  char *buf = mem_alloc(&ud->env->arena, len + 1);
  if (!buf) return SQLITE_ZERO;
  memcpy(buf, h->error_msg, len);
  buf[len] = '\0';

  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = {buf, len}};
}

/* == Loader =============================================================== */

void sqlite_load(void *p, void *e, void *s) {
  sqlite_ctx_t *ctx = (sqlite_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  mem_pool_t *scratch = (mem_pool_t *)s;
  if (!ctx || !env) return;

  sqlite_ud_t *ud = mem_alloc(&env->arena, sizeof(*ud));
  if (!ud) return;
  ud->ctx = ctx;
  ud->env = env;
  ud->scratch = scratch;

  exprtk_env_register_func(env, "sqlite.open", fn_sqlite_open, ud);
  exprtk_env_register_func(env, "sqlite.close", fn_sqlite_close, ud);
  exprtk_env_register_func(env, "sqlite.exec", fn_sqlite_exec, ud);
  exprtk_env_register_func(env, "sqlite.query_col", fn_sqlite_query_col, ud);
  exprtk_env_register_func(env, "sqlite.query_scalar", fn_sqlite_query_scalar, ud);
  exprtk_env_register_func(env, "sqlite.error", fn_sqlite_error, ud);
}
