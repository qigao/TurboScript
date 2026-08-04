#include "../src/turbo_script_internal.h"
#include "exprtk_types.h"
#include "tinytest.h"
#include "turbo_fs.h"
#include "turbo_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  turbo_script_ctx_t *ctx;
  const char *script;
  int result;
} script_coro_arg_t;

static unsigned long ts_test_nonce(void) {
  static unsigned long counter = 0;
  return ((unsigned long)time(NULL) << 16) ^ (unsigned long)clock() ^ ++counter;
}

static void ts_test_make_name(char *buf, size_t buf_size, const char *prefix, const char *suffix) {
  const char *tail = suffix ? suffix : "";
  if (!buf || buf_size == 0 || !prefix) return;
  snprintf(buf, buf_size, "%s_%lu%s", prefix, ts_test_nonce(), tail);
}

static exprtk_value_t test_triple_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0};
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = args[0].data.number * 3.0};
}


spec("turbo_script_basics") {
  describe("Memory policy") {
    it("should provide bounded profiles and report usage") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_memory_policy_t policy;
      turbo_script_memory_stats_t stats;

      check_not_null(ctx);
      check_int_eq(turbo_script_memory_policy_init(TURBO_SCRIPT_MEMORY_STREAMING, &policy), 0);
      check_true(policy.max_external_value_bytes > 0);
      check_true(policy.max_external_value_bytes <= policy.max_task_bytes);
      check_true(policy.max_task_bytes <= policy.max_context_bytes);
      check_int_eq(turbo_script_set_memory_policy(ctx, &policy), 0);
      check_int_eq(turbo_script_run(ctx, "memory_policy_probe = \"ok\";"), 0);
      check_int_eq(turbo_script_get_memory_stats(ctx, &stats), 0);
      check_true(stats.context_bytes > 0);
      check_true(stats.peak_context_bytes >= stats.context_bytes);

      policy.max_external_value_bytes = 0;
      check_int_eq(turbo_script_set_memory_policy(ctx, &policy), -1);
      turbo_script_free(ctx);
    }

    it("should count retained closure snapshots in context_bytes") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_memory_stats_t before;
      turbo_script_memory_stats_t after;

      check_not_null(ctx);
      check_int_eq(turbo_script_get_memory_stats(ctx, &before), 0);
      /* Each iteration creates a closure that captures a fresh 1 KiB string;
       * the captured snapshot is retained by the root env closure chain. */
      check_int_eq(turbo_script_run(ctx, "for (i = 0; i < 50; i += 1) { txt = repeat(\"x\", 1024); f = (x) => x + txt; } result = 1;"), 0);
      check_int_eq(turbo_script_get_memory_stats(ctx, &after), 0);
      check_true(after.context_bytes > before.context_bytes + 1024);
      turbo_script_free(ctx);
    }
  }

  describe("Basics") {
    it("should execute math scripts") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check_int_eq(turbo_script_run(ctx, " x = 10; y = x * 2;"), 0);
      check_float_eq(ts_get_num(ctx, "y"), 20.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should support prefixed binding helpers without breaking old ones") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      ts_bind_num(ctx, "x", 7.0);
      check_float_eq(ts_get_num(ctx, "x"), 7.0, 0.001);
      ts_bind_num(ctx, "x", 9.0);
      check_float_eq(ts_get_num(ctx, "x"), 9.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should expose a stable version string") {
      check_str_eq(turbo_script_version(), TURBO_SCRIPT_VERSION_STRING);
    }

    it("should expose host map and borrowed list values") {
      exprtk_value_t items[2] = {
          {EXPRTK_VAL_NUMBER, .data.number = 3.0},
          {EXPRTK_VAL_NUMBER, .data.number = 5.0},
      };
      exprtk_value_t list = turbo_script_value_list_borrowed(items, 2);
      exprtk_value_t map = turbo_script_value_map();
      turbo_script_value_map_iterator_t iterator;
      const char *key = NULL;
      exprtk_value_t entry;

      check_int_eq(list.type, EXPRTK_VAL_LIST);
      check_ptr_eq(list.data.list.items, items);
      check_size_eq(list.data.list.count, 2);
      check_int_eq(list.data.list.heap_owned, 0);

      turbo_script_value_map_set(&map, "values", list);
      iterator = turbo_script_value_map_iter_begin(&map);
      check_int_eq(turbo_script_value_map_iter_next(&iterator, &key, &entry), 1);
      check_str_eq(key, "values");
      check_int_eq(entry.type, EXPRTK_VAL_LIST);
      check_size_eq(entry.data.list.count, 2);
      check_float_eq(entry.data.list.items[1].data.number, 5.0, 0.001);
      check_int_eq(turbo_script_value_map_iter_next(&iterator, &key, &entry), 0);

      exprtk_map_free(&map);
    }

    it("should reuse parsed ast for repeated runs of the same script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      exprtk_node_t *first_expr = NULL;
      exprtk_node_t *assign = NULL;
      check_not_null(ctx);

      check_int_eq(turbo_script_run(ctx, "x = 1; y = x + 2;"), 0);
      first_expr = ctx->expr;
      check_not_null(first_expr);

      check_int_eq(turbo_script_run(ctx, "x = 1; y = x + 2;"), 0);
      check_ptr_eq(ctx->expr, first_expr);
      check_float_eq(ts_get_num(ctx, "y"), 3.0, 0.001);

      check_int_eq(turbo_script_run(ctx, "x = 2; y = x + 2;"), 0);
      check_float_eq(ts_get_num(ctx, "y"), 4.0, 0.001);
      check_str_eq(ctx->expr_source, "x = 2; y = x + 2;");
      check_int_eq(ctx->expr->type, EXPRTK_NODE_BLOCK);
      check_int_eq((int)ctx->expr->data.block.count, 2);
      assign = ctx->expr->data.block.statements[0];
      check_not_null(assign);
      check_int_eq(assign->type, EXPRTK_NODE_ASSIGNMENT);
      check_not_null(assign->data.assignment.value);
      check(assign->data.assignment.value->type == EXPRTK_NODE_NUMBER ||
            assign->data.assignment.value->type == EXPRTK_NODE_INTEGER);
      if (assign->data.assignment.value->type == EXPRTK_NODE_INTEGER) {
        check_int_eq((int)assign->data.assignment.value->data.integer, 2);
      } else {
        check_float_eq(assign->data.assignment.value->data.number, 2.0, 0.001);
      }

      turbo_script_free(ctx);
    }

    it("should support the var keyword") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run(ctx, " var "
                                         "a = 123; var b = 456; var c = a + b;"),
                   0);
      check_float_eq(ts_get_num(ctx, "c"), 579.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support complex expressions and multiple assignments") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = " a = 5; "
                           "b = 10; "
                           "c = (a + b) * 2; "
                           "d = c / 3;";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "c"), 30.0, 0.001);
      check_float_eq(ts_get_num(ctx, "d"), 10.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should persist variables across multiple runs") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run(ctx, " x = 100;"), 0);
      check_int_eq(turbo_script_run(ctx, " y = x + 50;"), 0);
      check_float_eq(ts_get_num(ctx, "y"), 150.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support simple control flow") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = " sum = 0; "
                           "i = 0; "
                           "while (i < 5) { "
                           "  sum = sum + i; "
                           "  i = i + 1; "
                           "}";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "sum"), 10.0, 0.001); // 0+1+2+3+4
      turbo_script_free(ctx);
    }
  }

  describe("DateTime") {
    it("should support date parsing and now()") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      // 2024-01-01 12:00:00 UTC is 1704110400
      int res = turbo_script_run(ctx, "t = date(\"2024-01-01 12:00:00\");");
      if (res != 0) printf("DateTime Parse Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "t"), 1704110400.0, 1.0);

      check_int_eq(turbo_script_run(ctx, "curr = now();"), 0);
      check_float_gt(ts_get_num(ctx, "curr"), 1700000000.0);

      turbo_script_free(ctx);
    }
  }

  describe("String Utilities") {
    it("should support string conversions and tokens") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var s = \"123.45\";"
                           "var n = str_to_num(s);"
                           "var s2 = num_to_str(n);"
                           "var data = \"apple,banana,cherry\";"
                           "var count = str_count(data, \",\");"
                           "var item = str_token(data, \",\", 1);";

      int res_str = turbo_script_run(ctx, script);
      if (res_str != 0) printf("String Utils Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res_str, 0);
      check_float_eq(ts_get_num(ctx, "n"), 123.45, 0.01);
      check_float_eq(ts_get_num(ctx, "count"), 3.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("File System") {
    it("should support file operations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      // 1. Write
      check_int_eq(turbo_script_run(ctx, "res "
                                         "= write_file(\"test_ts.txt\", \"Hello Turbo Script!\");"),
                   0);
      double write_res = ts_get_num(ctx, "res");
      printf("DEBUG: file_write res = %g\n", write_res);
      check_float_eq(write_res, 0.0, 0.1);

      // 2. Exists
      check_int_eq(turbo_script_run(ctx, ""
                                         "exists = file_exists(\"test_ts.txt\");"),
                   0);
      double exists_res = ts_get_num(ctx, "exists");
      printf("DEBUG: file_exists res = %g\n", exists_res);
      check_float_eq(exists_res, 1.0, 0.1);

      // 3. Read
      check_int_eq(turbo_script_run(ctx, "data "
                                         "= read_file(\"test_ts.txt\");"),
                   0);
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "Hello Turbo Script!");

      // 4. Remove
      check_int_eq(turbo_script_run(ctx, "del "
                                         "= file_remove(\"test_ts.txt\");"),
                   0);
      check_float_eq(ts_get_num(ctx, "del"), 0.0, 0.1);

      // 5. Not exists
      check_int_eq(turbo_script_run(ctx, ""
                                         "exists2 = file_exists(\"test_ts.txt\");"),
                   0);
      check_float_eq(ts_get_num(ctx, "exists2"), 0.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Vector Functions") {
    it("should support split, avg, len, sum, min, max") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = " data = \"10,20,30,40,50\";"
                           "v = split(data, \",\");"
                           "a = vec.avg(v);"
                           "l = vec.len(v);"
                           "s = vec.sum(v);"
                           "mi = vec.min(v);"
                           "ma = vec.max(v);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "a"), 30.0, 0.1);
      check_float_eq(ts_get_num(ctx, "l"), 5.0, 0.1);
      check_float_eq(ts_get_num(ctx, "s"), 150.0, 0.1);
      check_float_eq(ts_get_num(ctx, "mi"), 10.0, 0.1);
      check_float_eq(ts_get_num(ctx, "ma"), 50.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Error Handling") {
    it("should report parse errors") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run(ctx, "x = 10 + * 5"), -1);
      check_not_null(turbo_script_get_error(ctx));
      check_int_eq((int)turbo_script_get_error_code(ctx), (int)TURBO_SCRIPT_ERROR_PARSE);
      check_not_null(strstr(turbo_script_get_error(ctx), "line 1"));
      check_not_null(strstr(turbo_script_get_error(ctx), "near '*'"));
      turbo_script_free(ctx);
    }
  }

}
