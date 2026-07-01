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

static exprtk_value_t test_triple_fn(size_t argc, exprtk_value_t *args, void *user_data) {
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
      check_int_eq(res, 0);
      const char *data_str = ts_get_str(ctx, "data");
      check_not_null(data_str);
      check_str_eq(data_str, "dot notation works");
      check_float_eq(ts_get_num(ctx, "gone"), 0.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should import json module and use dot notation") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_not_null(ctx);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = ""
                           "js = \"{\\\"name\\\": \\\"alice\\\", \\\"score\\\": 99}\"; "
                           "val = json.query(js, \"$.score\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON dot Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "val"), 99.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should still support file import with init_bare") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_not_null(ctx);

      turbo_script_run(ctx, ""
                            "write_file(\"utils_dot.tbs\", \"func square(x) { return x * x; };\");");

      const char *script = "import(\"utils_dot.tbs\"); "
                           "res = square(7);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 49.0, 0.1);

      turbo_script_run(ctx, "file_remove(\"utils_dot.tbs\");");
      turbo_script_free(ctx);
    }
  }

  describe("CSV Module") {
    it("should count rows and columns") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = "
                           "\"name_s,age_n,city_s\\nAlice,30,NYC\\nBob,25,LA\\nCharlie,35,SF\";"
                           "var r = csv.rows(data);"
                           "var c = csv.cols(data);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV rows/cols Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "r"), 3.0, 0.1); // data rows only (header separated)
      check_float_eq(ts_get_num(ctx, "c"), 3.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should get cell value as string") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = "
                           "\"name,score\\nAlice,95\\nBob,87\";"
                           "var val = csv.get(data, 0, 0);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = ts_get_str(ctx, "val");
      check_not_null(val);
      check_int_eq(strcmp(val, "Alice"), 0);

      turbo_script_free(ctx);
    }

    it("should get cell value as number") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = "
                           "\"name,score\\nAlice,95\\nBob,87\";"
                           "var val = csv.ts_get_num(data, 1, 1);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "val"), 87.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should bind CSV rows through a TBE schema") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Trade { double price; uint32 qty; bool active; string symbol; }\";"
          "var data = \"symbol,price,qty,active\\nAAPL,123.5,10,1\\nMSFT,20,3,0\";"
          "var trade = csv.bind(schema, data, 0, \"Trade\");"
          "var total = trade.price * trade.qty + trade.active;"
          "var symbol_len = trade.symbol.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 1236.0, 0.001);
      check_float_eq(ts_get_num(ctx, "symbol_len"), 4.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind all CSV rows through a TBE schema") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Trade { double price; uint32 qty; bool active; string symbol; }\";"
          "var data = \"symbol,price,qty,active\\nAAPL,10.5,2,1\\nMSFT,20,3,0\";"
          "var rows = csv.bind_all(schema, data, \"Trade\");"
          "var first = rows[0];"
          "var second = rows[1];"
          "var n = rows.length();"
          "var total = first.price * first.qty + second.price * second.qty;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV bind_all Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 81.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should reuse parsed schema handles for CSV binding") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Trade { double price; uint32 qty; string symbol; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var data = \"symbol,price,qty\\nAAPL,10.5,2\\nMSFT,20,3\";"
          "var trade = csv.bind_schema(schema_id, data, 0, \"Trade\");"
          "var rows = csv.bind_all_schema(schema_id, data, \"Trade\");"
          "var close_rc = schema.close(schema_id);"
          "var total = trade.price * trade.qty + rows[1].price * rows[1].qty;"
          "var n = rows.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV bind_schema Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "schema_id") >= 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 81.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind nested CSV composites and arrays through a TBE schema") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"composite Header { uint32 venue; uint32 seq; } "
          "composite Point { uint32 x; uint32 y; } "
          "message Trade { Header header; Point[2] points; uint32[2] levels; "
          "double price; string symbol; }\";"
          "var data = \"symbol,price,header.venue,header.seq,points[0].x,points[0].y,"
          "points[1].x,points[1].y,levels[0],levels[1]\\n"
          "AAPL,5.5,7,8,1,2,3,4,10,20\";"
          "var trade = csv.bind(schema, data, 0, \"Trade\");"
          "var score = trade.header.venue + trade.header.seq + trade.points[1].x + "
          "trade.levels[0] + trade.levels[1] + trade.price;"
          "var point_count = trade.points.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV nested bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "score"), 53.5, 0.001);
      check_float_eq(ts_get_num(ctx, "point_count"), 2.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate CSV groups through TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"group Level { uint64 price; uint32 qty; } "
          "message Book { uint32 seq; group<Level> bids; }\";"
          "var good = \"seq,bids[0].price,bids[0].qty,bids[1].price,bids[1].qty\\n"
          "7,100,10,200,20\";"
          "var bad = \"seq,bids[0].price,bids[0].qty,bids[1].price,bids[1].qty\\n"
          "7,100,10,200,bad\";"
          "var book = csv.bind(schema, good, 0, \"Book\");"
          "var ok = csv.validate(schema, good, \"Book\");"
          "var bad_detail = csv.validate_ex(schema, bad, \"Book\");"
          "var emitted = csv.emit(schema, book, \"Book\");"
          "var rebound = csv.bind(schema, emitted, 0, \"Book\");"
          "var n = book.bids.length();"
          "var total = book.seq + book.bids[0].qty + book.bids[1].price;"
          "var rebound_total = rebound.seq + rebound.bids[0].qty + rebound.bids[1].price;"
          "var bad_ok = bad_detail.ok;"
          "var bad_path_len = bad_detail.path.length();"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV group bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 217.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rebound_total"), 217.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_path_len"), 11.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate CSV list and set containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Containers { list<uint32> values; set<string> tags; }\";"
          "var good = \"values[0],values[1],values[2],tags[0],tags[1]\\n10,20,30,a,bb\";"
          "var bad = \"values[0],values[1],tags[0]\\n10,bad,a\";"
          "var row = csv.bind(schema, good, 0, \"Containers\");"
          "var ok = csv.validate(schema, good, \"Containers\");"
          "var bad_detail = csv.validate_ex(schema, bad, \"Containers\");"
          "var emitted = csv.emit(schema, row, \"Containers\");"
          "var rebound = csv.bind(schema, emitted, 0, \"Containers\");"
          "var total = row.values[0] + row.values[2] + row.tags[1].length();"
          "var rebound_total = rebound.values[0] + rebound.values[2] + rebound.tags[1].length();"
          "var value_count = row.values.length();"
          "var tag_count = row.tags.length();"
          "var bad_ok = bad_detail.ok;"
          "var bad_path_len = bad_detail.path.length();"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV list/set bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 42.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rebound_total"), 42.0, 0.001);
      check_float_eq(ts_get_num(ctx, "value_count"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tag_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_path_len"), 9.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should emit CSV from schema-bound values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"composite Header { uint32 venue; uint32 seq; } "
          "composite Point { uint32 x; uint32 y; } "
          "message Trade { Header header; Point[2] points; uint32[2] levels; "
          "double price; string symbol; }\";"
          "var js = \"[{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":5.5,"
          "\\\"header\\\":{\\\"venue\\\":7,\\\"seq\\\":8},"
          "\\\"points\\\":[{\\\"x\\\":1,\\\"y\\\":2},{\\\"x\\\":3,\\\"y\\\":4}],"
          "\\\"levels\\\":[10,20]}]\";"
          "var rows = json.bind_all(schema, js, \"Trade\");"
          "var emitted = csv.emit(schema, rows, \"Trade\");"
          "var rebound = csv.bind_all(schema, emitted, \"Trade\");"
          "var trade = rebound[0];"
          "var score = trade.header.venue + trade.header.seq + trade.points[1].x + "
          "trade.levels[0] + trade.levels[1] + trade.price;"
          "var n = rebound.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV emit Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 53.5, 0.001);

      turbo_script_free(ctx);
    }

    it("should emit CSV record lists with merged dynamic headers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Success { uint32 code; } "
          "union Choice { Side side; Permissions perms; Success success; } "
          "group Level { uint64 price; uint32 qty; } "
          "message Batch { uint32 seq; Choice choice; group<Level> bids; "
          "map<string,int32> attrs; list<Choice> choices; }\";"
          "var js = \"["
          "{\\\"seq\\\":1,\\\"bids\\\":[{\\\"price\\\":100,\\\"qty\\\":10},"
          "{\\\"price\\\":200,\\\"qty\\\":20}],\\\"attrs\\\":{\\\"x\\\":3},"
          "\\\"choice\\\":{\\\"side\\\":\\\"Buy\\\"},"
          "\\\"choices\\\":[{\\\"side\\\":\\\"Buy\\\"}]},"
          "{\\\"seq\\\":2,\\\"bids\\\":[{\\\"price\\\":300,\\\"qty\\\":30}],"
          "\\\"attrs\\\":{\\\"y\\\":4},\\\"choice\\\":{\\\"success\\\":{\\\"code\\\":200}},"
          "\\\"choices\\\":[{\\\"success\\\":{\\\"code\\\":201}},"
          "{\\\"perms\\\":\\\"Read|Execute\\\"}]}]\";"
          "var rows = json.bind_all(schema, js, \"Batch\");"
          "var emitted = csv.emit(schema, rows, \"Batch\");"
          "var ok = csv.validate(schema, emitted, \"Batch\");"
          "var detail = csv.validate_ex(schema, emitted, \"Batch\");"
          "var rebound = csv.bind_all(schema, emitted, \"Batch\");"
          "var first = rebound[0];"
          "var second = rebound[1];"
          "var counts = rebound.length() + first.bids.length() + second.bids.length() + "
          "first.choices.length() + second.choices.length();"
          "var total = first.seq + first.bids[1].qty + first.attrs.x + first.choice.side + "
          "first.choices[0].side + second.seq + second.bids[0].qty + second.attrs.y + "
          "second.choice.success.code + second.choices[0].success.code + second.choices[1].perms;"
          "var detail_ok = detail.ok;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV merged dynamic emit Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "detail_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "counts"), 8.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 468.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should validate CSV against TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"composite Header { uint32 venue; uint32 seq; } "
          "message Trade { Header header; double price; uint32 qty; string symbol; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var good = \"symbol,price,qty,header.venue,header.seq\\nAAPL,10.5,2,7,8\";"
          "var missing = \"symbol,price,qty,header.venue\\nAAPL,10.5,2,7\";"
          "var bad = \"symbol,price,qty,header.venue,header.seq\\nAAPL,bad,2,7,8\";"
          "var ok = csv.validate_schema(schema_id, good, \"Trade\");"
          "var missing_ok = csv.validate_schema(schema_id, missing, \"Trade\");"
          "var bad_ok = csv.validate(schema_text, bad, \"Trade\");"
          "var missing_detail = csv.validate_ex_schema(schema_id, missing, \"Trade\");"
          "var bad_detail = csv.validate_ex(schema_text, bad, \"Trade\");"
          "var missing_path_len = missing_detail.path.length();"
          "var missing_message_len = missing_detail.message.length();"
          "var bad_path_len = bad_detail.path.length();"
          "var bad_message_len = bad_detail.message.length();"
          "var close_rc = schema.close(schema_id);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV validate Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_path_len"), 10.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_path_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate CSV maps through TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Attrs { uint32 seq; map<string,int32> attrs; }\";"
          "var good = \"seq,attrs.x,attrs.y\\n7,30,40\";"
          "var bad = \"seq,attrs.x,attrs.y\\n7,30,bad\";"
          "var row = csv.bind(schema_text, good, 0, \"Attrs\");"
          "var ok = csv.validate(schema_text, good, \"Attrs\");"
          "var bad_detail = csv.validate_ex(schema_text, bad, \"Attrs\");"
          "var emitted = csv.emit(schema_text, row, \"Attrs\");"
          "var rebound = csv.bind(schema_text, emitted, 0, \"Attrs\");"
          "var total = row.seq + row.attrs.x + row.attrs.y;"
          "var rebound_total = rebound.seq + rebound.attrs.x + rebound.attrs.y;"
          "var bad_ok = bad_detail.ok;"
          "var bad_path_len = bad_detail.path.length();"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV map bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 77.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rebound_total"), 77.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_path_len"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind CSV maps and groups from sanitized flattened columns") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"group Level { uint64 price; uint32 qty; } "
          "message Book { uint32 seq; group<Level> bids; map<string,int32> attrs; }\";"
          "var data = \"seq,bids_0_price,bids_0_qty,bids_1_price,bids_1_qty,attrs_x,attrs_y\\n"
          "7,100,10,200,20,3,4\";"
          "var book = csv.bind(schema, data, 0, \"Book\");"
          "var ok = csv.validate(schema, data, \"Book\");"
          "var total = book.seq + book.bids[0].qty + book.bids[1].price + book.attrs.x + book.attrs.y;"
          "var bid_count = book.bids.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV sanitized bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 224.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bid_count"), 2.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should extract column as numeric vector by index") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var data = "
          "\"ticker,close,volume\\nAAPL,150.5,1000\\nGOOG,2800.0,500\\nMSFT,300.25,750\";"
          "var prices = csv.col(data, 1);"
          "var n = vec.len(prices);"
          "var total = vec.sum(prices);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV col Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 3.0, 0.1);
      check_float_eq(ts_get_num(ctx, "total"), 3250.75, 0.1);

      turbo_script_free(ctx);
    }

    it("should extract column as numeric vector by name") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var data = "
          "\"ticker,close,volume\\nAAPL,150.5,1000\\nGOOG,2800.0,500\\nMSFT,300.25,750\";"
          "var prices = csv.col(data, \"close\");"
          "var avg_price = vec.avg(prices);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "avg_price"), 1083.583, 0.1);

      turbo_script_free(ctx);
    }

    it("should run csv.col into TA pipeline") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "ta"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = \"date,close\\n"
                           "2024-01-01,100\\n2024-01-02,102\\n2024-01-03,104\\n"
                           "2024-01-04,103\\n2024-01-05,105\\n2024-01-06,107\\n"
                           "2024-01-07,109\\n2024-01-08,108\\n2024-01-09,110\\n"
                           "2024-01-10,112\";"
                           "var close = csv.col(data, \"close\");"
                           "var sma3 = ta.sma(close, 3);"
                           "var last = close[9];"
                           "var last_sma = sma3[9];";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV TA Pipeline Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "last"), 112.0, 0.1);
      check_float_gt(ts_get_num(ctx, "last_sma"), 0.0);

      turbo_script_free(ctx);
    }

    it("should filter rows and count matches") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = "
                           "\"name_s,score_n\\nAlice,95\\nBob,60\\nCharlie,85\\nDave,45\";"
                           "var n = csv.filter_count(data, \"score > 70\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV filter_count Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should filter rows and return matching content") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = "
                           "\"name_s,score_n\\nAlice,95\\nBob,60\\nCharlie,85\";"
                           "var result = csv.filter(data, \"score > 70\");"
                           "var has_data = (vec.len(result) > 0);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "has_data"), 1.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should work via import mechanism") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = ""
                           "import(\"parser\");"
                           "var data = \"x,y\\n1,2\\n3,4\\n5,6\";"
                           "var xs = csv.col(data, \"x\");"
                           "var total = vec.sum(xs);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "total"), 9.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Net Plugin (HTTP via Script)") {
    it("should fetch mockhttp.org using http.get from script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = ""
                           "import(\"net\");\n"
                           "var res = http.get(\"https://mockhttp.org\", {timeout: 5000});\n"
                           "var is_ok = (res.status == 200);\n"
                           "var has_headers = (map.size(res.headers) > 0);\n";

      int run_res = turbo_script_run(ctx, script);
      if (run_res != 0) printf("Net Plugin Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(run_res, 0);
      check_float_eq(ts_get_num(ctx, "is_ok"), 1.0, 0.1);
      check_float_eq(ts_get_num(ctx, "has_headers"), 1.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("DSV Filter via Script API") {
    it("should filter rows based on number column") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var data = "
          "\"name_s,age_n,role_s\\nAlice,30,dev\\nBob,40,manager\\nCharlie,25,intern\";"
          "var result = csv.filter(data, \"age > 30\");"
          "var n = csv.filter_count(data, \"age > 30\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("DSV filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 1.0, 0.1);

      const char *result = ts_get_str(ctx, "result");
      check_not_null(result);

      turbo_script_free(ctx);
    }

    it("should filter rows based on string column") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = "
                           "\"name_s,score_n\\nAlice,10\\nBob,5\";"
                           "var result = csv.filter(data, \"name == \\\"Alice\\\"\");"
                           "var n = csv.filter_count(data, \"name == \\\"Alice\\\"\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("DSV string filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 1.0, 0.1);

      turbo_script_free(ctx);
    }

    it("should count zero matches correctly") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var data = \"a_n,b_n\\n1,2\\n3,4\";"
                           "var n = csv.filter_count(data, \"a + b == 99\");";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 0.0, 0.1);

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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "first"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 9.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should sort descending") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = "var v = [3, 1, 4, 1, 5];"
                           "var s = vec.sort_desc(v);"
                           "var first = s[0];"
                           "var last = s[4];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "first"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 1.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should not modify original vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [3, 1, 2];"
                           "var s = vec.sort(v);"
                           "var orig_first = v[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "orig_first"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = " var v = [42];"
                           "var s = vec.sort(v);"
                           "var val = s[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "val"), 42.0, 0.001);
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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "first"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle all same values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [5, 5, 5, 5];"
                           "var u = vec.unique(v);"
                           "var n = vec.len(u);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("String Join") {
    it("should join vector with delimiter") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [1, 2, 3];"
                           "var s = join(v, \",\");";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *result = ts_get_str(ctx, "s");
      check_not_null(result);
      check_str_eq(result, "1,2,3");
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [42];"
                           "var s = join(v, \"-\");";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *result = ts_get_str(ctx, "s");
      check_not_null(result);
      check_str_eq(result, "42");
      turbo_script_free(ctx);
    }
  }

  describe("CSV Write") {
    it("should write and read back CSV file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      int r1 = turbo_script_run(ctx, "var data = \"name,age,role\\nAlice,30,dev\\nBob,40,mgr\";"
                                     "var res = csv.write(\"_test_csv_write.csv\", data);"
                                     "var content = read_file(\"_test_csv_write.csv\");"
                                     "var rows = csv.rows(data);"
                                     "file_remove(\"_test_csv_write.csv\");");
      if (r1 != 0) printf("CSV write Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(r1, 0);
      check_float_eq(ts_get_num(ctx, "res"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "rows"), 2.0, 0.1);
      const char *content = ts_get_str(ctx, "content");
      check_not_null(content);

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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "first"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 1.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [42];"
                           "var r = vec.reverse(v);"
                           "var val = r[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "val"), 42.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should not modify original vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [1, 2, 3];"
                           "var r = vec.reverse(v);"
                           "var orig = v[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "orig"), 1.0, 0.001);
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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 5.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle one empty vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      ts_bind_vec(ctx, "a", (const double[]){1.0, 2.0}, 2);
      const char *script = " var b = vec.range(0);"
                           "var c = vec.concat(a, b);"
                           "var n = vec.len(c);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "first"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 4.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should generate vec.range(2, 5)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = vec.range(2, 5);"
                           "var n = vec.len(v);"
                           "var first = v[0];"
                           "var last = v[2];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "first"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 4.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should return empty for vec.range(0)") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = "var v = vec.range(0);"
                           "var n = vec.len(v);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 0.0, 0.001);
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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "a"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "b"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c"), 6.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should handle single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var v = [42];"
                           "var cs = vec.cumsum(v);"
                           "var val = cs[0];";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "val"), 42.0, 0.001);
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
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "b"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should return empty for single element") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [42];"
                           "var d = vec.diff(v);"
                           "var n = vec.len(d);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "n"), 0.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Find") {
    it("should return index when found") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [10, 20, 30, 40];"
                           "var idx = vec.find(v, 30);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "idx"), 2.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should return -1 when not found") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " var v = [10, 20, 30];"
                           "var idx = vec.find(v, 99);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "idx"), -1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("String Format") {
    it("should format with %%s and %%d") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var s = "
                           "format(\"%s is %d\", \"age\", 30);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = ts_get_str(ctx, "s");
      check_not_null(val);
      check_str_eq(val, "age is 30");
      turbo_script_free(ctx);
    }

    it("should format with %%g") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var s = format(\"pi=%g\", 3.14);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = ts_get_str(ctx, "s");
      check_not_null(val);
      check_str_eq(val, "pi=3.14");
      turbo_script_free(ctx);
    }

    it("should escape %% as literal percent") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var s = format(\"%d%%\", 100);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *val = ts_get_str(ctx, "s");
      check_not_null(val);
      check_str_eq(val, "100%");
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "out_n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "out_a"), 10.0, 0.001);
      check_float_eq(ts_get_num(ctx, "out_b"), 30.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 10.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tn"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "dn"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ta"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "db"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "la"), -1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "lb"), 10.0, 0.001);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 67.0, 0.001);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 30.0, 0.001);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "line_total"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "token_total"), 12.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 18.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support file source streams through MIR JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *line_text = " a \nbb\n\nccc";
      const char *token_text = "AAPL,MSFT,TSLA";
      const char *json_text =
          "{\"orders\":[{\"price\":10,\"qty\":2},{\"price\":3,\"qty\":5},{\"price\":8,\"qty\":1}]}";
      const char *csv_text = "price,qty\n10,2\n3,5\n8,1\n";
      const char *csv_no_header_text = "10,2\n3,5\n8,1\n";
      const char *xml_text =
          "<orders><order id=\"a\"><price>10</price></order>"
          "<order id=\"b\"><price>3</price></order>"
          "<order id=\"c\"><price>8</price></order></orders>";
      char line_path[96];
      char token_path[96];
      char json_path[96];
      char csv_path[96];
      char csv_no_header_path[96];
      char xml_path[96];
      char script[4096];
      turbo_fs_buf_t buf;
      int res;

      ts_test_make_name(line_path, sizeof(line_path), "_test_stream_file_lines", ".txt");
      ts_test_make_name(token_path, sizeof(token_path), "_test_stream_file_tokens", ".txt");
      ts_test_make_name(json_path, sizeof(json_path), "_test_stream_file_json", ".json");
      ts_test_make_name(csv_path, sizeof(csv_path), "_test_stream_file_csv", ".csv");
      ts_test_make_name(csv_no_header_path, sizeof(csv_no_header_path),
                        "_test_stream_file_csv_no_header", ".csv");
      ts_test_make_name(xml_path, sizeof(xml_path), "_test_stream_file_xml", ".xml");

      buf = turbo_fs_buf_init((char *)line_text, strlen(line_text));
      check_int_eq(turbo_fs_write_file(line_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)token_text, strlen(token_text));
      check_int_eq(turbo_fs_write_file(token_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)json_text, strlen(json_text));
      check_int_eq(turbo_fs_write_file(json_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)csv_text, strlen(csv_text));
      check_int_eq(turbo_fs_write_file(csv_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)csv_no_header_text, strlen(csv_no_header_text));
      check_int_eq(turbo_fs_write_file(csv_no_header_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)xml_text, strlen(xml_text));
      check_int_eq(turbo_fs_write_file(xml_path, &buf), 0);

      snprintf(script, sizeof(script),
          "var line_total = stream.file(\"%s\")"
          "  .lines()"
          "  .map(line => line.trim())"
          "  .filter(line => line.length() > 0)"
          "  .map(line => line.length())"
          "  .reduce(0, (acc, n) => acc + n); "
          "var token_total = stream.file(\"%s\")"
          "  .text()"
          "  .split(\",\")"
          "  .map(sym => sym.length())"
          "  .reduce(0, (acc, n) => acc + n); "
          "var json_total = stream.file(\"%s\")"
          "  .json(\"$.orders[@.price > 5]\")"
          "  .map(r => r.price * r.qty)"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_total = stream.file(\"%s\")"
          "  .csv()"
          "  .filter(r => to_num(r.price) > 5)"
          "  .map(r => to_num(r.price) * to_num(r.qty))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_no_header_total = stream.file(\"%s\")"
          "  .csv(0)"
          "  .filter(r => to_num(r.c0) > 5)"
          "  .map(r => to_num(r.c0) * to_num(r.c1))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var xml_total = stream.file(\"%s\")"
          "  .xml(\"//price\")"
          "  .filter(n => to_num(n.text) > 5)"
          "  .map(n => to_num(n.text))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var attr_count = stream.file(\"%s\").xml(\"//@id\").count(); "
          "var score = line_total + token_total + json_total + csv_total + "
          "csv_no_header_total + xml_total + attr_count;",
          line_path, token_path, json_path, csv_path, csv_no_header_path, xml_path, xml_path);

      res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style file source stream Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "line_total"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "token_total"), 12.0, 0.001);
      check_float_eq(ts_get_num(ctx, "json_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_no_header_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "xml_total"), 18.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 123.0, 0.001);
      turbo_fs_unlink(line_path);
      turbo_fs_unlink(token_path);
      turbo_fs_unlink(json_path);
      turbo_fs_unlink(csv_path);
      turbo_fs_unlink(csv_no_header_path);
      turbo_fs_unlink(xml_path);
      turbo_script_free(ctx);
    }

    it("should support XML file streams through MIR JIT") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *xml =
          "<orders><order id=\"a\"><price>10</price></order>"
          "<order id=\"b\"><price>3</price></order>"
          "<order id=\"c\"><price>8</price></order></orders>";
      char xml_path[96];
      char script[1024];
      turbo_fs_buf_t buf;
      int res;

      ts_test_make_name(xml_path, sizeof(xml_path), "_test_stream_xml_jit", ".xml");
      buf = turbo_fs_buf_init((char *)xml, strlen(xml));
      check_int_eq(turbo_fs_write_file(xml_path, &buf), 0);

      snprintf(script, sizeof(script),
          "var total = stream.file(\"%s\").xml(\"//price\")"
          "  .filter(n => to_num(n.text) > 5)"
          "  .map(n => to_num(n.text))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var attr_count = stream.file(\"%s\").xml(\"//@id\").count(); "
          "var score = total + attr_count;",
          xml_path, xml_path);

      res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style XML JIT stream Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 18.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 21.0, 0.001);
      turbo_fs_unlink(xml_path);
      turbo_script_free(ctx);
    }

    it("should support Java-style streams from json and csv files") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char json_path[96];
      char csv_path[96];
      char csv_no_header_path[96];
      char script[3072];
      int res;
      ts_test_make_name(json_path, sizeof(json_path), "_test_stream_json", ".json");
      ts_test_make_name(csv_path, sizeof(csv_path), "_test_stream_csv", ".csv");
      ts_test_make_name(csv_no_header_path, sizeof(csv_no_header_path), "_test_stream_csv_no_header", ".csv");
      snprintf(script, sizeof(script),
          "write_file(\"%s\", \"[{\\\"price\\\":10,\\\"qty\\\":2},{\\\"price\\\":3,\\\"qty\\\":5},{\\\"price\\\":8,\\\"qty\\\":1}]\"); "
          "write_file(\"%s\", \"price,qty\\n10,2\\n3,5\\n8,1\\n\"); "
          "write_file(\"%s\", \"10,2\\n3,5\\n8,1\\n\"); "
          "var json_total = stream.file(\"%s\").json()"
          "  .filter(r => r.price > 5)"
          "  .map(r => r.price * r.qty)"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_total = stream.file(\"%s\").csv()"
          "  .filter(r => to_num(r.price) > 5)"
          "  .map(r => to_num(r.price) * to_num(r.qty))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_no_header_total = stream.file(\"%s\").csv(0)"
          "  .filter(r => to_num(r.c0) > 5)"
          "  .map(r => to_num(r.c0) * to_num(r.c1))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var line_count = stream.file(\"%s\").lines().filter(line => line.length() > 0).count(); "
          "var score = json_total + csv_total + csv_no_header_total + line_count;",
          json_path, csv_path, csv_no_header_path, json_path, csv_path,
          csv_no_header_path, csv_path);
      res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style file stream Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "json_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_no_header_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "line_count"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 88.0, 0.001);
      turbo_fs_unlink(json_path);
      turbo_fs_unlink(csv_path);
      turbo_fs_unlink(csv_no_header_path);
      turbo_script_free(ctx);
    }

    it("should support expression filters and json path stream sources") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char json_path[96];
      char csv_path[96];
      char csv_no_header_path[96];
      char script[4096];
      int res;
      ts_test_make_name(json_path, sizeof(json_path), "_test_stream_path_json", ".json");
      ts_test_make_name(csv_path, sizeof(csv_path), "_test_stream_expr_csv", ".csv");
      ts_test_make_name(csv_no_header_path, sizeof(csv_no_header_path),
                        "_test_stream_expr_csv_no_header", ".csv");
      snprintf(script, sizeof(script),
          "write_file(\"%s\", \"{\\\"orders\\\":[{\\\"price\\\":10,\\\"qty\\\":2},"
          "{\\\"price\\\":3,\\\"qty\\\":5},{\\\"price\\\":8,\\\"qty\\\":1}]}\"); "
          "write_file(\"%s\", \"price,qty\\n10,2\\n3,5\\n8,1\\n\"); "
          "write_file(\"%s\", \"10,2\\n3,5\\n8,1\\n\"); "
          "var json_total = stream.file(\"%s\").json(\"$.orders[@.price > 5]\")"
          "  .map(r => r.price * r.qty)"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_total = stream.file(\"%s\").csv()"
          "  .filterExpr(\"price > 5\")"
          "  .map(r => to_num(r.price) * to_num(r.qty))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_alias_total = stream.file(\"%s\").csv()"
          "  .where(\"price > 5\")"
          "  .map(r => to_num(r.price) * to_num(r.qty))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var csv_no_header_total = stream.file(\"%s\").csv(0)"
          "  .filterExpr(\"c0 > 5\")"
          "  .map(r => to_num(r.c0) * to_num(r.c1))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var score = json_total + csv_total + csv_alias_total + csv_no_header_total;",
          json_path, csv_path, csv_no_header_path, json_path, csv_path, csv_path,
          csv_no_header_path);
      res = turbo_script_run_jit(ctx, script);
      if (res != 0) printf("Java-style stream expression filter Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "json_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_alias_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_no_header_total"), 28.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 112.0, 0.001);
      turbo_fs_unlink(json_path);
      turbo_fs_unlink(csv_path);
      turbo_fs_unlink(csv_no_header_path);
      turbo_script_free(ctx);
    }

    it("should support Java-style streams from XML files") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      char xml_path[96];
      char script[2048];
      int res;

      ts_test_make_name(xml_path, sizeof(xml_path), "_test_stream_xml", ".xml");
      snprintf(script, sizeof(script),
          "write_file(\"%s\", \"<orders><order id=\\\"a\\\"><price>10</price></order>"
          "<order id=\\\"b\\\"><price>3</price></order>"
          "<order id=\\\"c\\\"><price>8</price></order></orders>\"); "
          "var total = stream.file(\"%s\").xml(\"//price\")"
          "  .filter(n => to_num(n.text) > 5)"
          "  .map(n => to_num(n.text))"
          "  .reduce(0, (acc, v) => acc + v); "
          "var names = stream.file(\"%s\").xml(\"//@id\").map(n => n.text).toList(); "
          "var score = total + names.length() + (names[1] == \"b\");",
          xml_path, xml_path, xml_path);

      res = turbo_script_run(ctx, script);
      if (res != 0) printf("Java-style XML stream Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 18.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 22.0, 0.001);
      turbo_fs_unlink(xml_path);
      turbo_script_free(ctx);
    }
  }

  describe("Print") {
    it("should not crash and return 0") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var r = "
                           "print(\"hello\", 42, [1, 2, 3]);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "r"), 0.0, 0.001);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "wr"), -1.0, 0.1);
      check_null(ts_get_str(ctx, "rf"));
      check_float_eq(ts_get_num(ctx, "rf"), 0.0, 0.1);
      check_int_eq(ts_get_vec(ctx, "st", &st_data, &st_len), -1);
      check_float_eq(ts_get_num(ctx, "st"), 0.0, 0.1);
      check_null(ts_get_str(ctx, "pj"));
      check_float_eq(ts_get_num(ctx, "pj"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "dt"), 0.0, 0.1);
      check_null(ts_get_str(ctx, "fd"));
      check_float_eq(ts_get_num(ctx, "fd"), 0.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "r"), 0.0, 0.1);
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "hello world");
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "c"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "t"), 0.0, 0.1);
      check_str_eq(ts_get_str(ctx, "copied"), "abcdef");
      check_str_eq(ts_get_str(ctx, "truncated"), "abc");

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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "sz"), 5.0, 0.1);
      turbo_script_free(ctx);
    }

    it("should return -1 for file_size on missing file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var sz = file_size(\"_nonexistent_xyz.txt\");";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "sz"), -1.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 4.0, 0.1);
      check_float_eq(ts_get_num(ctx, "sz"), 3.0, 0.1);
      check_float_eq(ts_get_num(ctx, "isf"), 1.0, 0.1);
      check_float_eq(ts_get_num(ctx, "isd"), 0.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "f"), 1.0, 0.1);
      check_float_eq(ts_get_num(ctx, "d"), 0.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "r"), 0.0, 0.1);
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "renamed");
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "r1"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "exists"), 1.0, 0.1);
      check_float_eq(ts_get_num(ctx, "r2"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "gone"), 0.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "mk"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "exists"), 1.0, 0.1);
      check_str_eq(ts_get_str(ctx, "data"), "nested");
      check_float_eq(ts_get_num(ctx, "rm"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "gone"), 0.0, 0.1);

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
      check_int_eq(res, 0);
      const char *td = ts_get_str(ctx, "td");
      check_not_null(td);
      check_float_eq(ts_get_num(ctx, "exists"), 1.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "r"), 0.0, 0.1);
      check_float_eq(ts_get_num(ctx, "entry_score"), 6.0, 0.1);
      check_float_eq(ts_get_num(ctx, "glob_score"), 4.0, 0.1);

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
      check_int_eq(res, 0);
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
      check_int_eq(res, 0);
      const char *d = ts_get_str(ctx, "d");
      const char *b = ts_get_str(ctx, "b");
      check_not_null(d);
      check_not_null(b);
      printf("  dirname: %s, basename: %s\n", d, b);
      check_str_eq(b, "bar.txt");
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "a"), 1.0, 0.1);
      check_float_eq(ts_get_num(ctx, "r"), 0.0, 0.1);
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Date/Time") {
    it("should return now() as a positive timestamp") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var t = now();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_gt(ts_get_num(ctx, "t"), 1700000000.0);
      turbo_script_free(ctx);
    }

    it("should parse date string to timestamp") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var t = date(\"2024-01-01 00:00:00\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("date Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_gt(ts_get_num(ctx, "t"), 0.0);
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
      check_int_eq(res, 0);
      const char *s1 = ts_get_str(ctx, "s1");
      const char *s2 = ts_get_str(ctx, "s2");
      check_not_null(s1);
      check_not_null(s2);
      check(strlen(s1) > 0);
      check_not_null(strstr(s1, "2024"));
      check_int_eq((int)strlen(s2), 19);
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
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "t1"), ts_get_num(ctx, "t2"), 0.1);
      check_str_eq(ts_get_str(ctx, "s"), "2024-01-02 03:04:05");
      turbo_script_free(ctx);
    }
  }

  describe("IO Module: Platform Info") {
    it("should return os_name as a known string") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var os = os_name();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      const char *os = ts_get_str(ctx, "os");
      check_not_null(os);
      printf("  os_name: %s\n", os);
      turbo_script_free(ctx);
    }

    it("should return pid as a positive number") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var p = pid();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_gt(ts_get_num(ctx, "p"), 0.0);
      turbo_script_free(ctx);
    }

    it("should return uptime_ms and monotonic_ms as non-negative") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "var u = uptime_ms(); "
                           "var m = monotonic_ms();";
      check_int_eq(turbo_script_run(ctx, script), 0);
      double u = ts_get_num(ctx, "u");
      double m = ts_get_num(ctx, "m");
      printf("  uptime_ms: %g, monotonic_ms: %g\n", u, m);
      check_float_eq(u >= 0.0, 1.0, 0.1);
      check_float_eq(m >= 0.0, 1.0, 0.1);
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
      check_int_eq(res, 0);
      check_float_gt(ts_get_num(ctx, "sz"), 0.0);
      check_float_eq(ts_get_num(ctx, "sz"), ts_get_num(ctx, "st_sz"), 0.1);
      check_float_eq(ts_get_num(ctx, "gone"), 0.0, 0.1);
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
      check_int_eq(res, 0);
      const char *td = ts_get_str(ctx, "td");
      const char *p = ts_get_str(ctx, "p");
      printf("  tmpdir: %s\n", td ? td : "(null)");
      printf("  path:   %s\n", p ? p : "(null)");
      check_float_eq(ts_get_num(ctx, "wr"), 0.0, 0.1);
      const char *data = ts_get_str(ctx, "data");
      check_not_null(data);
      check_str_eq(data, "temp data");
      turbo_script_free(ctx);
    }
  }

  describe("Async Await Syntax") {
    it("should await plain values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);

      check_int_eq(turbo_script_run(ctx, "var result = await 42;"), 0);
      check_float_eq(ts_get_num(ctx, "result"), 42.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should treat async functions as normal callable functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "async func add_one(x) { return x + 1; }"
                           "var result = await add_one(41);";
      check_not_null(ctx);

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "result"), 42.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should unwrap coroutine-style result maps") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var packet = map {status: \"suspended\", value: 42};"
                           "var result = await packet;";
      check_not_null(ctx);

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "result"), 42.0, 0.001);

      turbo_script_free(ctx);
    }

  }
}
 