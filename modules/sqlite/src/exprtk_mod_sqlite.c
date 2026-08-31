/**
 * @file exprtk_mod_sqlite.c
 * @brief SQLite module for TurboScript — sqlite.* functions + handle management.
 */
#include "sqlite_ctx.h"
#include <turbostl/vec.h>
#include <math.h>
#include <stdint.h>

typedef struct {
  int64_t id;
  double score;
  double vector_score;
  double fts_score;
  char *source;
  size_t source_len;
  char *text;
  size_t text_len;
} sqlite_rag_result_t;

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

static int sqlite_arg_int(exprtk_value_t value, int *out) {
  if (!out) return 0;
  if (value.type == EXPRTK_VAL_INTEGER) {
    *out = (int)value.data.integer;
    return 1;
  }
  if (value.type == EXPRTK_VAL_NUMBER) {
    *out = (int)value.data.number;
    return 1;
  }
  return 0;
}

static int64_t sqlite_arg_i64(exprtk_value_t value) {
  if (value.type == EXPRTK_VAL_INTEGER) return value.data.integer;
  if (value.type == EXPRTK_VAL_NUMBER) return (int64_t)value.data.number;
  return 0;
}

static double sqlite_arg_num(exprtk_value_t value, double fallback) {
  if (value.type == EXPRTK_VAL_INTEGER) return (double)value.data.integer;
  if (value.type == EXPRTK_VAL_NUMBER) return value.data.number;
  return fallback;
}

static char *sqlite_arena_cstr(mem_pool_t *arena, vstr value) {
  char *buf = mem_alloc(arena, value.len + 1);
  if (!buf) return NULL;
  memcpy(buf, value.data, value.len);
  buf[value.len] = '\0';
  return buf;
}

static int sqlite_copy_text(mem_pool_t *pool, const unsigned char *text,
                            char **out, size_t *out_len) {
  size_t len = text ? strlen((const char *)text) : 0;
  char *copy;
  if (!pool || !out || !out_len) return 0;
  copy = mem_alloc(pool, len + 1);
  if (!copy) return 0;
  if (len) memcpy(copy, text, len);
  copy[len] = '\0';
  *out = copy;
  *out_len = len;
  return 1;
}

static exprtk_value_t sqlite_make_string(exprtk_env_t *env, const char *text, size_t len) {
  exprtk_value_t value = SQLITE_ZERO;
  if (!env) return SQLITE_ZERO;
  if (exprtk_value_copy_to_env(
          exprtk_val_str(vstr_from_buf((char *)(text ? text : ""), len)), env,
          &value) != 0)
    return SQLITE_ZERO;
  return value;
}

static int sqlite_valid_ident(const char *name) {
  size_t i;
  if (!name || !name[0]) return 0;
  if (!((name[0] >= 'A' && name[0] <= 'Z') ||
        (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) {
    return 0;
  }
  for (i = 1; name[i]; i++) {
    if (!((name[i] >= 'A' && name[i] <= 'Z') ||
          (name[i] >= 'a' && name[i] <= 'z') ||
          (name[i] >= '0' && name[i] <= '9') || name[i] == '_')) {
      return 0;
    }
  }
  return i < 96;
}

static void sqlite_set_error(sqlite_handle_t *h, const char *msg) {
  if (!h) return;
  if (!msg) msg = "";
  strncpy(h->error_msg, msg, sizeof(h->error_msg) - 1);
  h->error_msg[sizeof(h->error_msg) - 1] = '\0';
}

static int sqlite_exec_sql(sqlite_handle_t *h, const char *sql) {
  char *err_msg = NULL;
  int rc;
  if (!h || !h->db || !sql) return SQLITE_ERROR;
  rc = sqlite3_exec(h->db, sql, NULL, NULL, &err_msg);
  if (rc != SQLITE_OK) {
    sqlite_set_error(h, err_msg ? err_msg : sqlite3_errmsg(h->db));
    if (err_msg) sqlite3_free(err_msg);
  }
  return rc;
}

static int sqlite_value_as_vector(exprtk_value_t value, const double **out, size_t *out_count) {
  if (!out || !out_count) return 0;
  if (value.type == EXPRTK_VAL_VECTOR) {
    *out = value.data.vector.data;
    *out_count = value.data.vector.size;
    return *out != NULL || *out_count == 0;
  }
  if (value.type == EXPRTK_VAL_BYTES) {
    if (value.data.bytes.len % sizeof(double) != 0) return 0;
    *out = (const double *)value.data.bytes.data;
    *out_count = value.data.bytes.len / sizeof(double);
    return 1;
  }
  return 0;
}

static double sqlite_cosine_raw(const double *a, size_t an,
                                const double *b, size_t bn) {
  double dot = 0.0;
  double na = 0.0;
  double nb = 0.0;
  size_t i;
  if (!a || !b || an == 0 || an != bn) return 0.0;
  for (i = 0; i < an; i++) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  if (na <= 0.0 || nb <= 0.0) return 0.0;
  return dot / (sqrt(na) * sqrt(nb));
}

static exprtk_value_t sqlite_rag_result_to_object(sqlite_ud_t *ud,
                                                  const sqlite_rag_result_t *row) {
  exprtk_value_t obj = exprtk_val_object();
  exprtk_value_t source;
  exprtk_value_t text;
  exprtk_map_set(&obj, "id", exprtk_val_int(row->id));
  exprtk_map_set(&obj, "score", exprtk_val_num(row->score));
  exprtk_map_set(&obj, "vector_score", exprtk_val_num(row->vector_score));
  exprtk_map_set(&obj, "fts_score", exprtk_val_num(row->fts_score));
  source = sqlite_make_string(ud->env, row->source, row->source_len);
  text = sqlite_make_string(ud->env, row->text, row->text_len);
  exprtk_map_set(&obj, "source", source);
  exprtk_map_set(&obj, "text", text);
  exprtk_value_destroy(&source);
  exprtk_value_destroy(&text);
  return obj;
}

static void sqlite_top_insert(sqlite_rag_result_t *top, size_t *top_count, size_t top_k,
                              sqlite_rag_result_t row) {
  size_t pos;
  if (!top || !top_count || top_k == 0) return;
  if (*top_count < top_k) {
    pos = (*top_count)++;
  } else if (row.score <= top[*top_count - 1].score) {
    return;
  } else {
    pos = *top_count - 1;
  }
  top[pos] = row;
  while (pos > 0 && top[pos].score > top[pos - 1].score) {
    sqlite_rag_result_t tmp = top[pos - 1];
    top[pos - 1] = top[pos];
    top[pos] = tmp;
    pos--;
  }
}

/* == API functions ======================================================== */

static exprtk_value_t fn_sqlite_open(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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

static exprtk_value_t fn_sqlite_close(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    SQLITE_CTX_ERROR(ud, "sqlite.close: expected number handle");
    return SQLITE_ZERO;
  }
  sqlite_handle_free(ud->ctx, (int)args[0].data.number);
  return SQLITE_ZERO;
}

static exprtk_value_t fn_sqlite_exec(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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

static exprtk_value_t fn_sqlite_query_col(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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

  size_t element_limit = ud->env->max_external_value_bytes / sizeof(double);
  size_t initial_capacity = element_limit < 64 ? element_limit : 64;
  vec_t data = {0};
  if (element_limit == 0 ||
      vec_init_bytes(&data, sizeof(double), _Alignof(double), element_limit) != STL_OK ||
      vec_reserve(&data, initial_capacity) != STL_OK) {
    if (data.data) vec_destroy(&data);
    sqlite3_finalize(stmt);
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: OOM");
    return SQLITE_ZERO;
  }

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    double element = sqlite3_column_double(stmt, col_idx);
    if (vec_push(&data, &element) != STL_OK) {
      vec_destroy(&data);
      sqlite3_finalize(stmt);
      SQLITE_CTX_ERROR(ud, "sqlite.query_col: OOM");
      return SQLITE_ZERO;
    }
  }

  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    vec_destroy(&data);
    strncpy(h->error_msg, sqlite3_errmsg(h->db), sizeof(h->error_msg) - 1);
    h->error_msg[sizeof(h->error_msg) - 1] = '\0';
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: step failed");
    return SQLITE_ZERO;
  }

  exprtk_value_t result = SQLITE_ZERO;
  if (exprtk_value_copy_to_env(
          exprtk_val_vec((double *)data.data, data.size), ud->env, &result) != 0) {
    vec_destroy(&data);
    SQLITE_CTX_ERROR(ud, "sqlite.query_col: OOM");
    return SQLITE_ZERO;
  }
  vec_destroy(&data);
  return result;
}

static exprtk_value_t fn_sqlite_query_scalar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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

static exprtk_value_t fn_sqlite_error(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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

  exprtk_value_t value;
  if (exprtk_value_copy_to_env(exprtk_val_str(vstr_from_buf(h->error_msg, len)),
                               ud->env, &value) != 0)
    return SQLITE_ZERO;
  return value;
}

static exprtk_value_t fn_sqlite_fts5_available(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  (void)args;
  (void)user_data;
  if (argc != 0) return SQLITE_ZERO;
  return exprtk_val_num(sqlite3_compileoption_used("ENABLE_FTS5") ? 1.0 : 0.0);
}

static exprtk_value_t fn_sqlite_vec_blob(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  const double *vec;
  size_t n;
  size_t len;
  exprtk_value_t result = SQLITE_ZERO;
  if (argc != 1 || !sqlite_value_as_vector(args[0], &vec, &n) || args[0].type != EXPRTK_VAL_VECTOR) {
    SQLITE_CTX_ERROR(ud, "sqlite.vec_blob: expected vector");
    return SQLITE_ZERO;
  }
  len = n * sizeof(double);
  if (exprtk_value_copy_to_env(
          exprtk_val_bytes(vstr_from_buf((char *)vec, len)), ud->env, &result) != 0) {
    SQLITE_CTX_ERROR(ud, "sqlite.vec_blob: OOM");
    return SQLITE_ZERO;
  }
  return result;
}

static exprtk_value_t fn_sqlite_vec_from_blob(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  size_t n;
  exprtk_value_t result = SQLITE_ZERO;
  if (argc != 1 || args[0].type != EXPRTK_VAL_BYTES ||
      args[0].data.bytes.len % sizeof(double) != 0) {
    SQLITE_CTX_ERROR(ud, "sqlite.vec_from_blob: expected embedding blob");
    return SQLITE_ZERO;
  }
  n = args[0].data.bytes.len / sizeof(double);
  if (exprtk_value_copy_to_env(
          exprtk_val_vec((double *)args[0].data.bytes.data, n), ud->env, &result) != 0) {
    SQLITE_CTX_ERROR(ud, "sqlite.vec_from_blob: OOM");
    return SQLITE_ZERO;
  }
  return result;
}

static exprtk_value_t fn_sqlite_vec_cosine(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  const double *a;
  const double *b;
  size_t an;
  size_t bn;
  if (argc != 2 || !sqlite_value_as_vector(args[0], &a, &an) ||
      !sqlite_value_as_vector(args[1], &b, &bn)) {
    SQLITE_CTX_ERROR(ud, "sqlite.vec_cosine: expected two vectors or embedding blobs");
    return SQLITE_ZERO;
  }
  return exprtk_val_num(sqlite_cosine_raw(a, an, b, bn));
}

static exprtk_value_t fn_sqlite_embedding_init(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  sqlite_handle_t *h;
  char *table;
  char sql[256];
  if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_init: expected (db, table)");
    return SQLITE_ZERO;
  }
  h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  table = sqlite_arena_cstr(ud->scratch, args[1].data.string);
  if (!h || !h->db || !sqlite_valid_ident(table)) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_init: invalid handle or table");
    return SQLITE_ZERO;
  }
  snprintf(sql, sizeof(sql),
           "CREATE TABLE IF NOT EXISTS %s ("
           "id INTEGER PRIMARY KEY, dim INTEGER NOT NULL, embedding BLOB NOT NULL)",
           table);
  return exprtk_val_num(sqlite_exec_sql(h, sql) == SQLITE_OK ? 1.0 : 0.0);
}

static exprtk_value_t fn_sqlite_embedding_put(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  sqlite_handle_t *h;
  sqlite3_stmt *stmt = NULL;
  char *table;
  char sql[256];
  const double *vec;
  size_t n;
  int rc;
  if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING ||
      !sqlite_value_as_vector(args[3], &vec, &n) || args[3].type != EXPRTK_VAL_VECTOR) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_put: expected (db, table, id, vector)");
    return SQLITE_ZERO;
  }
  h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  table = sqlite_arena_cstr(ud->scratch, args[1].data.string);
  if (!h || !h->db || !sqlite_valid_ident(table)) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_put: invalid handle or table");
    return SQLITE_ZERO;
  }
  snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO %s (id, dim, embedding) VALUES (?, ?, ?)", table);
  rc = sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite_set_error(h, sqlite3_errmsg(h->db));
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_put: prepare failed");
    return SQLITE_ZERO;
  }
  sqlite3_bind_int64(stmt, 1, sqlite_arg_i64(args[2]));
  sqlite3_bind_int(stmt, 2, (int)n);
  sqlite3_bind_blob(stmt, 3, vec, (int)(n * sizeof(double)), SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    sqlite_set_error(h, sqlite3_errmsg(h->db));
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_put: insert failed");
    return SQLITE_ZERO;
  }
  return exprtk_val_num(1.0);
}

static exprtk_value_t fn_sqlite_embedding_search(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  sqlite_handle_t *h;
  sqlite3_stmt *stmt = NULL;
  char *table;
  char sql[256];
  const double *query;
  size_t query_n;
  int top_k;
  sqlite_rag_result_t *top;
  size_t top_count = 0;
  exprtk_value_t list = exprtk_val_list_empty();
  int rc;
  size_t i;

  if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING ||
      !sqlite_value_as_vector(args[2], &query, &query_n) || !sqlite_arg_int(args[3], &top_k) ||
      top_k <= 0) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_search: expected (db, table, query_vector, top_k)");
    return list;
  }
  h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  table = sqlite_arena_cstr(ud->scratch, args[1].data.string);
  if (!h || !h->db || !sqlite_valid_ident(table)) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_search: invalid handle or table");
    return list;
  }
  top = calloc((size_t)top_k, sizeof(*top));
  if (!top) {
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_search: OOM");
    return list;
  }
  snprintf(sql, sizeof(sql), "SELECT id, dim, embedding FROM %s", table);
  rc = sqlite3_prepare_v2(h->db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite_set_error(h, sqlite3_errmsg(h->db));
    free(top);
    SQLITE_CTX_ERROR(ud, "sqlite.embedding_search: prepare failed");
    return list;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int64_t id = sqlite3_column_int64(stmt, 0);
    int dim = sqlite3_column_int(stmt, 1);
    const void *blob = sqlite3_column_blob(stmt, 2);
    int bytes = sqlite3_column_bytes(stmt, 2);
    if (dim > 0 && (size_t)dim == query_n && bytes == dim * (int)sizeof(double) && blob) {
      sqlite_rag_result_t row;
      memset(&row, 0, sizeof(row));
      row.id = id;
      row.vector_score = sqlite_cosine_raw(query, query_n, (const double *)blob, (size_t)dim);
      row.score = row.vector_score;
      sqlite_top_insert(top, &top_count, (size_t)top_k, row);
    }
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) sqlite_set_error(h, sqlite3_errmsg(h->db));
  for (i = 0; i < top_count; i++) {
    exprtk_value_t obj = exprtk_val_object();
    exprtk_map_set(&obj, "id", exprtk_val_int(top[i].id));
    exprtk_map_set(&obj, "score", exprtk_val_num(top[i].score));
    exprtk_list_push(&list, obj);
  }
  free(top);
  return list;
}

static exprtk_value_t fn_sqlite_rag_init(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  sqlite_handle_t *h;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_init: expected db");
    return SQLITE_ZERO;
  }
  h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h || !h->db) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_init: invalid handle");
    return SQLITE_ZERO;
  }
  if (!sqlite3_compileoption_used("ENABLE_FTS5")) {
    sqlite_set_error(h, "sqlite.rag_init: SQLite was not built with SQLITE_ENABLE_FTS5");
    return SQLITE_ZERO;
  }
  if (sqlite_exec_sql(h, "CREATE TABLE IF NOT EXISTS rag_docs ("
                         "id INTEGER PRIMARY KEY, source TEXT, text TEXT NOT NULL)") != SQLITE_OK)
    return SQLITE_ZERO;
  if (sqlite_exec_sql(h, "CREATE VIRTUAL TABLE IF NOT EXISTS rag_fts USING fts5(text)") != SQLITE_OK)
    return SQLITE_ZERO;
  if (sqlite_exec_sql(h, "CREATE TABLE IF NOT EXISTS rag_embeddings ("
                         "doc_id INTEGER PRIMARY KEY, dim INTEGER NOT NULL, embedding BLOB NOT NULL)") != SQLITE_OK)
    return SQLITE_ZERO;
  return exprtk_val_num(1.0);
}

static exprtk_value_t fn_sqlite_rag_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  sqlite_handle_t *h;
  sqlite3_stmt *stmt = NULL;
  int64_t id;
  char *source;
  char *text;
  const double *vec;
  size_t n;
  int rc;
  if (argc != 5 || args[0].type != EXPRTK_VAL_NUMBER || args[2].type != EXPRTK_VAL_STRING ||
      args[3].type != EXPRTK_VAL_STRING || !sqlite_value_as_vector(args[4], &vec, &n) ||
      args[4].type != EXPRTK_VAL_VECTOR) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_add: expected (db, id, source, text, embedding)");
    return SQLITE_ZERO;
  }
  h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h || !h->db) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_add: invalid handle");
    return SQLITE_ZERO;
  }
  id = sqlite_arg_i64(args[1]);
  source = sqlite_arena_cstr(ud->scratch, args[2].data.string);
  text = sqlite_arena_cstr(ud->scratch, args[3].data.string);
  if (!source || !text) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_add: OOM");
    return SQLITE_ZERO;
  }
  if (sqlite_exec_sql(h, "BEGIN IMMEDIATE") != SQLITE_OK) return SQLITE_ZERO;

  rc = sqlite3_prepare_v2(h->db,
      "INSERT OR REPLACE INTO rag_docs (id, source, text) VALUES (?, ?, ?)", -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) goto fail;

  stmt = NULL;
  rc = sqlite3_prepare_v2(h->db, "DELETE FROM rag_fts WHERE rowid = ?", -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) goto fail;

  stmt = NULL;
  rc = sqlite3_prepare_v2(h->db, "INSERT INTO rag_fts (rowid, text) VALUES (?, ?)", -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, text, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) goto fail;

  stmt = NULL;
  rc = sqlite3_prepare_v2(h->db,
      "INSERT OR REPLACE INTO rag_embeddings (doc_id, dim, embedding) VALUES (?, ?, ?)",
      -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, (int)n);
    sqlite3_bind_blob(stmt, 3, vec, (int)(n * sizeof(double)), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) goto fail;

  if (sqlite_exec_sql(h, "COMMIT") != SQLITE_OK) return SQLITE_ZERO;
  return exprtk_val_num(1.0);

fail:
  sqlite_set_error(h, sqlite3_errmsg(h->db));
  sqlite_exec_sql(h, "ROLLBACK");
  SQLITE_CTX_ERROR(ud, "sqlite.rag_add: insert failed");
  return SQLITE_ZERO;
}

static exprtk_value_t fn_sqlite_rag_search(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  sqlite_ud_t *ud = (sqlite_ud_t *)user_data;
  sqlite_handle_t *h;
  sqlite3_stmt *stmt = NULL;
  char *query_text;
  const double *query_vec;
  size_t query_n;
  int top_k;
  double fts_weight = 1.0;
  double vector_weight = 1.0;
  sqlite_rag_result_t *rows = NULL;
  size_t row_count = 0;
  size_t row_cap = 0;
  sqlite_rag_result_t *top = NULL;
  size_t top_count = 0;
  exprtk_value_t list = exprtk_val_list_empty();
  int rc;
  size_t i;

  if ((argc != 4 && argc != 6) || args[0].type != EXPRTK_VAL_NUMBER ||
      args[1].type != EXPRTK_VAL_STRING || !sqlite_value_as_vector(args[2], &query_vec, &query_n) ||
      !sqlite_arg_int(args[3], &top_k) || top_k <= 0) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_search: expected (db, query, embedding, top_k[, fts_weight, vector_weight])");
    return list;
  }
  h = sqlite_handle_get(ud->ctx, (int)args[0].data.number);
  if (!h || !h->db) {
    SQLITE_CTX_ERROR(ud, "sqlite.rag_search: invalid handle");
    return list;
  }
  query_text = sqlite_arena_cstr(ud->scratch, args[1].data.string);
  if (!query_text) return list;
  if (argc == 6) {
    fts_weight = sqlite_arg_num(args[4], 1.0);
    vector_weight = sqlite_arg_num(args[5], 1.0);
  }

  rc = sqlite3_prepare_v2(h->db,
      "SELECT d.id, d.source, d.text, e.dim, e.embedding "
      "FROM rag_docs d JOIN rag_embeddings e ON e.doc_id = d.id",
      -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite_set_error(h, sqlite3_errmsg(h->db));
    return list;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int dim = sqlite3_column_int(stmt, 3);
    const void *blob = sqlite3_column_blob(stmt, 4);
    int bytes = sqlite3_column_bytes(stmt, 4);
    sqlite_rag_result_t row;
    if (row_count >= row_cap) {
      size_t new_cap = row_cap ? row_cap * 2 : 16;
      sqlite_rag_result_t *new_rows = realloc(rows, new_cap * sizeof(*rows));
      if (!new_rows) {
        sqlite3_finalize(stmt);
        free(rows);
        SQLITE_CTX_ERROR(ud, "sqlite.rag_search: OOM");
        return list;
      }
      rows = new_rows;
      row_cap = new_cap;
    }
    memset(&row, 0, sizeof(row));
    row.id = sqlite3_column_int64(stmt, 0);
    sqlite_copy_text(ud->scratch, sqlite3_column_text(stmt, 1), &row.source, &row.source_len);
    sqlite_copy_text(ud->scratch, sqlite3_column_text(stmt, 2), &row.text, &row.text_len);
    if (dim > 0 && (size_t)dim == query_n && bytes == dim * (int)sizeof(double) && blob) {
      row.vector_score = sqlite_cosine_raw(query_vec, query_n, (const double *)blob, (size_t)dim);
    }
    rows[row_count++] = row;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    sqlite_set_error(h, sqlite3_errmsg(h->db));
    free(rows);
    return list;
  }

  if (query_text[0] != '\0' && sqlite3_compileoption_used("ENABLE_FTS5")) {
    rc = sqlite3_prepare_v2(h->db,
        "SELECT rowid, bm25(rag_fts) FROM rag_fts WHERE rag_fts MATCH ?",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, query_text, -1, SQLITE_TRANSIENT);
      while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        double rank = sqlite3_column_double(stmt, 1);
        double score = 1.0 / (1.0 + fabs(rank));
        for (i = 0; i < row_count; i++) {
          if (rows[i].id == id) {
            rows[i].fts_score = score;
            break;
          }
        }
      }
      sqlite3_finalize(stmt);
    } else {
      sqlite_set_error(h, sqlite3_errmsg(h->db));
    }
  }

  top = calloc((size_t)top_k, sizeof(*top));
  if (!top) {
    free(rows);
    SQLITE_CTX_ERROR(ud, "sqlite.rag_search: OOM");
    return list;
  }
  for (i = 0; i < row_count; i++) {
    rows[i].score = vector_weight * rows[i].vector_score + fts_weight * rows[i].fts_score;
    sqlite_top_insert(top, &top_count, (size_t)top_k, rows[i]);
  }
  for (i = 0; i < top_count; i++)
    exprtk_list_push(&list, sqlite_rag_result_to_object(ud, &top[i]));
  free(top);
  free(rows);
  return list;
}

/* == Loader =============================================================== */

void sqlite_load(void *p, void *e, void *s) {
  sqlite_ctx_t *ctx = (sqlite_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  mem_pool_t *scratch = (mem_pool_t *)s;
  if (!ctx || !env || !scratch) return;

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
  exprtk_env_register_func(env, "sqlite.fts5_available", fn_sqlite_fts5_available, ud);
  exprtk_env_register_func(env, "sqlite.vec_blob", fn_sqlite_vec_blob, ud);
  exprtk_env_register_func(env, "sqlite.vec_from_blob", fn_sqlite_vec_from_blob, ud);
  exprtk_env_register_func(env, "sqlite.vec_cosine", fn_sqlite_vec_cosine, ud);
  exprtk_env_register_func(env, "sqlite.embedding_init", fn_sqlite_embedding_init, ud);
  exprtk_env_register_func(env, "sqlite.embedding_put", fn_sqlite_embedding_put, ud);
  exprtk_env_register_func(env, "sqlite.embedding_search", fn_sqlite_embedding_search, ud);
  exprtk_env_register_func(env, "sqlite.rag_init", fn_sqlite_rag_init, ud);
  exprtk_env_register_func(env, "sqlite.rag_add", fn_sqlite_rag_add, ud);
  exprtk_env_register_func(env, "sqlite.rag_search", fn_sqlite_rag_search, ud);
}
