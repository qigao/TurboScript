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


spec("turbo_script_basics") {
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

  describe("JSON") {
    it("should support json_query") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script = " js = "
                           "\"{\\\"user\\\": \\\"bob\\\", \\\"score\\\": 42}\";"
                           "res = json.query(js, \"$.score\");"
                           "name = json.query(js, \"$.user\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON query Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "res"), 42.0, 0.001);
      check_str_eq(ts_get_str(ctx, "name"), "bob");

      turbo_script_free(ctx);
    }

    it("should support json query paths") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      const char *script =
          "js = \"{\\\"user\\\":{\\\"name\\\":\\\"bob\\\"},"
          "\\\"orders\\\":[{\\\"price\\\":10,\\\"qty\\\":2},{\\\"price\\\":3,\\\"qty\\\":5},{\\\"price\\\":8,\\\"qty\\\":3}],"
          "\\\"matrix\\\":[[10,11],[20,21]]}\";"
          "name = json.query(js, \"$.user.name\");"
          "qty = json.query_num(js, \"$.orders[1].qty\", -1);"
          "cell = json.query(js, \"$.matrix[1][0]\");"
          "filtered = json.query(js, \"$.orders[@.price > 5].qty\");"
          "filtered_count = filtered.length();"
          "filtered_total = filtered[0] + filtered[1];"
          "missing = json.query_num(js, \"$.orders[3].qty\", 99);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON query path Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_str_eq(ts_get_str(ctx, "name"), "bob");
      check_float_eq(ts_get_num(ctx, "qty"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cell"), 20.0, 0.001);
      check_float_eq(ts_get_num(ctx, "filtered_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "filtered_total"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing"), 99.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should parse JSON into native containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var data = json.parse(\"{\\\"name\\\":\\\"Alice\\\",\\\"age\\\":30,"
          "\\\"ok\\\":true,\\\"xs\\\":[1,2],\\\"nested\\\":{\\\"v\\\":\\\"x\\\"}}\");"
          "var total = data.age + data.ok + data.xs[1] + data.name.length() + "
          "data.nested.v.length();"
          "var object_checks = (typeof(data) == \"object\") + is_object(data) + "
          "is_object(data.nested) + is_map(data);"
          "var bool_checks = (typeof(data.ok) == \"bool\") + is_bool(data.ok);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON parse Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 39.0, 0.001);
      check_float_eq(ts_get_num(ctx, "object_checks"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bool_checks"), 2.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should stringify native containers as JSON") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var obj = map{name:\"Bob\",age:25,xs:list(1,\"two\",null),nested:map{ok:true}};"
          "var text = json.stringify(obj);"
          "var back = json.parse(text);"
          "var total = back.age + back.name.length() + back.xs[0] + "
          "back.xs[1].length() + back.nested.ok + is_null(back.xs[2]);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON stringify Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 34.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should return null for invalid JSON parse input") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script = "var bad = json.parse(\"{bad\");"
                           "var ok = is_null(bad);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON invalid parse Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should stringify native bool and bytes as JSON") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var obj = map{ok:true,raw:bytes(\"Az\")};"
          "var text = json.stringify(obj);"
          "var back = json.parse(text);"
          "var schema = \"message Blob { bytes raw; }\";"
          "var json_in = \"{\\\"raw\\\":\\\"Az\\\"}\";"
          "var blob = json.bind(schema, json_in, \"Blob\");"
          "var json_out = json.emit(schema, blob, \"Blob\");"
          "var csv_out = csv.emit(schema, blob, \"Blob\");"
          "var json_ok = json.validate_ex(schema, json_in, \"Blob\");"
          "var csv_ok = csv.validate_ex(schema, csv_out, \"Blob\");"
          "var schema_total = (typeof(blob.raw) == \"bytes\") + is_bytes(blob.raw) + "
          "(json_out == json_in) + (csv_out == \"raw\\nAz\") + json_ok.ok + csv_ok.ok;"
          "var total = (typeof(obj.ok) == \"bool\") + is_bool(obj.ok) + "
          "(typeof(obj.raw) == \"bytes\") + is_bytes(obj.raw) + "
          "back.ok + back.raw[0] + back.raw[1] + schema_total;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON bool/bytes stringify Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 198.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should query XML through XPath into native containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var doc = \"<orders><order id=\\\"a\\\"><price>10</price><qty>2</qty></order>"
          "<order id=\\\"b\\\"><price>3</price><qty>5</qty></order></orders>\";"
          "var prices = xml.query(doc, \"//price\");"
          "var attrs = xml.query(doc, \"//@id\");"
          "var n = xml.count(doc, \"//order\");"
          "var first_text = xml.text(doc, \"//price\");"
          "var first_name = prices[0].name;"
          "var first_type = prices[0].type;"
          "var second_attr = attrs[1].text;"
          "var score = n + prices.length() + attrs.length() + first_text.length() + "
          "(second_attr == \"b\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("XML XPath Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "score"), 9.0, 0.001);
      check_str_eq(ts_get_str(ctx, "first_text"), "10");
      check_str_eq(ts_get_str(ctx, "first_name"), "price");
      check_str_eq(ts_get_str(ctx, "first_type"), "element");
      check_str_eq(ts_get_str(ctx, "second_attr"), "b");

      turbo_script_free(ctx);
    }

    it("should expose datetime parser functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var dt = datetime.parse(\"Sat, 04 Mar 2006 13:27:54 GMT\");"
          "var ts = datetime.to_time(dt);"
          "var direct = parser.datetime_to_time(\"2006-03-14T13:27:54+03:45\");"
          "var text = datetime.format_rfc822(ts);"
          "var bad = datetime.parse(\"not-a-date\");"
          "var kind_ok = typeof(dt) == \"datetime\" && is_datetime(dt);"
          "var schema = \"message Event { datetime at; }\";"
          "var json_in = \"{\\\"at\\\":\\\"Sat, 04 Mar 2006 13:27:54 GMT\\\"}\";"
          "var event = json.bind(schema, json_in, \"Event\");"
          "var dt_json = json.stringify(event.at);"
          "var json_out = json.emit(schema, event, \"Event\");"
          "var json_ok = json.validate_ex(schema, json_in, \"Event\");"
          "var json_bad = json.validate_ex(schema, \"{\\\"at\\\":\\\"not-a-date\\\"}\", \"Event\");"
          "var csv_out = csv.emit(schema, event, \"Event\");"
          "var csv_ok = csv.validate_ex(schema, csv_out, \"Event\");"
          "var flow_ok = (typeof(event.at) == \"datetime\") + "
          "(dt_json == \"\\\"Sat, 04 Mar 2006 13:27:54 GMT\\\"\") + "
          "(json_out == json_in) + json_ok.ok + (json_bad.ok == 0) + "
          "csv_ok.ok + (csv_out == \"at\\n\\\"Sat, 04 Mar 2006 13:27:54 GMT\\\"\");"
          "var total = dt.year + dt.month + dt.day + dt.hour + dt.minute + "
          "dt.second + dt.has_tz + dt.tz_offset + is_null(bad) + kind_ok;"
          "var text_len = text.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Datetime parser Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 2110.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ts"), 1141478874.0, 0.001);
      check_float_eq(ts_get_num(ctx, "direct"), 1142329374.0, 0.001);
      check_float_eq(ts_get_num(ctx, "flow_ok"), 7.0, 0.001);
      check_float_gt(ts_get_num(ctx, "text_len"), 20.0);

      turbo_script_free(ctx);
    }

    it("should expose native date time and duration values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var d = date.parse(\"2026-06-28\");"
          "var t = time.parse(\"09:30:05.123\");"
          "var dur = duration.parse(\"1h30m5s250ms\");"
          "var json = json.stringify(list(d,t,dur));"
          "var text = `${d}|${t}|${dur}`;"
          "var type_score = (typeof(d) == \"date\") + is_date(d) + "
          "(typeof(t) == \"time\") + is_time(t) + "
          "(typeof(dur) == \"duration\") + is_duration(dur);"
          "var member_score = (d.year == 2026) + (d.month == 6) + (d.day == 28) + "
          "(t.hour == 9) + (t.minute == 30) + (t.second == 5) + "
          "(t.millisecond == 123) + (dur.milliseconds == 5405250) + "
          "(dur.seconds == 5405.25);"
          "var string_fn_score = (date.to_string(d) == \"2026-06-28\") + "
          "(time.to_string(t) == \"09:30:05.123\") + "
          "(duration.to_string(dur) == \"1:30:05.250\");"
          "var json_ok = json == \"[\\\"2026-06-28\\\",\\\"09:30:05.123\\\",\\\"1:30:05.250\\\"]\";"
          "var template_ok = text == \"2026-06-28|09:30:05.123|1:30:05.250\";";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Temporal values Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "type_score"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "member_score"), 9.0, 0.001);
      check_float_eq(ts_get_num(ctx, "string_fn_score"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "json_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "template_ok"), 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind date time and duration through TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Event { date d; time t; duration span; }\";"
          "var json_in = \"{\\\"d\\\":\\\"2026-06-28\\\","
          "\\\"t\\\":\\\"09:30:05.123\\\",\\\"span\\\":\\\"1h30m5s250ms\\\"}\";"
          "var event = json.bind(schema, json_in, \"Event\");"
          "var json_out = json.emit(schema, event, \"Event\");"
          "var rebound = json.bind(schema, json_out, \"Event\");"
          "var csv_out = csv.emit(schema, event, \"Event\");"
          "var csv_back = csv.bind(schema, csv_out, 0, \"Event\");"
          "var xml_in = \"<event><d>2026-06-28</d><t>09:30:05.123</t>\" + "
          "\"<span>1h30m5s250ms</span></event>\";"
          "var xml_event = xml.bind(schema, xml_in, \"Event\");"
          "var json_ok = json.validate_ex(schema, json_out, \"Event\");"
          "var csv_ok = csv.validate_ex(schema, csv_out, \"Event\");"
          "var xml_ok = xml.validate_ex(schema, xml_in, \"Event\");"
          "var bad = json.validate_ex(schema, "
          "\"{\\\"d\\\":\\\"2026-02-31\\\",\\\"t\\\":\\\"09:30:05.123\\\","
          "\\\"span\\\":\\\"1h30m5s250ms\\\"}\", \"Event\");"
          "var type_score = (typeof(event.d) == \"date\") + "
          "(typeof(event.t) == \"time\") + (typeof(event.span) == \"duration\") + "
          "(typeof(xml_event.d) == \"date\") + (typeof(csv_back.t) == \"time\");"
          "var value_score = (event.d.year == 2026) + (event.t.millisecond == 123) + "
          "(event.span.milliseconds == 5405250) + (rebound.span.seconds == 5405.25);"
          "var io_score = (json_out == \"{\\\"d\\\":\\\"2026-06-28\\\","
          "\\\"t\\\":\\\"09:30:05.123\\\",\\\"span\\\":\\\"1:30:05.250\\\"}\") + "
          "(csv_out == \"d,t,span\\n2026-06-28,09:30:05.123,1:30:05.250\") + "
          "json_ok.ok + csv_ok.ok + xml_ok.ok + (bad.ok == 0);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Temporal schema bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "type_score"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "value_score"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "io_score"), 6.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should expose native decimal values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var d = decimal.parse(\"123.4500\");"
          "var same = decimal.parse(\"123.45\");"
          "var json = json.stringify(list(d));"
          "var text = `${d}`;"
          "var type_score = (typeof(d) == \"decimal\") + is_decimal(d);"
          "var member_score = (d.mantissa == 12345) + (d.scale == 2) + "
          "(decimal.mantissa(d) == 12345) + (decimal.scale(d) == 2);"
          "var string_score = (decimal.to_string(d) == \"123.45\") + "
          "(d.to_string() == \"123.45\") + (json == \"[\\\"123.45\\\"]\") + "
          "(text == \"123.45\") + (d == same);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Decimal values Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "type_score"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "member_score"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "string_score"), 5.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind decimal through TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Quote { decimal price; }\";"
          "var json_in = \"{\\\"price\\\":\\\"123.4500\\\"}\";"
          "var quote = json.bind(schema, json_in, \"Quote\");"
          "var json_out = json.emit(schema, quote, \"Quote\");"
          "var csv_out = csv.emit(schema, quote, \"Quote\");"
          "var csv_back = csv.bind(schema, csv_out, 0, \"Quote\");"
          "var xml_in = \"<quote><price>-0.1250</price></quote>\";"
          "var xml_quote = xml.bind(schema, xml_in, \"Quote\");"
          "var json_ok = json.validate_ex(schema, json_out, \"Quote\");"
          "var csv_ok = csv.validate_ex(schema, csv_out, \"Quote\");"
          "var xml_ok = xml.validate_ex(schema, xml_in, \"Quote\");"
          "var bad = json.validate_ex(schema, \"{\\\"price\\\":\\\"bad\\\"}\", \"Quote\");"
          "var type_score = (typeof(quote.price) == \"decimal\") + "
          "(typeof(xml_quote.price) == \"decimal\") + (typeof(csv_back.price) == \"decimal\");"
          "var value_score = (quote.price.mantissa == 12345) + (quote.price.scale == 2) + "
          "(xml_quote.price.mantissa == -125) + (xml_quote.price.scale == 3);"
          "var io_score = (json_out == \"{\\\"price\\\":\\\"123.45\\\"}\") + "
          "(csv_out == \"price\\n123.45\") + json_ok.ok + csv_ok.ok + xml_ok.ok + "
          "(bad.ok == 0) + (bad.message == \"expected decimal but got string\");";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Decimal schema bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "type_score"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "value_score"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "io_score"), 7.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind XML through TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Event { uuid id; bytes raw; datetime at; }\";"
          "var xml_in = \"<event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id>\" + "
          "\"<raw>Az</raw><at>Sat, 04 Mar 2006 13:27:54 GMT</at></event>\";"
          "var many = \"<events>\" + xml_in + xml_in + \"</events>\";"
          "var event = xml.bind(schema, xml_in, \"Event\");"
          "var rows = xml.bind_all(schema, many, \"//event\", \"Event\");"
          "var ok = xml.validate_ex(schema, many, \"//event\", \"Event\");"
          "var bad = xml.validate_ex(schema, "
          "\"<event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id><raw>Az</raw><at>bad</at></event>\", "
          "\"Event\");"
          "var score = (typeof(event.id) == \"uuid\") + (typeof(event.raw) == \"bytes\") + "
          "(event.raw.length == 2) + (typeof(event.at) == \"datetime\") + "
          "(rows.length() == 2) + ok.ok + (bad.ok == 0);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("XML schema bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "score"), 7.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind JSON objects through a TBE schema") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Trade { double price; uint32 qty; bool active; string symbol; }\";"
          "var js = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":123.5,"
          "\\\"qty\\\":10,\\\"active\\\":true}\";"
          "var trade = json.bind(schema, js, \"Trade\");"
          "var total = trade.price * trade.qty + trade.active;"
          "var symbol_len = trade.symbol.length();"
          "var bool_checks = (typeof(trade.active) == \"bool\") + is_bool(trade.active);"
          "var object_checks = (typeof(trade) == \"object\") + is_object(trade);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 1236.0, 0.001);
      check_float_eq(ts_get_num(ctx, "symbol_len"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bool_checks"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "object_checks"), 2.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind JSON arrays through a TBE schema") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Trade { double price; uint32 qty; bool active; string symbol; }\";"
          "var js = \"[{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,"
          "\\\"qty\\\":2,\\\"active\\\":true},{\\\"symbol\\\":\\\"MSFT\\\","
          "\\\"price\\\":20.0,\\\"qty\\\":3,\\\"active\\\":false}]\";"
          "var rows = json.bind_all(schema, js, \"Trade\");"
          "var first = rows[0];"
          "var second = rows[1];"
          "var n = rows.length();"
          "var total = first.price * first.qty + second.price * second.qty;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON bind_all Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 81.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind nested JSON composites and arrays through a TBE schema") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"composite Header { uint32 venue; uint32 seq; } "
          "composite Point { uint32 x; uint32 y; } "
          "message Trade { Header header; Point[2] points; uint32[2] levels; "
          "double price; string symbol; }\";"
          "var js = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":5.5,"
          "\\\"header\\\":{\\\"venue\\\":7,\\\"seq\\\":8},"
          "\\\"points\\\":[{\\\"x\\\":1,\\\"y\\\":2},{\\\"x\\\":3,\\\"y\\\":4}],"
          "\\\"levels\\\":[10,20]}\";"
          "var trade = json.bind(schema, js, \"Trade\");"
          "var score = trade.header.venue + trade.header.seq + trade.points[1].x + "
          "trade.levels[0] + trade.levels[1] + trade.price;"
          "var point_count = trade.points.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON nested bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "score"), 53.5, 0.001);
      check_float_eq(ts_get_num(ctx, "point_count"), 2.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind JSON object fields to schema maps") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"message Attrs { map<string,int32> attrs; }\";"
          "var js = \"{\\\"attrs\\\":{\\\"x\\\":30,\\\"y\\\":40}}\";"
          "var bound = json.bind(schema, js, \"Attrs\");"
          "var total = bound.attrs.x + bound.attrs.y;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON map bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 70.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate JSON list and set containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Containers { list<uint32> values; set<string> tags; }\";"
          "var js = \"{\\\"values\\\":[10,20,30],\\\"tags\\\":[\\\"a\\\",\\\"bb\\\"]}\";"
          "var bad = \"{\\\"values\\\":[10,\\\"bad\\\"],\\\"tags\\\":[\\\"a\\\"]}\";"
          "var row = json.bind(schema_text, js, \"Containers\");"
          "var ok = json.validate(schema_text, js, \"Containers\");"
          "var bad_detail = json.validate_ex(schema_text, bad, \"Containers\");"
          "var emitted = json.emit(schema_text, row, \"Containers\");"
          "var rebound = json.bind(schema_text, emitted, \"Containers\");"
          "var total = row.values[0] + row.values[2] + row.tags[1].length();"
          "var rebound_total = rebound.values[0] + rebound.values[2] + rebound.tags[1].length();"
          "var value_count = row.values.length();"
          "var tag_count = row.tags.length();"
          "var bad_ok = bad_detail.ok;"
          "var bad_path_len = bad_detail.path.length();"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON list/set bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 42.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rebound_total"), 42.0, 0.001);
      check_float_eq(ts_get_num(ctx, "value_count"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tag_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_path_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind JSON arrays to schema groups") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema = \"group Level { uint64 price; uint32 qty; } "
          "message Book { uint32 seq; group<Level> bids; string symbol; }\";"
          "var js = \"{\\\"seq\\\":7,\\\"symbol\\\":\\\"AAPL\\\","
          "\\\"bids\\\":[{\\\"price\\\":100,\\\"qty\\\":10},"
          "{\\\"price\\\":200,\\\"qty\\\":20}]}\";"
          "var book = json.bind(schema, js, \"Book\");"
          "var total = book.seq + book.bids[0].qty + book.bids[1].price;"
          "var n = book.bids.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON group bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "total"), 217.0, 0.001);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should skip invalid JSON container shapes instead of binding empty containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"group Level { uint64 price; uint32 qty; } "
          "message Batch { uint32[2] levels; group<Level> bids; "
          "list<uint32> values; map<string,uint32> attrs; }\";"
          "var rows_json = \"["
          "{\\\"levels\\\":[1,2],\\\"values\\\":[3],\\\"attrs\\\":{\\\"x\\\":4},"
          "\\\"bids\\\":[{\\\"price\\\":5,\\\"qty\\\":6}]},"
          "{\\\"levels\\\":[1],\\\"values\\\":[3],\\\"attrs\\\":{\\\"x\\\":4},"
          "\\\"bids\\\":[{\\\"price\\\":5,\\\"qty\\\":6}]},"
          "{\\\"levels\\\":[1,2],\\\"values\\\":{},\\\"attrs\\\":{\\\"x\\\":4},"
          "\\\"bids\\\":[{\\\"price\\\":5,\\\"qty\\\":6}]},"
          "{\\\"levels\\\":[1,2],\\\"values\\\":[3],\\\"attrs\\\":[],"
          "\\\"bids\\\":[{\\\"price\\\":5,\\\"qty\\\":6}]},"
          "{\\\"levels\\\":[1,2],\\\"values\\\":[3],\\\"attrs\\\":{\\\"x\\\":4},"
          "\\\"bids\\\":{}},"
          "{\\\"levels\\\":[7,8],\\\"values\\\":[9],\\\"attrs\\\":{\\\"y\\\":10},"
          "\\\"bids\\\":[{\\\"price\\\":11,\\\"qty\\\":12}]}]\";"
          "var rows = json.bind_all(schema_text, rows_json, \"Batch\");"
          "var ok = json.validate(schema_text, rows_json, \"Batch\");"
          "var detail = json.validate_ex(schema_text, rows_json, \"Batch\");"
          "var emitted = json.emit(schema_text, rows, \"Batch\");"
          "var rebound = json.bind_all(schema_text, emitted, \"Batch\");"
          "var total = rows.length() + rebound.length() + rows[0].levels[1] + "
          "rows[0].values[0] + rows[0].attrs.x + rows[0].bids[0].qty + "
          "rows[1].levels[0] + rows[1].attrs.y + rows[1].bids[0].qty;"
          "var detail_ok = detail.ok;"
          "var detail_message_len = detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Invalid JSON container shape Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "detail_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "detail_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 48.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should reuse parsed schema handles for JSON binding") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Trade { double price; uint32 qty; string symbol; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var one = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2}\";"
          "var many = \"[{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2},"
          "{\\\"symbol\\\":\\\"MSFT\\\",\\\"price\\\":20.0,\\\"qty\\\":3}]\";"
          "var trade = json.bind_schema(schema_id, one, \"Trade\");"
          "var rows = json.bind_all_schema(schema_id, many, \"Trade\");"
          "var close_rc = schema.close(schema_id);"
          "var total = trade.price * trade.qty + rows[1].price * rows[1].qty;"
          "var n = rows.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON bind_schema Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "schema_id") >= 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "n"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 81.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should emit JSON from schema-bound values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"composite Header { uint32 venue; uint32 seq; } "
          "message Trade { Header header; double price; uint32 qty; string symbol; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var js = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2,"
          "\\\"header\\\":{\\\"venue\\\":7,\\\"seq\\\":8}}\";"
          "var trade = json.bind_schema(schema_id, js, \"Trade\");"
          "var emitted = json.emit_schema(schema_id, trade, \"Trade\");"
          "var rebound = json.bind_schema(schema_id, emitted, \"Trade\");"
          "var close_rc = schema.close(schema_id);"
          "var total = rebound.price * rebound.qty + rebound.header.seq + rebound.symbol.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON emit Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 33.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should validate JSON against TBE schemas") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"composite Header { uint32 venue; uint32 seq; } "
          "message Trade { Header header; double price; uint32 qty; string symbol; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var good = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2,"
          "\\\"header\\\":{\\\"venue\\\":7,\\\"seq\\\":8}}\";"
          "var missing = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":10.5,\\\"qty\\\":2}\";"
          "var bad = \"{\\\"symbol\\\":\\\"AAPL\\\",\\\"price\\\":\\\"bad\\\",\\\"qty\\\":2,"
          "\\\"header\\\":{\\\"venue\\\":7,\\\"seq\\\":8}}\";"
          "var ok = json.validate_schema(schema_id, good, \"Trade\");"
          "var missing_ok = json.validate_schema(schema_id, missing, \"Trade\");"
          "var bad_ok = json.validate(schema_text, bad, \"Trade\");"
          "var missing_detail = json.validate_ex_schema(schema_id, missing, \"Trade\");"
          "var bad_detail = json.validate_ex(schema_text, bad, \"Trade\");"
          "var missing_path_len = missing_detail.path.length();"
          "var missing_message_len = missing_detail.message.length();"
          "var bad_path_len = bad_detail.path.length();"
          "var bad_message_len = bad_detail.message.length();"
          "var close_rc = schema.close(schema_id);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON validate Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_path_len"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_path_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should reflect TBE schema types and fields") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "composite Header { uint32 venue; uint32 seq; } "
          "group Level { uint64 price; uint32 qty; } "
          "message Book { Header header; uint32[2] levels; "
          "Side side; group<Level> bids; map<string,int32> attrs; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var types = schema.types(schema_id);"
          "var fields = schema.fields(schema_id, \"Book\");"
          "var fields_from_text = schema.fields(schema_text, \"Header\");"
          "var type_count = types.length();"
          "var field_count = fields.length();"
          "var header_field_count = fields_from_text.length();"
          "var first_type_len = types[0].name.length();"
          "var header_kind_len = fields[0].kind.length();"
          "var array_inner_len = fields[1].inner_type.length();"
          "var group_type_len = fields[3].group_type.length();"
          "var map_value_len = fields[4].value_type.length();"
          "var reflection_objects = is_object(types[0]) + is_object(fields[0]) + "
          "is_object(fields_from_text[0]);"
          "var has_book = schema.type_exists(schema_id, \"Book\");"
          "var has_missing = schema.type_exists(schema_text, \"Missing\");"
          "var close_rc = schema.close(schema_id);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Schema reflect Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "type_count"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "field_count"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "header_field_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "first_type_len"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "header_kind_len"), 9.0, 0.001);
      check_float_eq(ts_get_num(ctx, "array_inner_len"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "group_type_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "map_value_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "reflection_objects"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "has_book"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "has_missing"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should reflect TBE enums attributes and layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"schema Market [id(7), version(2), byte_order(little)]; "
          "enum Side <uint8> { Buy = 1; Sell = 2; } "
          "[id(100), version(1)] message Book { uint32 seq; Side side; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var enums = schema.enums(schema_id);"
          "var root_attrs = schema.attributes(schema_id);"
          "var book_attrs = schema.attributes(schema_id, \"Book\");"
          "var root_layout = schema.layout(schema_id);"
          "var book_layout = schema.layout(schema_id, \"Book\");"
          "var fields = schema.fields(schema_id, \"Book\");"
          "var enum_count = enums.length();"
          "var enum_item_count = enums[0].item_count;"
          "var enum_name_len = enums[0].name.length();"
          "var enum_underlying_len = enums[0].underlying_type.length();"
          "var enum_first_value_len = enums[0].items[0].value.length();"
          "var root_id_len = root_attrs.id.length();"
          "var book_id_len = book_attrs.id.length();"
          "var root_order_len = root_layout.wire_byte_order.length();"
          "var book_kind_len = book_layout.kind.length();"
          "var book_fixed = book_layout.fixed_block_size;"
          "var field_kind_len = fields[1].kind.length();"
          "var reflection_objects = is_object(enums[0]) + is_object(root_attrs) + "
          "is_object(book_attrs) + is_object(root_layout) + is_object(book_layout) + "
          "is_object(fields[0]);"
          "var close_rc = schema.close(schema_id);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Schema extended reflect Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "enum_count"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "enum_item_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "enum_name_len"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "enum_underlying_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "enum_first_value_len"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "root_id_len"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "book_id_len"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "root_order_len"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "book_kind_len"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "book_fixed"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "field_kind_len"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "reflection_objects"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should reflect TBE flags and unions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"flags Permissions <uint8> { Read; Write; Execute; } "
          "message Success { uint32 code; } "
          "message Error { uint32 code; } "
          "union Result { Success success; Error error; }\";"
          "var schema_id = schema.parse(schema_text);"
          "var types = schema.types(schema_id);"
          "var flags = schema.flags(schema_id);"
          "var unions = schema.unions(schema_id);"
          "var fields = schema.fields(schema_id, \"Result\");"
          "var layout = schema.layout(schema_id, \"Result\");"
          "var has_result = schema.type_exists(schema_id, \"Result\");"
          "var type_count = types.length();"
          "var flags_count = flags.length();"
          "var flag_item_count = flags[0].item_count;"
          "var flag_kind_len = schema.layout(schema_id, \"Permissions\").kind.length();"
          "var third_flag_value_len = flags[0].items[2].value.length();"
          "var union_count = unions.length();"
          "var variant_count = unions[0].variant_count;"
          "var union_field_count = fields.length();"
          "var union_kind_len = layout.kind.length();"
          "var reflection_objects = is_object(types[0]) + is_object(flags[0]) + "
          "is_object(unions[0]) + is_object(fields[0]) + is_object(layout);"
          "var close_rc = schema.close(schema_id);";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Schema flags/union reflect Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "type_count"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "flags_count"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "flag_item_count"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "flag_kind_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "third_flag_value_len"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "union_count"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "variant_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "union_field_count"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "union_kind_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "reflection_objects"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "has_result"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind and validate JSON enum and flags symbolic values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Order { Side side; Permissions perms; }\";"
          "var good = \"{\\\"side\\\":\\\"Buy\\\",\\\"perms\\\":[\\\"Read\\\",\\\"Write\\\"]}\";"
          "var good2 = \"{\\\"side\\\":2,\\\"perms\\\":\\\"Read|Execute\\\"}\";"
          "var bad = \"{\\\"side\\\":\\\"Hold\\\",\\\"perms\\\":[\\\"Read\\\",\\\"Missing\\\"]}\";"
          "var row = json.bind(schema_text, good, \"Order\");"
          "var row2 = json.bind(schema_text, good2, \"Order\");"
          "var good_ok = json.validate(schema_text, good, \"Order\");"
          "var good2_ok = json.validate(schema_text, good2, \"Order\");"
          "var bad_detail = json.validate_ex(schema_text, bad, \"Order\");"
          "var total = row.side + row.perms + row2.side + row2.perms;"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON enum/flags bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "good2_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 11.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind and validate CSV enum and flags symbolic values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Order { Side side; Permissions perms; }\";"
          "var good = \"side,perms\\nBuy,Read|Write\";"
          "var numeric = \"side,perms\\n2,5\";"
          "var bad = \"side,perms\\nHold,Read|Missing\";"
          "var row = csv.bind(schema_text, good, 0, \"Order\");"
          "var row2 = csv.bind(schema_text, numeric, 0, \"Order\");"
          "var good_ok = csv.validate(schema_text, good, \"Order\");"
          "var numeric_ok = csv.validate(schema_text, numeric, \"Order\");"
          "var bad_detail = csv.validate_ex(schema_text, bad, \"Order\");"
          "var total = row.side + row.perms + row2.side + row2.perms;"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV enum/flags bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "numeric_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 11.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind and validate enum and flags symbolic values in schema containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Batch { list<Side> sides; map<string,Permissions> perms; }\";"
          "var js = \"{\\\"sides\\\":[\\\"Buy\\\",2],"
          "\\\"perms\\\":{\\\"a\\\":[\\\"Read\\\",\\\"Write\\\"],\\\"b\\\":\\\"Read|Execute\\\"}}\";"
          "var csv_text = \"sides[0],sides[1],perms.a,perms.b\\n"
          "Buy,Sell,Read|Write,\\\"Read,Execute\\\"\";"
          "var bad_json = \"{\\\"sides\\\":[\\\"Buy\\\",\\\"Hold\\\"],"
          "\\\"perms\\\":{\\\"a\\\":\\\"Read|Missing\\\"}}\";"
          "var bad_csv = \"sides[0],sides[1],perms.a\\nBuy,Hold,Read|Missing\";"
          "var jrow = json.bind(schema_text, js, \"Batch\");"
          "var crow = csv.bind(schema_text, csv_text, 0, \"Batch\");"
          "var json_ok = json.validate(schema_text, js, \"Batch\");"
          "var csv_ok = csv.validate(schema_text, csv_text, \"Batch\");"
          "var bad_json_detail = json.validate_ex(schema_text, bad_json, \"Batch\");"
          "var bad_csv_detail = csv.validate_ex(schema_text, bad_csv, \"Batch\");"
          "var json_total = jrow.sides[0] + jrow.sides[1] + jrow.perms.a + jrow.perms.b;"
          "var csv_total = crow.sides[0] + crow.sides[1] + crow.perms.a + crow.perms.b;"
          "var bad_total = bad_json_detail.ok + bad_csv_detail.ok;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Schema container enum/flags bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "json_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_total"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "json_total"), 11.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_total"), 11.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind validate and emit top-level JSON enum and flags values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Order { Side side; Permissions perms; }\";"
          "var side_json = \"\\\"Buy\\\"\";"
          "var flags_json = \"[\\\"Read\\\",\\\"Execute\\\"]\";"
          "var bad_json = \"\\\"Hold\\\"\";"
          "var side = json.bind(schema_text, side_json, \"Side\");"
          "var flags = json.bind(schema_text, flags_json, \"Permissions\");"
          "var qty = json.bind(schema_text, \"42\", \"uint32\");"
          "var sides = json.bind_all(schema_text, \"[\\\"Buy\\\",2]\", \"Side\");"
          "var side_out = json.emit(schema_text, side, \"Side\");"
          "var flags_out = json.emit(schema_text, flags, \"Permissions\");"
          "var qty_out = json.emit(schema_text, qty, \"uint32\");"
          "var flags_rebound = json.bind(schema_text, flags_out, \"Permissions\");"
          "var side_ok = json.validate(schema_text, side_json, \"Side\");"
          "var flags_ok = json.validate(schema_text, flags_json, \"Permissions\");"
          "var qty_ok = json.validate(schema_text, \"42\", \"uint32\");"
          "var bad_detail = json.validate_ex(schema_text, bad_json, \"Side\");"
          "var total = side + flags + qty + sides[0] + sides[1] + flags_rebound;"
          "var emit_len = side_out.length() + flags_out.length() + qty_out.length();"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Top-level JSON enum/flags bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "side_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "flags_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "qty_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 56.0, 0.001);
      check_float_eq(ts_get_num(ctx, "emit_len"), 4.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should skip invalid JSON scalar bindings instead of coercing to zero") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; }\";"
          "var nums = json.bind_all(schema_text, \"[1,\\\"bad\\\",2]\", \"uint32\");"
          "var sides = json.bind_all(schema_text, \"[\\\"Buy\\\",\\\"Hold\\\",\\\"Sell\\\"]\", \"Side\");"
          "var perms = json.bind_all(schema_text, \"[\\\"Read|Execute\\\",\\\"Read|Missing\\\",3]\", "
          "\"Permissions\");"
          "var good_ok = json.validate(schema_text, \"[1,2]\", \"uint32\");"
          "var bad_num_detail = json.validate_ex(schema_text, \"\\\"bad\\\"\", \"uint32\");"
          "var bad_side_detail = json.validate_ex(schema_text, \"\\\"Hold\\\"\", \"Side\");"
          "var bad_perms_detail = json.validate_ex(schema_text, \"\\\"Read|Missing\\\"\", \"Permissions\");"
          "var total = nums.length() + nums[0] + nums[1] + "
          "sides.length() + sides[0] + sides[1] + "
          "perms.length() + perms[0] + perms[1];"
          "var bad_total = bad_num_detail.ok + bad_side_detail.ok + bad_perms_detail.ok;"
          "var bad_message_total = bad_num_detail.message.length() + "
          "bad_side_detail.message.length() + bad_perms_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Invalid JSON scalar bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_total"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_total") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 20.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should skip invalid schema records instead of binding partial maps") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Trade { double price; uint32 qty; optional uint32 venue; }\";"
          "var json_rows = \"[{\\\"price\\\":10,\\\"qty\\\":2},"
          "{\\\"price\\\":\\\"bad\\\",\\\"qty\\\":3},"
          "{\\\"price\\\":5,\\\"qty\\\":4,\\\"venue\\\":\\\"oops\\\"},"
          "{\\\"price\\\":7,\\\"qty\\\":8}]\";"
          "var csv_rows = \"price,qty,venue\\n10,2,\\nbad,3,\\n5,4,oops\\n7,8,\";"
          "var jrows = json.bind_all(schema_text, json_rows, \"Trade\");"
          "var crows = csv.bind_all(schema_text, csv_rows, \"Trade\");"
          "var json_ok = json.validate(schema_text, json_rows, \"Trade\");"
          "var csv_ok = csv.validate(schema_text, csv_rows, \"Trade\");"
          "var json_detail = json.validate_ex(schema_text, json_rows, \"Trade\");"
          "var csv_detail = csv.validate_ex(schema_text, csv_rows, \"Trade\");"
          "var json_total = jrows.length() + jrows[0].price + jrows[1].qty;"
          "var csv_total = crows.length() + crows[0].price + crows[1].qty;"
          "var bad_total = json_ok + csv_ok + json_detail.ok + csv_detail.ok;"
          "var bad_message_total = json_detail.message.length() + csv_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Invalid schema record bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "bad_total"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_total") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "json_total"), 20.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_total"), 20.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind validate and emit top-level CSV scalar enum and flags values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; }\";"
          "var side_csv = \"value\\nBuy\";"
          "var perms_csv = \"value\\nRead|Execute\";"
          "var price_csv = \"price\\n12.5\";"
          "var sides_csv = \"value\\nBuy\\nSell\";"
          "var bad_csv = \"value\\nHold\";"
          "var side = csv.bind(schema_text, side_csv, 0, \"Side\");"
          "var perms = csv.bind(schema_text, perms_csv, 0, \"Permissions\");"
          "var price = csv.bind(schema_text, price_csv, 0, \"double\");"
          "var sides = csv.bind_all(schema_text, sides_csv, \"Side\");"
          "var emitted_sides = csv.emit(schema_text, sides, \"Side\");"
          "var emitted_perms = csv.emit(schema_text, perms, \"Permissions\");"
          "var rebound_sides = csv.bind_all(schema_text, emitted_sides, \"Side\");"
          "var rebound_perms = csv.bind(schema_text, emitted_perms, 0, \"Permissions\");"
          "var side_ok = csv.validate(schema_text, side_csv, \"Side\");"
          "var perms_ok = csv.validate(schema_text, perms_csv, \"Permissions\");"
          "var price_ok = csv.validate(schema_text, price_csv, \"double\");"
          "var emitted_ok = csv.validate(schema_text, emitted_sides, \"Side\");"
          "var bad_detail = csv.validate_ex(schema_text, bad_csv, \"Side\");"
          "var ok_total = side_ok + perms_ok + price_ok + emitted_ok;"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();"
          "var total = side + perms + price + sides[0] + sides[1] + "
          "rebound_sides[0] + rebound_sides[1] + rebound_perms;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Top-level CSV scalar Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok_total"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 29.5, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind validate and emit JSON union values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Success { uint32 code; } "
          "message Error { uint32 code; } "
          "union Result { Success success; Error error; }\";"
          "var good = \"{\\\"success\\\":{\\\"code\\\":200}}\";"
          "var empty = \"{}\";"
          "var many = \"{\\\"success\\\":{\\\"code\\\":200},\\\"error\\\":{\\\"code\\\":500}}\";"
          "var missing = \"{\\\"missing\\\":{\\\"code\\\":1}}\";"
          "var r = json.bind(schema_text, good, \"Result\");"
          "var emitted = json.emit(schema_text, r, \"Result\");"
          "var rebound = json.bind(schema_text, emitted, \"Result\");"
          "var good_ok = json.validate(schema_text, good, \"Result\");"
          "var empty_ok = json.validate(schema_text, empty, \"Result\");"
          "var many_ok = json.validate(schema_text, many, \"Result\");"
          "var missing_detail = json.validate_ex(schema_text, missing, \"Result\");"
          "var total = r.success.code + rebound.success.code;"
          "var missing_ok = missing_detail.ok;"
          "var missing_path_len = missing_detail.path.length();"
          "var missing_message_len = missing_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON union bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "empty_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "many_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_path_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "missing_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 400.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind validate and emit top-level JSON union lists") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Success { uint32 code; } "
          "message Error { uint32 code; } "
          "union Result { Success success; Error error; }\";"
          "var good = \"[{\\\"success\\\":{\\\"code\\\":200}},"
          "{\\\"error\\\":{\\\"code\\\":500}}]\";"
          "var bad = \"[{\\\"success\\\":{\\\"code\\\":200}},"
          "{\\\"success\\\":{\\\"code\\\":1},\\\"error\\\":{\\\"code\\\":2}}]\";"
          "var rows = json.bind_all(schema_text, good, \"Result\");"
          "var emitted = json.emit(schema_text, rows, \"Result\");"
          "var rebound = json.bind_all(schema_text, emitted, \"Result\");"
          "var good_ok = json.validate(schema_text, good, \"Result\");"
          "var emitted_ok = json.validate(schema_text, emitted, \"Result\");"
          "var bad_detail = json.validate_ex(schema_text, bad, \"Result\");"
          "var total = rows.length() + rebound.length() + rows[0].success.code + "
          "rows[1].error.code + rebound[0].success.code + rebound[1].error.code;"
          "var ok_total = good_ok + emitted_ok;"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON union list Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok_total"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 1404.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind validate and emit JSON union scalar payloads") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "union Choice { Side side; Permissions perms; uint32 qty; }\";"
          "var side_json = \"{\\\"side\\\":\\\"Buy\\\"}\";"
          "var perms_json = \"{\\\"perms\\\":[\\\"Read\\\",\\\"Execute\\\"]}\";"
          "var qty_json = \"{\\\"qty\\\":42}\";"
          "var bad_json = \"{\\\"side\\\":\\\"Hold\\\"}\";"
          "var side = json.bind(schema_text, side_json, \"Choice\");"
          "var perms = json.bind(schema_text, perms_json, \"Choice\");"
          "var qty = json.bind(schema_text, qty_json, \"Choice\");"
          "var side_out = json.emit(schema_text, side, \"Choice\");"
          "var perms_out = json.emit(schema_text, perms, \"Choice\");"
          "var qty_out = json.emit(schema_text, qty, \"Choice\");"
          "var rebound_side = json.bind(schema_text, side_out, \"Choice\");"
          "var rebound_perms = json.bind(schema_text, perms_out, \"Choice\");"
          "var rebound_qty = json.bind(schema_text, qty_out, \"Choice\");"
          "var side_ok = json.validate(schema_text, side_json, \"Choice\");"
          "var perms_ok = json.validate(schema_text, perms_json, \"Choice\");"
          "var qty_ok = json.validate(schema_text, qty_json, \"Choice\");"
          "var emitted_ok = json.validate(schema_text, side_out, \"Choice\") + "
          "json.validate(schema_text, perms_out, \"Choice\") + "
          "json.validate(schema_text, qty_out, \"Choice\");"
          "var bad_detail = json.validate_ex(schema_text, bad_json, \"Choice\");"
          "var total = side.side + perms.perms + qty.qty + "
          "rebound_side.side + rebound_perms.perms + rebound_qty.qty;"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON union scalar payload Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "side_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "perms_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "qty_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "emitted_ok"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 96.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind JSON union fields inside messages") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message Success { uint32 code; } "
          "message Error { uint32 code; } "
          "union Result { Success success; Error error; } "
          "message Envelope { uint32 seq; Result result; }\";"
          "var good = \"{\\\"seq\\\":7,\\\"result\\\":{\\\"error\\\":{\\\"code\\\":500}}}\";"
          "var bad = \"{\\\"seq\\\":7,\\\"result\\\":{\\\"success\\\":{\\\"code\\\":200},"
          "\\\"error\\\":{\\\"code\\\":500}}}\";"
          "var env_msg = json.bind(schema_text, good, \"Envelope\");"
          "var emitted = json.emit(schema_text, env_msg, \"Envelope\");"
          "var good_ok = json.validate(schema_text, good, \"Envelope\");"
          "var emitted_ok = json.validate(schema_text, emitted, \"Envelope\");"
          "var bad_detail = json.validate_ex(schema_text, bad, \"Envelope\");"
          "var bad_ok = bad_detail.ok;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("JSON union field Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "emitted_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate CSV union fields inside messages") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Success { uint32 code; } "
          "union Choice { Side side; Permissions perms; Success success; } "
          "message Envelope { uint32 seq; Choice choice; }\";"
          "var side_csv = \"seq,choice.side\\n7,Buy\";"
          "var perms_csv = \"seq,choice.perms\\n8,Read|Execute\";"
          "var success_csv = \"seq,choice.success.code\\n9,200\";"
          "var missing_csv = \"seq\\n10\";"
          "var many_csv = \"seq,choice.side,choice.perms\\n11,Buy,Read\";"
          "var bad_csv = \"seq,choice.side\\n12,Hold\";"
          "var side = csv.bind(schema_text, side_csv, 0, \"Envelope\");"
          "var perms = csv.bind(schema_text, perms_csv, 0, \"Envelope\");"
          "var success = csv.bind(schema_text, success_csv, 0, \"Envelope\");"
          "var emitted = csv.emit(schema_text, side, \"Envelope\");"
          "var rebound = csv.bind(schema_text, emitted, 0, \"Envelope\");"
          "var side_ok = csv.validate(schema_text, side_csv, \"Envelope\");"
          "var perms_ok = csv.validate(schema_text, perms_csv, \"Envelope\");"
          "var success_ok = csv.validate(schema_text, success_csv, \"Envelope\");"
          "var emitted_ok = csv.validate(schema_text, emitted, \"Envelope\");"
          "var missing_detail = csv.validate_ex(schema_text, missing_csv, \"Envelope\");"
          "var many_detail = csv.validate_ex(schema_text, many_csv, \"Envelope\");"
          "var bad_detail = csv.validate_ex(schema_text, bad_csv, \"Envelope\");"
          "var total = side.seq + side.choice.side + perms.choice.perms + "
          "success.choice.success.code + rebound.choice.side;"
          "var ok_total = side_ok + perms_ok + success_ok + emitted_ok;"
          "var bad_total = missing_detail.ok + many_detail.ok + bad_detail.ok;"
          "var bad_message_len = missing_detail.message.length() + "
          "many_detail.message.length() + bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV union field Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok_total"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_total"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 214.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate CSV union values in containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Success { uint32 code; } "
          "union Choice { Side side; Permissions perms; Success success; } "
          "message Batch { list<Choice> choices; map<string,Choice> by_key; }\";"
          "var good = \"choices[0].side,choices[1].success.code,by_key.a.perms,by_key.b.side\\n"
          "Buy,200,Read|Write,Sell\";"
          "var bad = \"choices[0].side,choices[0].perms,by_key.a.perms\\n"
          "Buy,Read,Read|Missing\";"
          "var row = csv.bind(schema_text, good, 0, \"Batch\");"
          "var emitted = csv.emit(schema_text, row, \"Batch\");"
          "var rebound = csv.bind(schema_text, emitted, 0, \"Batch\");"
          "var good_ok = csv.validate(schema_text, good, \"Batch\");"
          "var emitted_ok = csv.validate(schema_text, emitted, \"Batch\");"
          "var bad_detail = csv.validate_ex(schema_text, bad, \"Batch\");"
          "var total = row.choices[0].side + row.choices[1].success.code + "
          "row.by_key.a.perms + row.by_key.b.side + rebound.choices[0].side;"
          "var bad_ok = bad_detail.ok;"
          "var bad_message_len = bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV union container Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "emitted_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 207.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind emit and validate top-level CSV union values") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"enum Side { Buy = 1; Sell = 2; } "
          "flags Permissions { Read; Write; Execute; } "
          "message Success { uint32 code; } "
          "union Choice { Side side; Permissions perms; Success success; }\";"
          "var side_csv = \"side\\nBuy\";"
          "var perms_csv = \"perms\\nRead|Execute\";"
          "var success_csv = \"success.code\\n200\";"
          "var rows_csv = \"side\\nBuy\\nSell\";"
          "var many_csv = \"side,perms\\nBuy,Read\";"
          "var bad_csv = \"side\\nHold\";"
          "var side = csv.bind(schema_text, side_csv, 0, \"Choice\");"
          "var perms = csv.bind(schema_text, perms_csv, 0, \"Choice\");"
          "var success = csv.bind(schema_text, success_csv, 0, \"Choice\");"
          "var rows = csv.bind_all(schema_text, rows_csv, \"Choice\");"
          "var emitted = csv.emit(schema_text, side, \"Choice\");"
          "var rebound = csv.bind(schema_text, emitted, 0, \"Choice\");"
          "var all_json = \"[{\\\"side\\\":\\\"Buy\\\"},{\\\"perms\\\":\\\"Read|Execute\\\"},"
          "{\\\"success\\\":{\\\"code\\\":200}}]\";"
          "var all_choices = json.bind_all(schema_text, all_json, \"Choice\");"
          "var emitted_all = csv.emit(schema_text, all_choices, \"Choice\");"
          "var rebound_all = csv.bind_all(schema_text, emitted_all, \"Choice\");"
          "var side_ok = csv.validate(schema_text, side_csv, \"Choice\");"
          "var perms_ok = csv.validate(schema_text, perms_csv, \"Choice\");"
          "var success_ok = csv.validate(schema_text, success_csv, \"Choice\");"
          "var emitted_ok = csv.validate(schema_text, emitted, \"Choice\");"
          "var emitted_all_ok = csv.validate(schema_text, emitted_all, \"Choice\");"
          "var many_detail = csv.validate_ex(schema_text, many_csv, \"Choice\");"
          "var bad_detail = csv.validate_ex(schema_text, bad_csv, \"Choice\");"
          "var ok_total = side_ok + perms_ok + success_ok + emitted_ok + emitted_all_ok;"
          "var row_count = rows.length() + rebound_all.length();"
          "var total = side.side + perms.perms + success.success.code + "
          "rows[0].side + rows[1].side + rebound.side + rebound_all[0].side + "
          "rebound_all[1].perms + rebound_all[2].success.code;"
          "var bad_total = many_detail.ok + bad_detail.ok;"
          "var bad_message_len = many_detail.message.length() + bad_detail.message.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Top-level CSV union Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "ok_total"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "row_count"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_total"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 416.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should bind optional and default fields consistently for JSON and CSV") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var schema_text = \"message User { required uint32 id; "
          "optional uint32 age default 18; optional string role default \\\"user\\\"; "
          "optional string email; }\";"
          "var json_text = \"{\\\"id\\\":7}\";"
          "var csv_text = \"id\\n7\";"
          "var ju = json.bind(schema_text, json_text, \"User\");"
          "var cu = csv.bind(schema_text, csv_text, 0, \"User\");"
          "var json_ok = json.validate(schema_text, json_text, \"User\");"
          "var csv_ok = csv.validate(schema_text, csv_text, \"User\");"
          "var json_detail = json.validate_ex(schema_text, json_text, \"User\");"
          "var csv_detail = csv.validate_ex(schema_text, csv_text, \"User\");"
          "var bad_json = json.validate(schema_text, \"{}\", \"User\");"
          "var total = ju.id + ju.age + ju.role.length() + cu.id + cu.age + cu.role.length();"
          "var detail_ok = json_detail.ok + csv_detail.ok;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Schema optional/default bind Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "json_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "csv_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_json"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "detail_ok"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "total"), 58.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should expose schema parse diagnostics") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      const char *script =
          "var good = schema.parse_ex(\"message Trade { uint32 qty; }\");"
          "var types = schema.types(good.handle);"
          "var good_ok = good.ok;"
          "var good_handle_ok = good.handle >= 0;"
          "var type_count = types.length();"
          "var close_rc = schema.close(good.handle);"
          "var bad = schema.parse_ex(\"message Bad { uint32 ; }\");"
          "var bad_ok = bad.ok;"
          "var bad_handle = bad.handle;"
          "var bad_code = bad.code;"
          "var bad_message_len = bad.message.length();"
          "var last_error = schema.error();"
          "var diag_objects = is_object(good) + is_object(types[0]) + is_object(bad);"
          "var last_error_len = last_error.length();";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Schema diagnostics Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "good_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "good_handle_ok"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "type_count"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "close_rc"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_ok"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_handle"), -1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_code") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_message_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "diag_objects"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last_error_len") > 0.0 ? 1.0 : 0.0, 1.0, 0.001);

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
