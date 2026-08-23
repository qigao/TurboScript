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


spec("turbo_script_io") {
  describe("Module Import: dot notation") {
    it("should import fs module and use dot notation") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_not_null(ctx);

      const char *script = ""
                           "io.write_file(\"test_dot.txt\", \"dot notation works\"); "
                           "data = io.read_file(\"test_dot.txt\"); "
                           "io.file_remove(\"test_dot.txt\"); "
                           "gone = io.file_exists(\"test_dot.txt\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("FS dot Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      const char *data_str = ts_get_str(ctx, "data");
      check_not_null(data_str);
      check(strcmp((data_str), ("dot notation works")) == 0);
      check(fabs((double)(ts_get_num(ctx, "gone")) - (double)(0.0)) <= (double)(0.1));

      turbo_script_free(ctx);
    }

    it("should still support file import with init_bare") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_not_null(ctx);

      turbo_script_run(ctx, ""
                            "write_file(\"utils_dot.tbs\", \"func square(x) { return x * x; };\");");

      const char *script = "import(\"utils_dot.tbs\"); "
                           "res = square(7);";

      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "res")) - (double)(49.0)) <= (double)(0.1));

      turbo_script_run(ctx, "file_remove(\"utils_dot.tbs\");");
      turbo_script_free(ctx);
    }
  }

  describe("Net Plugin (HTTP via Script)") {
    it("should fetch mockhttp.org using http.get from script") {
      /* This test hits a live external service, so it is gated behind
       * TS_NET_TESTS=1. Offline/CI environments stay hermetic; set the
       * variable to opt into the network check. */
      if (getenv("TS_NET_TESTS") == NULL) {
        printf("[SKIPPED] set TS_NET_TESTS=1 to run the network-dependent http.get test\n");
        return;
      }
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = ""
                           "import(\"net\");\n"
                           "var res = http.get(\"https://mockhttp.org\", {timeout: 5000});\n"
                           "var is_ok = (res.status == 200);\n"
                           "var has_headers = (map.size(res.headers) > 0);\n";

      int run_res = turbo_script_run(ctx, script);
      if (run_res != 0) printf("Net Plugin Error: %s\n", turbo_script_get_error(ctx));
      check((run_res) == (0));
      check(fabs((double)(ts_get_num(ctx, "is_ok")) - (double)(1.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "has_headers")) - (double)(1.0)) <= (double)(0.1));

      turbo_script_free(ctx);
    }
  }

  describe("Vector Sort") {
    it("should sort ascending") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = "var v = [3, 1, 4, 1, 5, 9, 2, 6];"
                           "var s = vec.sort(v);"
                           "var first = s[0];"
                           "var last = s[7];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "first")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(9.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should sort descending") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = "var v = [3, 1, 4, 1, 5];"
                           "var s = vec.sort_desc(v);"
                           "var first = s[0];"
                           "var last = s[4];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "first")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(1.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should not modify original vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [3, 1, 2];"
                           "var s = vec.sort(v);"
                           "var orig_first = v[0];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "orig_first")) - (double)(3.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = " var v = [42];"
                           "var s = vec.sort(v);"
                           "var val = s[0];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "val")) - (double)(42.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("Vector Unique") {
    it("should remove duplicates and sort") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [3, 1, 2, 1, 3, 2];"
                           "var u = vec.unique(v);"
                           "var n = vec.len(u);"
                           "var first = u[0];"
                           "var last = u[2];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "first")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(3.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should handle all same values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [5, 5, 5, 5];"
                           "var u = vec.unique(v);"
                           "var n = vec.len(u);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(1.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("String Join") {
    it("should join vector with delimiter") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [1, 2, 3];"
                           "var s = join(v, \",\");";
      check((turbo_script_run(ctx, script)) == (0));
      const char *result = ts_get_str(ctx, "s");
      check_not_null(result);
      check(strcmp((result), ("1,2,3")) == 0);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [42];"
                           "var s = join(v, \"-\");";
      check((turbo_script_run(ctx, script)) == (0));
      const char *result = ts_get_str(ctx, "s");
      check_not_null(result);
      check(strcmp((result), ("42")) == 0);
      turbo_script_free(ctx);
    }
  }

  describe("Structured Text Write") {
    it("should write and read back CSV text with IO functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);

      int r1 = turbo_script_run(ctx, "var data = \"name,age,role\\nAlice,30,dev\\nBob,40,mgr\";"
                                     "var res = io.write_file(\"_test_csv_write.csv\", data);"
                                     "var content = read_file(\"_test_csv_write.csv\");"
                                     "file_remove(\"_test_csv_write.csv\");");
      if (r1 != 0) printf("CSV write Error: %s\n", turbo_script_get_error(ctx));
      check((r1) == (0));
      check(fabs((double)(ts_get_num(ctx, "res")) - (double)(0.0)) <= (double)(0.1));
      const char *content = ts_get_str(ctx, "content");
      check_not_null(content);
      check(strcmp((content), ("name,age,role\nAlice,30,dev\nBob,40,mgr")) == 0);

      turbo_script_free(ctx);
    }
  }

  describe("Vector Reverse") {
    it("should reverse a vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [1, 2, 3, 4, 5];"
                           "var r = vec.reverse(v);"
                           "var first = r[0];"
                           "var last = r[4];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "first")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(1.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [42];"
                           "var r = vec.reverse(v);"
                           "var val = r[0];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "val")) - (double)(42.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should not modify original vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [1, 2, 3];"
                           "var r = vec.reverse(v);"
                           "var orig = v[0];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "orig")) - (double)(1.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("Vector Concat") {
    it("should merge two vectors") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var a = [1, 2, 3];"
                           "var b = [4, 5];"
                           "var c = vec.concat(a, b);"
                           "var n = vec.len(c);"
                           "var last = c[4];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(5.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should handle one empty vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      ts_bind_vec(ctx, "a", (const double[]){1.0, 2.0}, 2);
      const char *script = " var b = vec.range(0);"
                           "var c = vec.concat(a, b);"
                           "var n = vec.len(c);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(2.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("Vector Range") {
    it("should generate vec.range(5)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = vec.range(5);"
                           "var n = vec.len(v);"
                           "var first = v[0];"
                           "var last = v[4];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "first")) - (double)(0.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(4.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should generate vec.range(2, 5)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = vec.range(2, 5);"
                           "var n = vec.len(v);"
                           "var first = v[0];"
                           "var last = v[2];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "first")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "last")) - (double)(4.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should return empty for vec.range(0)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = "var v = vec.range(0);"
                           "var n = vec.len(v);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(0.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("Vector Cumsum") {
    it("should compute cumulative sum") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [1, 2, 3];"
                           "var cs = vec.cumsum(v);"
                           "var a = cs[0];"
                           "var b = cs[1];"
                           "var c = cs[2];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(3.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(6.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [42];"
                           "var cs = vec.cumsum(v);"
                           "var val = cs[0];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "val")) - (double)(42.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("Vector Diff") {
    it("should compute first-order differences") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [1, 3, 6];"
                           "var d = vec.diff(v);"
                           "var n = vec.len(d);"
                           "var a = d[0];"
                           "var b = d[1];";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "b")) - (double)(3.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should return empty for single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [42];"
                           "var d = vec.diff(v);"
                           "var n = vec.len(d);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(0.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("Vector Find") {
    it("should return index when found") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [10, 20, 30, 40];"
                           "var idx = vec.find(v, 30);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "idx")) - (double)(2.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should return -1 when not found") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [10, 20, 30];"
                           "var idx = vec.find(v, 99);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "idx")) - (double)(-1.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("String Format") {
    it("should format with %%s and %%d") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var s = "
                           "format(\"%s is %d\", \"age\", 30);";
      check((turbo_script_run(ctx, script)) == (0));
      const char *val = ts_get_str(ctx, "s");
      check_not_null(val);
      check(strcmp((val), ("age is 30")) == 0);
      turbo_script_free(ctx);
    }

    it("should format with %%g") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var s = format(\"pi=%g\", 3.14);";
      check((turbo_script_run(ctx, script)) == (0));
      const char *val = ts_get_str(ctx, "s");
      check_not_null(val);
      check(strcmp((val), ("pi=3.14")) == 0);
      turbo_script_free(ctx);
    }

    it("should escape %% as literal percent") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var s = format(\"%d%%\", 100);";
      check((turbo_script_run(ctx, script)) == (0));
      const char *val = ts_get_str(ctx, "s");
      check_not_null(val);
      check(strcmp((val), ("100%")) == 0);
      turbo_script_free(ctx);
    }
  }

  describe("Pipeline Primitives") {
    it("should support vector map filter reduce take drop and lag") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var out = [1, -2, 3, 0] |> filter(x => x > 0) |> map(x => x * 10); "
          "var out_n = vec.len(out); "
          "var out_a = out[0]; "
          "var out_b = out[1]; "
          "var total = [1, 2, 3, 4] |> reduce(0, (acc, x) => acc + x); "
          "var t = take([5, 6, 7], 2); "
          "var d = drop([5, 6, 7], 1); "
          "var l = lag([10, 20, 30], 1, -1); "
          "var tn = vec.len(t); "
          "var dn = vec.len(d); "
          "var ta = t[0]; "
          "var db = d[1]; "
          "var la = l[0]; "
          "var lb = l[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Pipeline primitives Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "out_n")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "out_a")) - (double)(10.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "out_b")) - (double)(30.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "total")) - (double)(10.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "tn")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "dn")) - (double)(2.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "ta")) - (double)(5.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "db")) - (double)(7.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "la")) - (double)(-1.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "lb")) - (double)(10.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support Java-style streams over containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var rows = list(map{price:10, qty:2}, map{price:3, qty:5}, map{price:8, qty:1}); "
          "var total = rows.stream()"
          "  .filter(r => r.price > 5)"
          "  .map(r => r.price * r.qty)"
          "  .reduce(0, (acc, v) => acc + v); "
          "var nums = [1, 2, 3, 4].stream().filter(x => x > 2).map(x => x * 10).toList(); "
          "var entries = map{a:1, b:2, c:3}.stream().filter(v => v > 1).collect(); "
          "var line_count = \"a\\nb\\n\".stream().count(); "
          "var plain = map{__ts_stream:1, source:list(1,2)}; "
          "var marker_len = plain.length(); "
          "var score = total + nums.length() + nums[0] + entries.length() + line_count + marker_len;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Java-style container stream Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "total")) - (double)(28.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(67.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support Java-style streams through MIR JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var rows = list(map{price:10, qty:2}, map{price:3, qty:5}, map{price:8, qty:1}); "
          "var total = rows.stream()"
          "  .filter(r => r.price > 5)"
          "  .map(r => r.price * r.qty)"
          "  .reduce(0, (acc, v) => acc + v); "
          "var count = [1, 2, 3, 4].stream().filter(x => x > 2).count(); "
          "var score = total + count;";
      int res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style JIT stream Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "total")) - (double)(28.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(30.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support text streams through MIR JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var line_total = stream.text(\" a \\nbb\\n\\nccc\")"
          "  .lines()"
          "  .map(line => line.trim())"
          "  .filter(line => line.length() > 0)"
          "  .map(line => line.length())"
          "  .reduce(0, (acc, n) => acc + n); "
          "var token_total = stream.text(\"AAPL,MSFT,TSLA\")"
          "  .split(\",\")"
          "  .map(sym => sym.length())"
          "  .reduce(0, (acc, n) => acc + n); "
          "var score = line_total + token_total;";
      int res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style text stream JIT Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "line_total")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "token_total")) - (double)(12.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(18.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }

    it("should support file source streams through MIR JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *line_text = " a \nbb\n\nccc";
      const char *token_text = "AAPL,MSFT,TSLA";
      char line_path[96];
      char token_path[96];
      char script[2048];
      turbo_fs_buf_t buf;
      int res;

      ts_test_make_name(line_path, sizeof(line_path), "_test_stream_file_lines", ".txt");
      ts_test_make_name(token_path, sizeof(token_path), "_test_stream_file_tokens", ".txt");

      buf = turbo_fs_buf_init((char *)line_text, strlen(line_text));
      check((turbo_fs_write_file(line_path, &buf)) == (0));
      buf = turbo_fs_buf_init((char *)token_text, strlen(token_text));
      check((turbo_fs_write_file(token_path, &buf)) == (0));

      snprintf(script, sizeof(script),
          "var line_total = stream.text(io.read_file(\"%s\"))"
          "  .lines()"
          "  .map(line => line.trim())"
          "  .filter(line => line.length() > 0)"
          "  .map(line => line.length())"
          "  .reduce(0, (acc, n) => acc + n); "
          "var token_total = stream.text(io.read_file(\"%s\"))"
          "  .split(\",\")"
          "  .map(sym => sym.length())"
          "  .reduce(0, (acc, n) => acc + n); "
          "var score = line_total + token_total;",
          line_path, token_path);

      res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style file source stream Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "line_total")) - (double)(6.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "token_total")) - (double)(12.0)) <= (double)(0.001));
      check(fabs((double)(ts_get_num(ctx, "score")) - (double)(18.0)) <= (double)(0.001));
      turbo_fs_unlink(line_path);
      turbo_fs_unlink(token_path);
      turbo_script_free(ctx);
    }

  }

  describe("Print") {
    it("should not crash and return 0") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var r = "
                           "print(\"hello\", 42, [1, 2, 3]);";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(0.0)) <= (double)(0.001));
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: File Operations") {
    it("should enforce the IO failure contract for status and data builtins") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char missing_path[128];
      char script[512];
      const double *st_data = NULL;
      size_t st_len = 0;

      ts_test_make_name(missing_path, sizeof(missing_path), "_ts_missing_contract", ".txt");
      snprintf(script, sizeof(script),
               "var wr = write_file(123, \"bad\"); "
               "var rf = read_file(\"%s\"); "
               "var st = file_stat(\"%s\"); "
               "var pj = path_join(1, \"x\"); "
               "var dt = date(\"not-a-date\"); "
               "var fd = format_date(now(), 123);",
               missing_path, missing_path);

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("IO failure contract Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "wr")) - (double)(-1.0)) <= (double)(0.1));
      check_null(ts_get_str(ctx, "rf"));
      check(fabs((double)(ts_get_num(ctx, "rf")) - (double)(0.0)) <= (double)(0.1));
      check((ts_get_vec(ctx, "st", &st_data, &st_len)) == (-1));
      check(fabs((double)(ts_get_num(ctx, "st")) - (double)(0.0)) <= (double)(0.1));
      check_null(ts_get_str(ctx, "pj"));
      check(fabs((double)(ts_get_num(ctx, "pj")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "dt")) - (double)(0.0)) <= (double)(0.1));
      check_null(ts_get_str(ctx, "fd"));
      check(fabs((double)(ts_get_num(ctx, "fd")) - (double)(0.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should append_file content") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "write_file(\"_test_append.txt\", \"hello\"); "
                           "var r = append_file(\"_test_append.txt\", \" world\"); "
                           "var data = read_file(\"_test_append.txt\"); "
                           "file_remove(\"_test_append.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("append_file Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(0.0)) <= (double)(0.1));
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check(strcmp((data), ("hello world")) == 0);
      turbo_script_free(ctx);
    }

    it("should copy and truncate files") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char src_path[128];
      char dst_path[128];
      char script[640];

      ts_test_make_name(src_path, sizeof(src_path), "_test_copy_src", ".txt");
      ts_test_make_name(dst_path, sizeof(dst_path), "_test_copy_dst", ".txt");
      snprintf(script, sizeof(script),
               "write_file(\"%s\", \"abcdef\"); "
               "var c = copy_file(\"%s\", \"%s\"); "
               "var copied = read_file(\"%s\"); "
               "var t = file_truncate(\"%s\", 3); "
               "var truncated = read_file(\"%s\");",
               src_path, src_path, dst_path, dst_path, dst_path, dst_path);

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("copy/truncate Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "c")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "t")) - (double)(0.0)) <= (double)(0.1));
      check(strcmp((ts_get_str(ctx, "copied")), ("abcdef")) == 0);
      check(strcmp((ts_get_str(ctx, "truncated")), ("abc")) == 0);

      turbo_fs_unlink(src_path);
      turbo_fs_unlink(dst_path);
      turbo_script_free(ctx);
    }

    it("should get file_size") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "write_file(\"_test_size.txt\", \"12345\"); "
                           "var sz = file_size(\"_test_size.txt\"); "
                           "file_remove(\"_test_size.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("file_size Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "sz")) - (double)(5.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should return -1 for file_size on missing file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var sz = file_size(\"_nonexistent_xyz.txt\");";
      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "sz")) - (double)(-1.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should get file_stat vector [size, mtime, is_file, is_dir]") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "write_file(\"_test_stat.txt\", \"abc\"); "
                           "var st = file_stat(\"_test_stat.txt\"); "
                           "var n = vec.len(st); "
                           "var sz = st[0]; "
                           "var isf = st[2]; "
                           "var isd = st[3]; "
                           "file_remove(\"_test_stat.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("file_stat Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "n")) - (double)(4.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "sz")) - (double)(3.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "isf")) - (double)(1.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "isd")) - (double)(0.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should check is_file and is_dir") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "write_file(\"_test_isfile.txt\", \"x\"); "
                           "var f = is_file(\"_test_isfile.txt\"); "
                           "var d = is_dir(\"_test_isfile.txt\"); "
                           "file_remove(\"_test_isfile.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("is_file/is_dir Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "f")) - (double)(1.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "d")) - (double)(0.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should rename a file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "write_file(\"_test_rename_a.txt\", \"renamed\"); "
                           "var r = file_rename(\"_test_rename_a.txt\", \"_test_rename_b.txt\"); "
                           "var data = read_file(\"_test_rename_b.txt\"); "
                           "file_remove(\"_test_rename_b.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("file_rename Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(0.0)) <= (double)(0.1));
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check(strcmp((data), ("renamed")) == 0);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Directory Operations") {
    it("should mkdir and rmdir") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char dir_name[128];
      char script[384];

      ts_test_make_name(dir_name, sizeof(dir_name), "_test_dir_io", "");
      snprintf(script, sizeof(script),
               "var r1 = mkdir(\"%s\"); "
               "var exists = is_dir(\"%s\"); "
               "var r2 = rmdir(\"%s\"); "
               "var gone = is_dir(\"%s\");",
               dir_name, dir_name, dir_name, dir_name);

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("mkdir/rmdir Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "r1")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "exists")) - (double)(1.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "r2")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "gone")) - (double)(0.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should create and remove directories recursively") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char root_dir[128];
      char mid_dir[192];
      char nested_dir[192];
      char file_path[256];
      char script[900];

      ts_test_make_name(root_dir, sizeof(root_dir), "_test_recursive_io", "");
      turbo_fs_path_join(mid_dir, sizeof(mid_dir), root_dir, "a");
      turbo_fs_path_join(nested_dir, sizeof(nested_dir), mid_dir, "b");
      turbo_fs_path_join(file_path, sizeof(file_path), nested_dir, "data.txt");

      snprintf(script, sizeof(script),
               "var mid = path_join(\"%s\", \"a\"); "
               "var nested = path_join(mid, \"b\"); "
               "var data_path = path_join(nested, \"data.txt\"); "
               "var mk = mkdir_recursive(nested); "
               "var exists = is_dir(nested); "
               "write_file(data_path, \"nested\"); "
               "var data = read_file(data_path); "
               "var rm = rmdir_recursive(\"%s\"); "
               "var gone = is_dir(\"%s\");",
               root_dir, root_dir, root_dir);

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("recursive dir Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "mk")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "exists")) - (double)(1.0)) <= (double)(0.1));
      check(strcmp((ts_get_str(ctx, "data")), ("nested")) == 0);
      check(fabs((double)(ts_get_num(ctx, "rm")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "gone")) - (double)(0.0)) <= (double)(0.1));

      if (ts_get_num(ctx, "rm") != 0.0) {
        turbo_fs_unlink(file_path);
        turbo_fs_rmdir(nested_dir);
        turbo_fs_rmdir(mid_dir);
        turbo_fs_rmdir(root_dir);
      }
      turbo_script_free(ctx);
    }

    it("should return tmpdir as a string") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var td = tmpdir(); "
                           "var exists = is_dir(td);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("tmpdir Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      const char *td = ts_get_str(ctx, "td");
      check_not_null(td);
      check(fabs((double)(ts_get_num(ctx, "exists")) - (double)(1.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }

    it("should list directory entries and glob matching paths") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char dir_name[128];
      char a_path[192];
      char b_path[192];
      char c_path[192];
      char script[1400];

      ts_test_make_name(dir_name, sizeof(dir_name), "_test_glob_io", "");
      turbo_fs_path_join(a_path, sizeof(a_path), dir_name, "a.txt");
      turbo_fs_path_join(b_path, sizeof(b_path), dir_name, "b.txt");
      turbo_fs_path_join(c_path, sizeof(c_path), dir_name, "c.log");

      snprintf(script, sizeof(script),
               "var r = mkdir(\"%s\"); "
               "write_file(path_join(\"%s\", \"a.txt\"), \"a\"); "
               "write_file(path_join(\"%s\", \"b.txt\"), \"b\"); "
               "write_file(path_join(\"%s\", \"c.log\"), \"c\"); "
               "var entries = listdir(\"%s\"); "
               "var txts = glob(path_join(\"%s\", \"*.txt\")); "
               "var entry_score = entries.contains(\"a.txt\") + entries.contains(\"b.txt\") + "
               "entries.contains(\"c.log\") + entries.length(); "
               "var glob_score = txts.contains(path_join(\"%s\", \"a.txt\")) + "
               "txts.contains(path_join(\"%s\", \"b.txt\")) + "
               "txts.contains(path_join(\"%s\", \"c.log\")) + txts.length();",
               dir_name, dir_name, dir_name, dir_name, dir_name, dir_name,
               dir_name, dir_name, dir_name);

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("listdir/glob Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(0.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "entry_score")) - (double)(6.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "glob_score")) - (double)(4.0)) <= (double)(0.1));

      turbo_fs_unlink(a_path);
      turbo_fs_unlink(b_path);
      turbo_fs_unlink(c_path);
      turbo_fs_rmdir(dir_name);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Path Utilities") {
    it("should join paths") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var p = path_join(\"foo\", \"bar.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("path_join Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      const char *p = ts_get_str(ctx, "p");
      check_not_null(p);
      printf("  path_join result: %s\n", p);
      turbo_script_free(ctx);
    }

    it("should extract dirname and basename") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
#ifdef _WIN32
      const char *script = ""
                           "var d = path_dirname(\"C:\\\\foo\\\\bar.txt\"); "
                           "var b = path_basename(\"C:\\\\foo\\\\bar.txt\");";
#else
      const char *script = ""
                           "var d = path_dirname(\"/foo/bar.txt\"); "
                           "var b = path_basename(\"/foo/bar.txt\");";
#endif
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("dirname/basename Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      const char *d = ts_get_str(ctx, "d");
      const char *b = ts_get_str(ctx, "b");
      check_not_null(d);
      check_not_null(b);
      printf("  dirname: %s, basename: %s\n", d, b);
      check(strcmp((b), ("bar.txt")) == 0);
      turbo_script_free(ctx);
    }

    it("should detect absolute vs relative paths") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
#ifdef _WIN32
      const char *script = ""
                           "var a = path_is_absolute(\"C:\\\\foo\"); "
                           "var r = path_is_absolute(\"foo\\\\bar\");";
#else
      const char *script = ""
                           "var a = path_is_absolute(\"/foo\"); "
                           "var r = path_is_absolute(\"foo/bar\");";
#endif
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("path_is_absolute Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "a")) - (double)(1.0)) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "r")) - (double)(0.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Date/Time") {
    it("should return now() as a positive timestamp") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var t = now();";
      check((turbo_script_run(ctx, script)) == (0));
      check((ts_get_num(ctx, "t")) > (1700000000.0));
      turbo_script_free(ctx);
    }

    it("should parse date string to timestamp") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var t = date(\"2024-01-01 00:00:00\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("date Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check((ts_get_num(ctx, "t")) > (0.0));
      turbo_script_free(ctx);
    }

    it("should format_date with default and custom format") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var t = date(\"2024-01-02 03:04:05\"); "
                           "var s1 = format_date(t); "
                           "var s2 = format_date(t, \"%Y-%m-%d %H:%M:%S\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("format_date Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      const char *s1 = ts_get_str(ctx, "s1");
      const char *s2 = ts_get_str(ctx, "s2");
      check_not_null(s1);
      check_not_null(s2);
      check(strlen(s1) > 0);
      check_not_null(strstr(s1, "2024"));
      check(((int)strlen(s2)) == (19));
      check_not_null(strstr(s2, "2024-01-02 "));
      printf("  format_date default: %s\n", s1);
      printf("  format_date custom:  %s\n", s2);
      turbo_script_free(ctx);
    }

    it("should support explicit UTC date parsing and formatting") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var t1 = date_utc(\"2024-01-02 03:04:05\"); "
                           "var t2 = date_utc(\"2024-01-02T03:04:05Z\"); "
                           "var s = format_date_utc(t1, \"%Y-%m-%d %H:%M:%S\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("UTC date/time Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check(fabs((double)(ts_get_num(ctx, "t1")) - (double)(ts_get_num(ctx, "t2"))) <= (double)(0.1));
      check(strcmp((ts_get_str(ctx, "s")), ("2024-01-02 03:04:05")) == 0);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Platform Info") {
    it("should return os_name as a known string") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var os = os_name();";
      check((turbo_script_run(ctx, script)) == (0));
      const char *os = ts_get_str(ctx, "os");
      check_not_null(os);
      printf("  os_name: %s\n", os);
      turbo_script_free(ctx);
    }

    it("should return pid as a positive number") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var p = pid();";
      check((turbo_script_run(ctx, script)) == (0));
      check((ts_get_num(ctx, "p")) > (0.0));
      turbo_script_free(ctx);
    }

    it("should return uptime_ms and monotonic_ms as non-negative") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var u = uptime_ms(); "
                           "var m = monotonic_ms();";
      check((turbo_script_run(ctx, script)) == (0));
      double u = ts_get_num(ctx, "u");
      double m = ts_get_num(ctx, "m");
      printf("  uptime_ms: %g, monotonic_ms: %g\n", u, m);
      check(fabs((double)(u >= 0.0) - (double)(1.0)) <= (double)(0.1));
      check(fabs((double)(m >= 0.0) - (double)(1.0)) <= (double)(0.1));
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: End-to-End Pipeline") {
    it("should write, append, stat, read, rename, remove in sequence") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "write_file(\"_test_e2e.txt\", \"line1\\n\"); "
                           "append_file(\"_test_e2e.txt\", \"line2\\n\"); "
                           "var sz = file_size(\"_test_e2e.txt\"); "
                           "var st = file_stat(\"_test_e2e.txt\"); "
                           "var st_sz = st[0]; "
                           "file_rename(\"_test_e2e.txt\", \"_test_e2e_renamed.txt\"); "
                           "var data = read_file(\"_test_e2e_renamed.txt\"); "
                           "file_remove(\"_test_e2e_renamed.txt\"); "
                           "var gone = file_exists(\"_test_e2e_renamed.txt\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("E2E IO Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      check((ts_get_num(ctx, "sz")) > (0.0));
      check(fabs((double)(ts_get_num(ctx, "sz")) - (double)(ts_get_num(ctx, "st_sz"))) <= (double)(0.1));
      check(fabs((double)(ts_get_num(ctx, "gone")) - (double)(0.0)) <= (double)(0.1));
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      printf("  E2E data: %s\n", data);
      turbo_script_free(ctx);
    }

    it("should use tmpdir + path_join for temp file workflow") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var td = tmpdir(); "
                           "var p = path_join(td, \"_test_tmpfile.txt\"); "
                           "var wr = write_file(p, \"temp data\"); "
                           "var data = read_file(p); "
                           "file_remove(p);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("tmpdir pipeline Error: %s\n", turbo_script_get_error(ctx));
      check((res) == (0));
      const char *td = ts_get_str(ctx, "td");
      const char *p = ts_get_str(ctx, "p");
      printf("  tmpdir: %s\n", td ? td : "(null)");
      printf("  path:   %s\n", p ? p : "(null)");
      check(fabs((double)(ts_get_num(ctx, "wr")) - (double)(0.0)) <= (double)(0.1));
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check(strcmp((data), ("temp data")) == 0);
      turbo_script_free(ctx);
    }
  }

  describe("Async Await Syntax") {
    it("should await plain values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check((turbo_script_run(ctx, "var result = await 42;")) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(42.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }

    it("should treat async functions as normal callable functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "async func add_one(x) { return x + 1; }"
                           "var result = await add_one(41);";
      check_not_null(ctx);

      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(42.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }

    it("should unwrap coroutine-style result maps") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var packet = map {status: \"suspended\", value: 42};"
                           "var result = await packet;";
      check_not_null(ctx);

      check((turbo_script_run(ctx, script)) == (0));
      check(fabs((double)(ts_get_num(ctx, "result")) - (double)(42.0)) <= (double)(0.001));

      turbo_script_free(ctx);
    }

  }
}
