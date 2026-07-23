/**
 * @file test_sqlite.c
 * @brief Tests for sqlite module — sqlite.* functions via DLL plugin.
 *
 * Loads sqlite_plugin DLL at runtime through ts_plugin_loader,
 * then exercises the registered script functions through exprtk_env.
 */

#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"
#include <stdio.h>
#include <string.h>


/* ── Platform-specific DLL names ──────────────────────────────────── */

#ifdef _WIN32
  #define SQLITE_PLUGIN_DLL "tbs_sqlite.dll"
#else
  #define SQLITE_PLUGIN_DLL "tbs_sqlite.so"
#endif

/* ── Helper: create a minimal environment via DLL ──────────────────── */

typedef struct {
  ts_plugin_handle_t *sqlite_plugin;
  exprtk_env_t env;
  mem_pool_t scratch;
} test_env_t;

static void test_env_init(test_env_t *t) {
  memset(t, 0, sizeof(*t));
  exprtk_env_init(&t->env);
  mem_init(&t->scratch, 65536);
  t->sqlite_plugin = ts_plugin_load(SQLITE_PLUGIN_DLL);
  if (t->sqlite_plugin)
    ts_plugin_init(t->sqlite_plugin, &t->env, &t->scratch);
}

static void test_env_free(test_env_t *t) {
  ts_plugin_unload(t->sqlite_plugin);
  exprtk_env_free(&t->env);
  mem_destroy(&t->scratch);
}

/* Helper: call a registered function by name */
static exprtk_value_t call_fn(test_env_t *t, const char *name, size_t argc, exprtk_value_t *args) {
  exprtk_func_t *fn = t->env.funcs;
  while (fn) {
    if (strcmp(fn->name, name) == 0 && !fn->is_script) {
      return fn->data.native.fn(argc, args, &t->env, fn->data.native.user_data);
    }
    fn = fn->next;
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -999.0};
}

/* Helper: make a string value in env arena */
static exprtk_value_t make_str(test_env_t *t, const char *s) {
  size_t len = strlen(s);
  char *buf = mem_alloc(&t->env.arena, len + 1);
  memcpy(buf, s, len + 1);
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t make_num(double v) {
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = v};
}

static exprtk_value_t make_vec(test_env_t *t, const double *values, size_t n) {
  double *data = mem_alloc(&t->env.arena, n * sizeof(double));
  if (n > 0) memcpy(data, values, n * sizeof(double));
  return exprtk_val_vec(data, n);
}

spec("sqlite_module") {

  /* ── Plugin loading ───────────────────────────────────────────── */

  describe("plugin") {

    it("should load and unload sqlite_plugin via DLL") {
      ts_plugin_handle_t *h = ts_plugin_load(SQLITE_PLUGIN_DLL);
      check_not_null(h);
      check_not_null(h->plugin);
      check_str_eq(h->plugin->name, "sqlite");

      exprtk_env_t env;
      mem_pool_t scratch;
      exprtk_env_init(&env);
      mem_init(&scratch, 4096);
      check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

      ts_plugin_unload(h);
      exprtk_env_free(&env);
      mem_destroy(&scratch);
    }
  }

  /* ── Basic operations ─────────────────────────────────────────── */

  describe("sqlite.open / sqlite.close") {

    it("should open and close an in-memory database") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t handle = call_fn(&t, "sqlite.open", 1, args);
      check_int_eq(handle.type, EXPRTK_VAL_NUMBER);
      check(handle.data.number >= 0.0);

      exprtk_value_t close_args[1] = {handle};
      call_fn(&t, "sqlite.close", 1, close_args);

      test_env_free(&t);
    }
  }

  describe("sqlite.exec") {

    it("should create a table and insert rows") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, args);
      check(db.data.number >= 0.0);

      exprtk_value_t exec_args[2] = {db,
                                     make_str(&t, "CREATE TABLE test (id INTEGER, value REAL)")};
      exprtk_value_t res = call_fn(&t, "sqlite.exec", 2, exec_args);
      check_int_eq(res.type, EXPRTK_VAL_NUMBER);

      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (1, 3.14)");
      res = call_fn(&t, "sqlite.exec", 2, exec_args);
      check_float_eq(res.data.number, 1.0, 0.01);

      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (2, 2.71)");
      res = call_fn(&t, "sqlite.exec", 2, exec_args);
      check_float_eq(res.data.number, 1.0, 0.01);

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);

      test_env_free(&t);
    }
  }

  describe("sqlite.query_scalar") {

    it("should query a single value") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, args);

      exprtk_value_t exec_args[2] = {db,
                                     make_str(&t, "CREATE TABLE test (id INTEGER, value REAL)")};
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (1, 3.14)");
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exprtk_value_t query_args[2] = {db, make_str(&t, "SELECT value FROM test WHERE id=1")};
      exprtk_value_t res = call_fn(&t, "sqlite.query_scalar", 2, query_args);
      check_int_eq(res.type, EXPRTK_VAL_NUMBER);
      check_float_eq(res.data.number, 3.14, 0.01);

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);

      test_env_free(&t);
    }

    it("should query COUNT(*)") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, args);

      exprtk_value_t exec_args[2] = {db, make_str(&t, "CREATE TABLE test (id INTEGER)")};
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (1)");
      call_fn(&t, "sqlite.exec", 2, exec_args);
      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (2)");
      call_fn(&t, "sqlite.exec", 2, exec_args);
      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (3)");
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exprtk_value_t query_args[2] = {db, make_str(&t, "SELECT COUNT(*) FROM test")};
      exprtk_value_t res = call_fn(&t, "sqlite.query_scalar", 2, query_args);
      check_float_eq(res.data.number, 3.0, 0.01);

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);

      test_env_free(&t);
    }
  }

  describe("sqlite.query_col") {

    it("should query a column as vector") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, args);

      exprtk_value_t exec_args[2] = {db,
                                     make_str(&t, "CREATE TABLE test (id INTEGER, value REAL)")};
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (1, 10.0)");
      call_fn(&t, "sqlite.exec", 2, exec_args);
      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (2, 20.0)");
      call_fn(&t, "sqlite.exec", 2, exec_args);
      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (3, 30.0)");
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exprtk_value_t query_args[3] = {db, make_str(&t, "SELECT value FROM test ORDER BY id"),
                                      make_num(0)};
      exprtk_value_t res = call_fn(&t, "sqlite.query_col", 3, query_args);
      check_int_eq(res.type, EXPRTK_VAL_VECTOR);
      check_int_eq(res.data.vector.size, 3);
      check_float_eq(res.data.vector.data[0], 10.0, 0.01);
      check_float_eq(res.data.vector.data[1], 20.0, 0.01);
      check_float_eq(res.data.vector.data[2], 30.0, 0.01);

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);

      test_env_free(&t);
    }

    it("should query multiple columns by index") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, args);

      exprtk_value_t exec_args[2] = {db, make_str(&t, "CREATE TABLE test (a REAL, b REAL)")};
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (1.0, 2.0)");
      call_fn(&t, "sqlite.exec", 2, exec_args);
      exec_args[1] = make_str(&t, "INSERT INTO test VALUES (3.0, 4.0)");
      call_fn(&t, "sqlite.exec", 2, exec_args);

      exprtk_value_t query_args[3] = {db, make_str(&t, "SELECT a, b FROM test ORDER BY a"),
                                      make_num(0)};
      exprtk_value_t col_a = call_fn(&t, "sqlite.query_col", 3, query_args);
      check_int_eq(col_a.data.vector.size, 2);
      check_float_eq(col_a.data.vector.data[0], 1.0, 0.01);
      check_float_eq(col_a.data.vector.data[1], 3.0, 0.01);

      query_args[2] = make_num(1);
      exprtk_value_t col_b = call_fn(&t, "sqlite.query_col", 3, query_args);
      check_int_eq(col_b.data.vector.size, 2);
      check_float_eq(col_b.data.vector.data[0], 2.0, 0.01);
      check_float_eq(col_b.data.vector.data[1], 4.0, 0.01);

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);

      test_env_free(&t);
    }
  }

  describe("sqlite RAG helpers") {
    it("should convert vectors to embedding blobs and compute cosine") {
      test_env_t t;
      double a_data[3] = {1.0, 0.0, 0.0};
      double b_data[3] = {1.0, 0.0, 0.0};
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t a = make_vec(&t, a_data, 3);
      exprtk_value_t b = make_vec(&t, b_data, 3);
      exprtk_value_t args[1] = {a};
      exprtk_value_t blob = call_fn(&t, "sqlite.vec_blob", 1, args);
      check_int_eq(blob.type, EXPRTK_VAL_BYTES);
      check_int_eq(blob.data.bytes.len, 3 * sizeof(double));

      args[0] = blob;
      exprtk_value_t roundtrip = call_fn(&t, "sqlite.vec_from_blob", 1, args);
      check_int_eq(roundtrip.type, EXPRTK_VAL_VECTOR);
      check_int_eq(roundtrip.data.vector.size, 3);
      check_float_eq(roundtrip.data.vector.data[0], 1.0, 0.001);

      exprtk_value_t cos_args[2] = {a, b};
      exprtk_value_t cos = call_fn(&t, "sqlite.vec_cosine", 2, cos_args);
      check_float_eq(cos.data.number, 1.0, 0.001);

      test_env_free(&t);
    }

    it("should store and search embedding vectors") {
      test_env_t t;
      double a_data[3] = {1.0, 0.0, 0.0};
      double b_data[3] = {0.0, 1.0, 0.0};
      double q_data[3] = {0.9, 0.1, 0.0};
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t open_args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, open_args);

      exprtk_value_t init_args[2] = {db, make_str(&t, "embeddings")};
      exprtk_value_t ok = call_fn(&t, "sqlite.embedding_init", 2, init_args);
      check_float_eq(ok.data.number, 1.0, 0.001);

      exprtk_value_t put_args[4] = {db, make_str(&t, "embeddings"), make_num(1), make_vec(&t, a_data, 3)};
      ok = call_fn(&t, "sqlite.embedding_put", 4, put_args);
      check_float_eq(ok.data.number, 1.0, 0.001);
      put_args[2] = make_num(2);
      put_args[3] = make_vec(&t, b_data, 3);
      ok = call_fn(&t, "sqlite.embedding_put", 4, put_args);
      check_float_eq(ok.data.number, 1.0, 0.001);

      exprtk_value_t search_args[4] = {db, make_str(&t, "embeddings"), make_vec(&t, q_data, 3), make_num(2)};
      exprtk_value_t result = call_fn(&t, "sqlite.embedding_search", 4, search_args);
      check_int_eq(result.type, EXPRTK_VAL_LIST);
      check_int_eq(result.data.list.count, 2);
      exprtk_value_t first = exprtk_list_get(&result, 0);
      check_int_eq(first.type, EXPRTK_VAL_OBJECT);
      check_float_eq((double)exprtk_map_get(&first, "id").data.integer, 1.0, 0.001);

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);
      test_env_free(&t);
    }

    it("should expose FTS5 availability and RAG helpers") {
      test_env_t t;
      double a_data[3] = {1.0, 0.0, 0.0};
      double b_data[3] = {0.0, 1.0, 0.0};
      double q_data[3] = {0.95, 0.05, 0.0};
      test_env_init(&t);
      check_not_null(t.sqlite_plugin);

      exprtk_value_t available = call_fn(&t, "sqlite.fts5_available", 0, NULL);
      check_int_eq(available.type, EXPRTK_VAL_NUMBER);

      exprtk_value_t open_args[1] = {make_str(&t, ":memory:")};
      exprtk_value_t db = call_fn(&t, "sqlite.open", 1, open_args);
      exprtk_value_t init_args[1] = {db};
      exprtk_value_t init = call_fn(&t, "sqlite.rag_init", 1, init_args);

      if (available.data.number < 0.5) {
        check_float_eq(init.data.number, 0.0, 0.001);
      } else {
        check_float_eq(init.data.number, 1.0, 0.001);
        exprtk_value_t add_args[5] = {
          db, make_num(1), make_str(&t, "doc1"),
          make_str(&t, "sqlite fts search for retrieval augmented generation"),
          make_vec(&t, a_data, 3)
        };
        exprtk_value_t ok = call_fn(&t, "sqlite.rag_add", 5, add_args);
        check_float_eq(ok.data.number, 1.0, 0.001);
        add_args[1] = make_num(2);
        add_args[2] = make_str(&t, "doc2");
        add_args[3] = make_str(&t, "unrelated finance time series note");
        add_args[4] = make_vec(&t, b_data, 3);
        ok = call_fn(&t, "sqlite.rag_add", 5, add_args);
        check_float_eq(ok.data.number, 1.0, 0.001);

        exprtk_value_t search_args[6] = {
          db, make_str(&t, "sqlite retrieval"),
          make_vec(&t, q_data, 3), make_num(2), make_num(1), make_num(1)
        };
        exprtk_value_t result = call_fn(&t, "sqlite.rag_search", 6, search_args);
        check_int_eq(result.type, EXPRTK_VAL_LIST);
        check(result.data.list.count >= 1);
        exprtk_value_t first = exprtk_list_get(&result, 0);
        check_int_eq(first.type, EXPRTK_VAL_OBJECT);
        check_float_eq((double)exprtk_map_get(&first, "id").data.integer, 1.0, 0.001);
      }

      exprtk_value_t close_args[1] = {db};
      call_fn(&t, "sqlite.close", 1, close_args);
      test_env_free(&t);
    }
  }
}
