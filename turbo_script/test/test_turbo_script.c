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

spec("turbo_script") {
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

  describe("Stock Research: TA-Lib Scripts") {
    it("SMA crossover signal generation") {
      // Scenario: A researcher loads daily closing prices and generates
      // buy/sell signals based on SMA(5) crossing above/below SMA(10).
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Simulated daily close prices (20 days of uptrend then pullback)
          "close = [100, 101, 102, 104, 106, "
          "108, 110, 112, 115, 118, "
          "          120, 119, 117, 115, 113, 112, 114, 116, 118, 120]; "

          // Compute fast and slow SMAs
          "sma_fast = ta.sma(close, 5); "
          "sma_slow = ta.sma(close, 10); "

          // Check the last bar: is fast SMA above slow SMA?
          // If yes → bullish signal (1), else bearish (0)
          "signal = if (sma_fast[19] > sma_slow[19]) { 1 } else { 0 }; "

          // Compute momentum via RSI
          "rsi = ta.rsi(close, 14); "
          "rsi_last = rsi[19]; "

          // Compute volatility
          "std = ta.stddev(close, 10, 1.0); "
          "vol_last = std[19];";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("SMA Crossover Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      // After pullback and recovery, fast SMA should be above slow SMA
      double signal = ts_get_num(ctx, "signal");
      printf("  Signal: %g (1=bullish, 0=bearish)\n", signal);

      double rsi_last = ts_get_num(ctx, "rsi_last");
      printf("  RSI(14): %.2f\n", rsi_last);
      check_float_gt(rsi_last, 0.0); // RSI should be valid

      double vol_last = ts_get_num(ctx, "vol_last");
      printf("  Volatility (StdDev): %.4f\n", vol_last);
      check_float_gt(vol_last, 0.0); // StdDev should be positive

      turbo_script_free(ctx);
    }

    it("RSI oversold screener") {
      // Scenario: A researcher screens for oversold stocks (RSI < 30)
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Stock A: strong downtrend (should be oversold)
          "stock_a = [50, 49, 47, 45, 43, 41, "
          "39, 37, 35, 33, "
          "            31, 30, 29, 28, 27]; "
          "rsi_a = ta.rsi(stock_a, 14);"
          "rsi_a_last = rsi_a[14]; "
          "oversold_a = if (rsi_a_last < 30) { 1 } else { 0 }; "

          // Stock B: strong uptrend (should be overbought)
          "stock_b = [50, 52, 54, 56, 58, 60, 62, 64, 66, 68, "
          "            70, 72, 74, 76, 78]; "
          "rsi_b = ta.rsi(stock_b, 14);"
          "rsi_b_last = rsi_b[14]; "
          "overbought_b = if (rsi_b_last > 70) { 1 } else { 0 };";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("RSI Screener Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double rsi_a = ts_get_num(ctx, "rsi_a_last");
      double rsi_b = ts_get_num(ctx, "rsi_b_last");
      printf("  Stock A RSI: %.2f (downtrend)\n", rsi_a);
      printf("  Stock B RSI: %.2f (uptrend)\n", rsi_b);

      // Downtrend stock should have lower RSI than uptrend stock
      check_float_lt(rsi_a, rsi_b);

      turbo_script_free(ctx);
    }

    it("Multi-indicator confluence scoring") {
      // Scenario: A researcher combines multiple indicators into a
      // composite score to rank stocks. Score range: 0 (bearish) to 100 (bullish).
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // 20 bars of price data with clear uptrend
          "close = [100, 102, 104, 103, 105, "
          "107, 109, 108, 110, 112, "
          "          114, 116, 115, 117, 119, 121, 123, 122, 124, 126]; "
          "high  = [102, 104, 106, 105, 107, 109, 111, 110, 112, 114, "
          "          116, 118, 117, 119, 121, 123, 125, 124, 126, 128]; "
          "low   = [ 99, 101, 103, 102, 104, 106, 108, 107, 109, 111, "
          "          113, 115, 114, 116, 118, 120, 122, 121, 123, 125]; "

          // --- Indicator 1: Trend (SMA crossover) ---
          "sma5  = ta.sma(close, 5); "
          "sma10 = ta.sma(close, 10); "
          "trend_score = if (sma5[19] > sma10[19]) { 25 } else { 0 }; "

          // --- Indicator 2: Momentum (RSI) ---
          "rsi = ta.rsi(close, 14); "
          "rsi_val = rsi[19]; "
          "mom_score = if (rsi_val > 50 and rsi_val < 70) { 25 } "
          "             else { if (rsi_val >= 70) { 10 } else { 0 } }; "

          // --- Indicator 3: Volatility (ATR normalized) ---
          "atr = ta.atr(high, low, close, 14); "
          "atr_pct = atr[19] / close[19] * 100; "
          "vol_score = if (atr_pct < 3) { 25 } else { 10 }; "

          // --- Indicator 4: Price position (above EMA) ---
          "ema20 = ta.ema(close, 20); "
          "pos_score = if (close[19] > ema20[19]) { 25 } else { 0 }; "

          // --- Composite Score ---
          "total_score = trend_score + mom_score + vol_score + pos_score;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Confluence Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double total = ts_get_num(ctx, "total_score");
      double trend = ts_get_num(ctx, "trend_score");
      double mom = ts_get_num(ctx, "mom_score");
      double vol = ts_get_num(ctx, "vol_score");
      double pos = ts_get_num(ctx, "pos_score");

      printf("  Confluence Score: %.0f / 100\n", total);
      printf("    Trend:    %.0f/25\n", trend);
      printf("    Momentum: %.0f/25\n", mom);
      printf("    Vol:      %.0f/25\n", vol);
      printf("    Position: %.0f/25\n", pos);

      // In a clear uptrend, the total score should be high
      check_float_gt(total, 50.0);

      turbo_script_free(ctx);
    }

    it("Options pricing: BSM call vs put parity") {
      // Scenario: A researcher verifies put-call parity:
      // C - P = S * e^(-qT) - K * e^(-rT)  (for q=0: C - P ≈ S - K*e^(-rT))
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // S=100, K=100, T=1yr, r=5%, sigma=20%
          "call = ta.bsm_call([100], [100], "
          "[1], [0.05], [0.2]); "
          "put  = ta.bsm_put([100], [100], [1], [0.05], [0.2]); "
          "call_price = call[0]; "
          "put_price  = put[0]; "

          // Put-call parity: C - P ≈ S - K*e^(-rT) = 100 - 100*e^(-0.05) ≈ 4.877
          "parity = call_price - put_price; "

          // Greeks
          "delta_c = bsm_delta_call([100], [100], [1], [0.05], [0.2]); "
          "delta_p = bsm_delta_put([100], [100], [1], [0.05], [0.2]); "
          "dc = delta_c[0]; "
          "dp = delta_p[0]; "

          // Delta parity: delta_call - delta_put ≈ e^(-qT) ≈ 1 (for q=0)
          "delta_parity = dc - dp;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Options Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double call_p = ts_get_num(ctx, "call_price");
      double put_p = ts_get_num(ctx, "put_price");
      double parity = ts_get_num(ctx, "parity");
      double dc = ts_get_num(ctx, "dc");
      double dp = ts_get_num(ctx, "dp");

      printf("  Call: $%.4f\n", call_p);
      printf("  Put:  $%.4f\n", put_p);
      printf("  C - P: $%.4f (expected ~4.877)\n", parity);
      printf("  Delta Call: %.4f\n", dc);
      printf("  Delta Put:  %.4f\n", dp);

      // Put-call parity check: C - P ≈ 4.877
      check_float_eq(parity, 4.877, 0.1);

      // Call should be more expensive than put for ATM with positive rate
      check_float_gt(call_p, put_p);

      // Delta parity: delta_c - delta_p ≈ 1
      double delta_parity = ts_get_num(ctx, "delta_parity");
      check_float_eq(delta_parity, 1.0, 0.05);

      turbo_script_free(ctx);
    }

    it("CSV data pipeline: split -> analyze -> score") {
      // Scenario: A researcher loads CSV price data from a string,
      // splits it into a vector, and runs TA analysis.
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Simulated CSV close prices (comma-separated)
          "csv_data = "
          "\"100,102,104,103,105,107,109,108,110,112\"; "

          // Parse CSV into a numeric vector
          "prices = split(csv_data, \",\"); "

          // Compute indicators on the parsed data
          "sma3 = ta.sma(prices, 3); "
          "ema3 = ta.ema(prices, 3); "

          // Last values
          "last_price = prices[9]; "
          "last_sma = sma3[9]; "
          "last_ema = ema3[9]; "

          // Simple trend check
          "trend = if (last_price > last_sma and last_price > last_ema) { 1 } else { 0 };";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV Pipeline Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double last_price = ts_get_num(ctx, "last_price");
      double last_sma = ts_get_num(ctx, "last_sma");
      double last_ema = ts_get_num(ctx, "last_ema");
      double trend = ts_get_num(ctx, "trend");

      printf("  Last Price: %.2f\n", last_price);
      printf("  SMA(3):    %.2f\n", last_sma);
      printf("  EMA(3):    %.2f\n", last_ema);
      printf("  Trend:     %s\n", trend > 0 ? "BULLISH" : "BEARISH");

      check_float_eq(last_price, 112.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Function Syntax (func)") {
    it("should allow defining and calling func in script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func double_val(x) { "
                           "  return x * 2; "
                           "}"
                           "res = double_val(21);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 42.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support recursion with func") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func factorial(n) { "
                           "  if (n <= 1) { return 1; } "
                           "  else { return n * factorial(n - 1); } "
                           "};"
                           "res = factorial(5);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 120.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Modules") {
    it("should support importing external scripts") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      // Write it using native file_write (or just setup beforehand? native is easier if we have
      // ctx) But we can use turbo_script_run to write it!
      turbo_script_run(ctx, "write_file(\"utils.tbs\", "
                            "\"var MODULE_VERSION = 2.0; func square(x) { return x * x; };\");");

      // Main script
      const char *script = "import(\"utils.tbs\"); "
                           "res = square(5); "
                           "ver = MODULE_VERSION;";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 25.0, 0.1);
      check_float_eq(ts_get_num(ctx, "ver"), 2.0, 0.1);

      // Clean up
      turbo_script_run(ctx, "file_remove(\"utils.tbs\");");
      turbo_script_free(ctx);
    }

    it("should resolve nested relative script imports from run_file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *util_src = "func helper(x) { return x + 1; };";
      const char *child_src = "import(\"./utils.tbs\");";
      const char *main_src = "import(\"sub/child.tbs\"); res = helper(7);";
      char root_dir[TURBO_FS_MAX_PATH];
      char sub_dir[TURBO_FS_MAX_PATH];
      char main_path[TURBO_FS_MAX_PATH];
      char child_path[TURBO_FS_MAX_PATH];
      char util_path[TURBO_FS_MAX_PATH];
      turbo_fs_buf_t buf;

      ts_test_make_name(root_dir, sizeof(root_dir), "_ts_import_rel", "");
      check_int_eq(turbo_fs_mkdir(root_dir, 0755), 0);
      check_int_eq(turbo_fs_path_join(sub_dir, sizeof(sub_dir), root_dir, "sub"), 0);
      check_int_eq(turbo_fs_mkdir(sub_dir, 0755), 0);
      check_int_eq(turbo_fs_path_join(main_path, sizeof(main_path), root_dir, "main.tbs"), 0);
      check_int_eq(turbo_fs_path_join(child_path, sizeof(child_path), sub_dir, "child.tbs"), 0);
      check_int_eq(turbo_fs_path_join(util_path, sizeof(util_path), sub_dir, "utils.tbs"), 0);

      buf = turbo_fs_buf_init((char *)util_src, strlen(util_src));
      check_int_eq(turbo_fs_write_file(util_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)child_src, strlen(child_src));
      check_int_eq(turbo_fs_write_file(child_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)main_src, strlen(main_src));
      check_int_eq(turbo_fs_write_file(main_path, &buf), 0);

      check_int_eq(turbo_script_run_file(ctx, main_path), 0);
      check_float_eq(ts_get_num(ctx, "res"), 8.0, 0.001);

      check_int_eq(turbo_fs_unlink(main_path), 0);
      check_int_eq(turbo_fs_unlink(child_path), 0);
      check_int_eq(turbo_fs_unlink(util_path), 0);
      check_int_eq(turbo_fs_rmdir(sub_dir), 0);
      check_int_eq(turbo_fs_rmdir(root_dir), 0);
      turbo_script_free(ctx);
    }

    it("should only treat .tbs files as script imports") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      turbo_script_run(ctx, "write_file(\"legacy.ts\", \"legacy_value = 7;\");");

      check_int_eq(turbo_script_run(ctx, "import(\"legacy.ts\");"), -1);
      check_float_eq(ts_get_num(ctx, "legacy_value"), 0.0, 0.001);

      turbo_script_run(ctx, "file_remove(\"legacy.ts\");");
      turbo_script_free(ctx);
    }

    it("should return explicit script exports as a module map") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *module_src = "load_count = load_count + 1; "
                               "func add1(x) { return x + 1; }; "
                               "export(\"add1\"); "
                               "export(\"answer\", 41); "
                               "export(\"load_count\");";
      char module_path[TURBO_FS_MAX_PATH];
      char script[512];
      turbo_fs_buf_t buf;

      ts_test_make_name(module_path, sizeof(module_path), "_ts_export_mod", ".tbs");
      buf = turbo_fs_buf_init((char *)module_src, strlen(module_src));
      check_int_eq(turbo_fs_write_file(module_path, &buf), 0);

      snprintf(script, sizeof(script),
               "var m1 = import(\"%s\"); "
               "var m2 = import(\"%s\"); "
               "var f = m1.add1; "
               "var res = f(4) + m2.answer; "
               "var loads = m2.load_count;",
               module_path, module_path);

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 46.0, 0.001);
      check_float_eq(ts_get_num(ctx, "loads"), 1.0, 0.001);

      check_int_eq(turbo_fs_unlink(module_path), 0);
      turbo_script_free(ctx);
    }

    it("should import a script module without leaking globals") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *module_src = "shared = 99; "
                               "secret = 5; "
                               "func add_secret(x) { return x + secret; }; "
                               "export(\"add_secret\"); "
                               "export(\"shared\");";
      char module_path[TURBO_FS_MAX_PATH];
      char script[640];
      turbo_fs_buf_t buf;

      ts_test_make_name(module_path, sizeof(module_path), "_ts_isolated_mod", ".tbs");
      buf = turbo_fs_buf_init((char *)module_src, strlen(module_src));
      check_int_eq(turbo_fs_write_file(module_path, &buf), 0);

      snprintf(script, sizeof(script),
               "shared = 7; "
               "var mod = import_module(\"%s\"); "
               "var f = mod.add_secret; "
               "var out = f(3); "
               "var exported_shared = mod.shared; "
               "var global_shared = shared; "
               "var leaked_secret = secret;",
               module_path);

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "out"), 8.0, 0.001);
      check_float_eq(ts_get_num(ctx, "exported_shared"), 99.0, 0.001);
      check_float_eq(ts_get_num(ctx, "global_shared"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "leaked_secret"), 0.0, 0.001);

      check_int_eq(turbo_fs_unlink(module_path), 0);
      turbo_script_free(ctx);
    }
  }

  describe("TA Indicators and JSON Vectors") {
    it("should compute SMA and RSI on mocked Polymarket data") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "ta"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      // Mocked Polymarket history JSON (array of objects with 'p' field)
      const char *mock_history = "["
                                 "{\"p\":0.51},{\"p\":0.52},{\"p\":0.53},{\"p\":0.54},{\"p\":0.55},"
                                 "{\"p\":0.56},{\"p\":0.57},{\"p\":0.58},{\"p\":0.59},{\"p\":0.60}"
                                 "]";
      ts_bind_str(ctx, "history_json", mock_history);

      const char *script = "prices = "
                           "json.to_vec(history_json, \"p\");"
                           "sma3 = ta.sma(prices, 3);"
                           "rsi5 = ta.rsi(prices, 5);"
                           "last_price = prices[9];"
                           "last_sma = sma3[9];"
                           "last_rsi = rsi5[9];"
                           "res = last_sma;";

      int run_res = turbo_script_run(ctx, script);
      if (run_res != 0) printf("TA Test Run Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(run_res, 0);

      double lp = ts_get_num(ctx, "last_price");
      printf("  Last Price: %g\n", lp);
      check_float_eq(lp, 0.60, 0.001);

      double ls = ts_get_num(ctx, "res");
      printf("  SMA(3): %g\n", ls);
      check_float_eq(ls, 0.59, 0.001);

      double rsi_val = ts_get_num(ctx, "last_rsi");
      printf("  RSI(5): %g\n", rsi_val);
      check_float_gt(rsi_val, 0.0);
      check_float_eq(rsi_val, 100.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Compile/Exec Separation") {
    it("should compile once and exec twice with different x") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, "y = x * 2;");
      check_not_null(compiled);

      ts_bind_num(ctx, "x", 5.0);
      turbo_script_exec(ctx, compiled);

      check_float_eq(ts_get_num(ctx, "y"), 10.0, 0.001);

      ts_bind_num(ctx, "x", 100.0);
      turbo_script_exec(ctx, compiled);
      check_float_eq(ts_get_num(ctx, "y"), 200.0, 0.001);

      turbo_script_compiled_free(compiled);
      turbo_script_free(ctx);
    }

    it("should return NULL on compile error") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, "x = 10 + * 5");
      check_null(compiled);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Data Channel") {
    it("should inject double[] from C and read back from script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      check_int_eq(ts_bind_vec(ctx, "v", data, 5), 0);

      check_int_eq(turbo_script_run(ctx, "s = "
                                         "vec.sum(v); a = vec.avg(v);"),
                   0);
      check_float_eq(ts_get_num(ctx, "s"), 15.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a"), 3.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should extract vector from script back to C") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run(ctx, "v = "
                                         "split(\"10,20,30\", \",\");"),
                   0);

      const double *out = NULL;
      size_t out_len = 0;
      check_int_eq(ts_get_vec(ctx, "v", &out, &out_len), 0);
      check_int_eq((int)out_len, 3);
      check_float_eq(out[0], 10.0, 0.001);
      check_float_eq(out[1], 20.0, 0.001);
      check_float_eq(out[2], 30.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should return -1 for not-found vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const double *out = NULL;
      size_t out_len = 0;
      check_int_eq(ts_get_vec(ctx, "nonexistent", &out, &out_len), -1);
      turbo_script_free(ctx);
    }
  }

  describe("String Variable Access") {
    it("should roundtrip set_var_str and get_var_str") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_str(ctx, "greeting", "hello world");
      const char *val = ts_get_str(ctx, "greeting");
      check_not_null(val);
      check_int_eq(strcmp(val, "hello world"), 0);
      turbo_script_free(ctx);
    }

    it("should return NULL for wrong type") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_num(ctx, "num", 42.0);
      const char *val = ts_get_str(ctx, "num");
      check_null(val);
      turbo_script_free(ctx);
    }
  }

  describe("User Function Registration") {
    it("should bind C function and call from script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      ts_bind_func(ctx, "triple", test_triple_fn, NULL);
      check_int_eq(turbo_script_run(ctx, "r = triple(7);"), 0);
      check_float_eq(ts_get_num(ctx, "r"), 21.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Error Propagation") {
    it("should abort on wrong arg type") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      int res = turbo_script_run(ctx, "r = vec.avg(42);");
      check_int_eq(res, -1);
      turbo_script_free(ctx);
    }

    it("should not abort on runtime failure like missing file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      int res = turbo_script_run(ctx, "data = "
                                      "read_file(\"nonexistent_file_xyz.txt\");");
      check_int_eq(res, 0);
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: Compound Assignment") {
    it("should support += -= *= /=") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = 10; "
                           "x += 5; " // 15
                           "x -= 3; " // 12
                           "x *= 2; " // 24
                           "x /= 4;"; // 6
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "x"), 6.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: For Loop") {
    it("should support for(init; cond; post) { body }") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; "
                           "for (i = 0; i < 10; i += 1) { "
                           "  sum += i; "
                           "}";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "sum"), 45.0, 0.001); // 0+1+...+9
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: Vector Slicing") {
    it("should support arr[start..end] slicing") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [10, 20, 30, 40, 50]; "
                           "s = v[1..4]; "
                           "l = vec.len(s); "
                           "total = vec.sum(s);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "l"), 3.0, 0.001);      // elements at index 1,2,3
      check_float_eq(ts_get_num(ctx, "total"), 90.0, 0.001); // 20+30+40
      turbo_script_free(ctx);
    }
  }

  describe("Containers") {
    it("should use broad truthiness for containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "m0 = map{}; "
                           "m1 = map{a: 1}; "
                           "l0 = list(); "
                           "l1 = list(42); "
                           "miss1 = m1.nope; "
                           "miss2 = m1[\"nope\"]; "
                           "tm0 = if (m0) { 1 } else { 0 }; "
                           "tm1 = if (m1) { 1 } else { 0 }; "
                           "tl0 = if (l0) { 1 } else { 0 }; "
                           "tl1 = if (l1) { 1 } else { 0 };";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Container contract Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "miss1"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "miss2"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tm0"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tm1"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tl0"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tl1"), 1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Quant: Risk Metrics") {
    it("should compute VaR, CVaR, Kelly criterion") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script =
          " returns = [-0.02, 0.01, -0.03, "
          "0.02, 0.01, -0.01, 0.03, -0.02, 0.01, -0.04, "
          "            0.02, 0.01, -0.01, 0.03, -0.02, 0.01, -0.03, 0.02, 0.01, -0.01]; "
          "vh = strategy.var_hist(returns, 0.95); "
          "vp = strategy.var_param(returns, 0.95); "
          "cv = strategy.cvar(returns, 0.95); "
          "k = strategy.kelly(0.6, 0.02, 0.015);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Risk Metrics Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double vh = ts_get_num(ctx, "vh");
      double vp = ts_get_num(ctx, "vp");
      double cv = ts_get_num(ctx, "cv");
      double k = ts_get_num(ctx, "k");
      printf("  VaR(hist): %.4f\n", vh);
      printf("  VaR(param): %.4f\n", vp);
      printf("  CVaR: %.4f\n", cv);
      printf("  Kelly: %.4f\n", k);

      // VaR should be positive (loss magnitude)
      check_float_gt(vh, 0.0);
      // Kelly should be positive for profitable strategy
      check_float_gt(k, 0.0);

      turbo_script_free(ctx);
    }

    it("should compute drawdown stats") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script = "equity = [100, "
                           "105, 103, 108, 106, 110, 107, 112, 115, 113]; "
                           "dd = strategy.drawdown(equity); "
                           "dd_stats = strategy.drawdown_stats(equity);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Drawdown Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      // dd should be a vector of same length
      const double *dd_data = NULL;
      size_t dd_len = 0;
      check_int_eq(ts_get_vec(ctx, "dd", &dd_data, &dd_len), 0);
      check_int_eq((int)dd_len, 10);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Signal Detection") {
    it("should detect crossover and crossunder") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");
      const char *script = " fast = [1, 3, 5, 4, 2, 4, 6]; "
                           "slow = [2, 2, 4, 5, 3, 3, 5]; "
                           "co = ta.crossover(fast, slow); "
                           "cu = ta.crossunder(fast, slow); "
                           "co_len = vec.len(co); "
                           "cu_len = vec.len(cu);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Signal Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "co_len"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cu_len"), 7.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Candlestick Patterns") {
    it("should detect doji and hammer patterns") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");
      const char *script =
          // Doji: open ≈ close, long shadows
          "o = [100, 100, 100, 100, 100]; "
          "h = [105, 105, 105, 105, 105]; "
          "l = [ 95,  95,  95,  95,  95]; "
          "c = [100.1, 100.2, 99.9, 100, 100.1]; "
          "doji = ta.candle_doji(o, h, l, c, 0.1); "
          "hammer = ta.candle_hammer(o, h, l, c); "
          "doji_len = vec.len(doji); "
          "hammer_len = vec.len(hammer);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Candle Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "doji_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "hammer_len"), 5.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Portfolio Optimization") {
    it("should compute minimum variance weights") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script =
          // 2x2 covariance matrix (2 assets): [var1, cov12, cov21, var2]
          "cov = [0.04, 0.01, 0.01, 0.09]; "
          "w = strategy.pf_min_variance(cov); "
          "w_len = vec.len(w);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Portfolio Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      const double *w_data = NULL;
      size_t w_len = 0;
      check_int_eq(ts_get_vec(ctx, "w", &w_data, &w_len), 0);
      check_int_eq((int)w_len, 2);

      // Weights should sum to ~1
      double w_sum = w_data[0] + w_data[1];
      printf("  Weights: [%.4f, %.4f] sum=%.4f\n", w_data[0], w_data[1], w_sum);
      check_float_eq(w_sum, 1.0, 0.05);

      turbo_script_free(ctx);
    }
    it("should compute minimum variance weights for three assets") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script =
          "cov = [0.04, 0.01, 0.00, "
          "       0.01, 0.09, 0.02, "
          "       0.00, 0.02, 0.16]; "
          "w = strategy.pf_min_variance(cov);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Portfolio 3 Asset Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      const double *w_data = NULL;
      size_t w_len = 0;
      check_int_eq(ts_get_vec(ctx, "w", &w_data, &w_len), 0);
      check_int_eq((int)w_len, 3);
      check_float_eq(w_data[0] + w_data[1] + w_data[2], 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should not expose finance risk helpers through ta aliases") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "ta"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "fin"), 0);

      const char *script = "returns = [-0.02, 0.01, -0.03]; "
                           "bad = try { ta.var_hist(returns, 0.95); 0 } catch (e) { 1 };";
      int res = turbo_script_run(ctx, script);
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "bad"), 1.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Advanced Statistics") {
    it("should compute median, percentile, skewness, kurtosis") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [1, 2, 3, 4, "
                           "5, 6, 7, 8, 9, 10]; "
                           "med = median(v); "
                           "p75 = percentile(v, 75); "
                           "sk = skewness(v); "
                           "ku = kurtosis(v); "
                           "gm = geometric_mean(v); "
                           "hm = harmonic_mean(v);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Stats Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double med = ts_get_num(ctx, "med");
      double p75 = ts_get_num(ctx, "p75");
      double sk = ts_get_num(ctx, "sk");
      double gm = ts_get_num(ctx, "gm");
      double hm = ts_get_num(ctx, "hm");
      printf("  Median: %.2f\n", med);
      printf("  P75: %.2f\n", p75);
      printf("  Skewness: %.4f\n", sk);
      printf("  Geometric Mean: %.4f\n", gm);
      printf("  Harmonic Mean: %.4f\n", hm);

      check_float_eq(med, 5.5, 0.1);
      check_float_gt(p75, med);
      // Uniform distribution: skewness ≈ 0
      check_float_eq(sk, 0.0, 0.5);
      // GM < AM < nothing, HM < GM
      check_float_lt(hm, gm);

      turbo_script_free(ctx);
    }

    it("should compute cumsum, cumprod, rank, zscore") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [3, 1, 4, 1, 5]; "
                           "cs = vec.cumsum(v); "
                           "rk = rank(v); "
                           "zs = zscore(v); "
                           "cs_last = cs[4]; "
                           "cs_len = vec.len(cs);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Stats2 Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "cs_last"), 14.0, 0.001); // 3+1+4+1+5
      check_float_eq(ts_get_num(ctx, "cs_len"), 5.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Time Series") {
    it("should compute diff, autocorrelation, hurst exponent") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ts");
      turbo_script_load_plugin(ctx, "vec");
      const char *script = "v = [100, 102, "
                           "101, 105, 103, 107, 106, 110, 108, 112, "
                           "     111, 115, 113, 117, 116, 120, 118, 122, 121, 125]; "
                           "d = ts.diff(v, 1); "
                           "d_len = vec.len(d); "
                           "ac = ts.autocorr(v, 5); "
                           "ac_len = vec.len(ac); "
                           "h = ts.hurst(v);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("TimeSeries Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double d_len = ts_get_num(ctx, "d_len");
      double ac_len = ts_get_num(ctx, "ac_len");
      printf("  Diff len: %.0f\n", d_len);
      printf("  Autocorr len: %.0f\n", ac_len);
      check_float_gt(d_len, 0.0);
      check_float_gt(ac_len, 0.0);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Calculus") {
    it("should integrate and differentiate script functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          // Define f(x) = x^2, integrate from 0 to 3 → should be 9
          "func f(x) { return x * x; }; "
          "area = integrate(\"f\", 0, 3, 1000); "
          // derivative of x^2 at x=2 → should be 4
          "slope = derivative(\"f\", 2);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Calculus Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double area = ts_get_num(ctx, "area");
      double slope = ts_get_num(ctx, "slope");
      printf("  Integral of x^2 from 0..3: %.4f (expected 9)\n", area);
      printf("  Derivative of x^2 at x=2: %.4f (expected 4)\n", slope);

      check_float_eq(area, 9.0, 0.01);
      check_float_eq(slope, 4.0, 0.01);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: Math Builtins") {
    it("should support trig, log, exp, rounding") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "s = sin(0); "
                           "c = cos(0); "
                           "sq = sqrt(144); "
                           "lg = log(e); "
                           "ex = exp(1); "
                           "cl = ceil(2.3); "
                           "fl = floor(2.7); "
                           "rn = round(2.5); "
                           "ab = abs(-42); "
                           "fib = fibonacci(10); "
                           "g = gcd(12, 8);";
      check_int_eq(turbo_script_run(ctx, script), 0);

      check_float_eq(ts_get_num(ctx, "s"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "sq"), 12.0, 0.001);
      check_float_eq(ts_get_num(ctx, "lg"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cl"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "fl"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rn"), 3.0, 1.0); // platform-dependent rounding
      check_float_eq(ts_get_num(ctx, "ab"), 42.0, 0.001);
      check_float_eq(ts_get_num(ctx, "fib"), 55.0, 0.001);
      check_float_eq(ts_get_num(ctx, "g"), 4.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Scientific: String Builtins") {
    it("should support lower, upper, trim, contains, substr, replace") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "lo = lower(\"HELLO\"); "
                           "up = upper(\"hello\"); "
                           "tr = trim(\"  hi  \"); "
                           "ct = contains(\"foobar\", \"bar\"); "
                           "sw = starts_with(\"foobar\", \"foo\"); "
                           "ew = ends_with(\"foobar\", \"bar\"); "
                           "ix = index_of(\"foobar\", \"bar\"); "
                           "ss = substr(\"hello world\", 6, 5); "
                           "rp = replace(\"aabbcc\", \"bb\", \"XX\");";
      check_int_eq(turbo_script_run(ctx, script), 0);

      check_float_eq(ts_get_num(ctx, "ct"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "sw"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ew"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ix"), 3.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should tokenize and convert strings through string views") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "data = \"10,,20;30\"; "
                           "count = token_count(data, \",;\"); "
                           "tok = str_token(data, \",;\", 1); "
                           "vals = split(data, \",;\"); "
                           "total = vals[0] + vals[1] + vals[2]; "
                           "num = to_num(\"42.5\"); "
                           "ival = to_int(\"0x10\"); "
                           "truth = to_bool(\"YES\");";
      check_int_eq(turbo_script_run(ctx, script), 0);

      check_float_eq(ts_get_num(ctx, "count"), 3.0, 0.001);
      check_str_eq(ts_get_str(ctx, "tok"), "20");
      check_float_eq(ts_get_num(ctx, "total"), 60.0, 0.001);
      check_float_eq(ts_get_num(ctx, "num"), 42.5, 0.001);
      check_float_eq(ts_get_num(ctx, "ival"), 16.0, 0.001);
      check_float_eq(ts_get_num(ctx, "truth"), 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should support utf8 codepoint string operations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "b\"; "
                           "var valid = utf8_valid(text); "
                           "var len = utf8_len(text); "
                           "var idx = utf8_index_of(text, \"" "\xF0\x9F\x99\x82" "\"); "
                           "var slice = utf8_substr(text, 1, 2); "
                           "var upper = utf8_upper(\"" "\xC3\xA9" "\"); "
                           "var lower = utf8_lower(\"" "\xC3\x89" "\"); "
                           "var total = valid + len + idx;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("UTF8 string Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "total"), 7.0, 0.001);
      check_str_eq(ts_get_str(ctx, "slice"), "\xE4\xBD\xA0" "\xF0\x9F\x99\x82");
      check_str_eq(ts_get_str(ctx, "upper"), "\xC3\x89");
      check_str_eq(ts_get_str(ctx, "lower"), "\xC3\xA9");

      turbo_script_free(ctx);
    }

    it("should split strings by substring and join string lists") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var parts = str_split(\"alpha--beta--\", \"--\"); "
                           "var joined = str_join(parts, \"|\"); "
                           "var chars = str_split(\"a" "\xE4\xBD\xA0" "b\", \"\"); "
                           "var score = parts.length() + parts[0].length() + "
                           "parts[1].length() + parts[2].length() + chars.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String split/join Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 15.0, 0.001);
      check_str_eq(ts_get_str(ctx, "joined"), "alpha|beta|");

      turbo_script_free(ctx);
    }

    it("should expose utf8 character helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "\xE4\xBD\xA0" "\"; "
                           "var ch = utf8_char_at(text, 1); "
                           "var cp = utf8_codepoint_at(text, 1); "
                           "var last = utf8_rindex_of(text, \"" "\xE4\xBD\xA0" "\"); "
                           "var rev = utf8_reverse(text); "
                           "var score = cp + last + utf8_len(rev);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("UTF8 helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 20327.0, 0.001);
      check_str_eq(ts_get_str(ctx, "ch"), "\xE4\xBD\xA0");
      check_str_eq(ts_get_str(ctx, "rev"), "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "\xE4\xBD\xA0" "a");

      turbo_script_free(ctx);
    }

    it("should expose codepoint and byte helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "b\"; "
                           "var cp0 = ord(\"" "\xE4\xBD\xA0" "\"); "
                           "var cp1 = codepoint_at(text, 2); "
                           "var cp2 = text.codePointAt(1); "
                           "var built = from_codepoint(20320, 128578); "
                           "var single = chr(20320); "
                           "var sliced = utf8_slice(text, -3, -1); "
                           "var byte = byte_at(\"Az\", 1); "
                           "var bytes = byte_length(\"" "\xE4\xBD\xA0" "\"); "
                           "var score = cp0 + cp1 + cp2 + byte + bytes;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Codepoint helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 169343.0, 0.001);
      check_str_eq(ts_get_str(ctx, "built"), "\xE4\xBD\xA0" "\xF0\x9F\x99\x82");
      check_str_eq(ts_get_str(ctx, "single"), "\xE4\xBD\xA0");
      check_str_eq(ts_get_str(ctx, "sliced"), "\xE4\xBD\xA0" "\xF0\x9F\x99\x82");

      turbo_script_free(ctx);
    }

    it("should support data-oriented string helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var escaped = csv_escape(\"a,\\\"b\\\"\\n\"); "
                           "var raw = csv_unescape(escaped); "
                           "var lines = str_lines(\"a\\r\\nb\\nc\\r\"); "
                           "var repeated = str_repeat(\"ab\", 3); "
                           "var count = str_count_substr(\"aaaa\", \"aa\"); "
                           "var score = lines.length() + count;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Data string helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 5.0, 0.001);
      check_str_eq(ts_get_str(ctx, "escaped"), "\"a,\"\"b\"\"\n\"");
      check_str_eq(ts_get_str(ctx, "raw"), "a,\"b\"\n");
      check_str_eq(ts_get_str(ctx, "repeated"), "ababab");

      turbo_script_free(ctx);
    }

    it("should support trimming padding and case-insensitive helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var trimmed = trim_chars(\"--value--\", \"-\"); "
                           "var left = ltrim_chars(\"..value..\", \".\"); "
                           "var right = rtrim_chars(\"..value..\", \".\"); "
                           "var ci_eq = str_eq_ci(\"" "\xC3\x89" "\", \"" "\xC3\xA9" "\"); "
                           "var ci_contains = str_contains_ci(\"a" "\xC3\x89" "b\", \"" "\xC3\xA9" "\"); "
                           "var once = replace_once(\"aaaa\", \"aa\", \"b\"); "
                           "var no_prefix = remove_prefix(\"pre-name\", \"pre-\"); "
                           "var no_suffix = remove_suffix(\"name.txt\", \".txt\"); "
                           "var padded_left = lpad(\"" "\xE4\xBD\xA0" "\", 3, \"" "\xF0\x9F\x99\x82" "\"); "
                           "var padded_right = rpad(\"x\", 3, \".\"); "
                           "var bad_prefix = starts_with_ci(\"" "\xE4\xBD\xA0" "a\", \"n\"); "
                           "var bad_suffix = ends_with_ci(\"a" "\xE4\xBD\xA0" "\", \"x\"); "
                           "var score = ci_eq + ci_contains;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String cleanup helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 2.0, 0.001);
      check_str_eq(ts_get_str(ctx, "trimmed"), "value");
      check_str_eq(ts_get_str(ctx, "left"), "value..");
      check_str_eq(ts_get_str(ctx, "right"), "..value");
      check_str_eq(ts_get_str(ctx, "once"), "baa");
      check_str_eq(ts_get_str(ctx, "no_prefix"), "name");
      check_str_eq(ts_get_str(ctx, "no_suffix"), "name");
      check_str_eq(ts_get_str(ctx, "padded_left"), "\xF0\x9F\x99\x82" "\xF0\x9F\x99\x82" "\xE4\xBD\xA0");
      check_str_eq(ts_get_str(ctx, "padded_right"), "x..");
      check_float_eq(ts_get_num(ctx, "bad_prefix"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "bad_suffix"), 0.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should support partition word and csv line helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var sw = starts_with_ci(\"Hello\", \"he\"); "
                           "var ew = ends_with_ci(\"Hello\", \"LO\"); "
                           "var p = str_partition(\"a=b=c\", \"=\"); "
                           "var rp = str_rpartition(\"a=b=c\", \"=\"); "
                           "var words = str_words(\"  alpha\\t beta\\n gamma  \"); "
                           "var cells = csv_split_line(\"a,\\\"b,c\\\",\\\"d\\\"\\\"e\\\"\"); "
                           "var line = csv_join_line(list(\"a\", \"b,c\", \"d\\\"e\")); "
                           "var p0 = p[0]; "
                           "var p1 = p[1]; "
                           "var p2 = p[2]; "
                           "var rp0 = rp[0]; "
                           "var rp1 = rp[1]; "
                           "var rp2 = rp[2]; "
                           "var word1 = words[1]; "
                           "var cell1 = cells[1]; "
                           "var cell2 = cells[2]; "
                           "var score = sw + ew + p.length() + rp.length() + words.length() + cells.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String partition/csv helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 14.0, 0.001);
      check_str_eq(ts_get_str(ctx, "p0"), "a");
      check_str_eq(ts_get_str(ctx, "p1"), "=");
      check_str_eq(ts_get_str(ctx, "p2"), "b=c");
      check_str_eq(ts_get_str(ctx, "rp0"), "a=b");
      check_str_eq(ts_get_str(ctx, "rp1"), "=");
      check_str_eq(ts_get_str(ctx, "rp2"), "c");
      check_str_eq(ts_get_str(ctx, "word1"), "beta");
      check_str_eq(ts_get_str(ctx, "cell1"), "b,c");
      check_str_eq(ts_get_str(ctx, "cell2"), "d\"e");
      check_str_eq(ts_get_str(ctx, "line"), "a,\"b,c\",\"d\"\"e\"");

      turbo_script_free(ctx);
    }

    it("should support Python and JS style string helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var fmt = str_format(\"{}:{1}:{{}}\", \"x\", 42); "
                           "var norm = unicode_normalize(\"" "\xC3\xA9" "\", \"NFC\"); "
                           "var gl = grapheme_len(\"a" "\xE4\xBD\xA0" "b\"); "
                           "var gs = grapheme_substr(\"a" "\xE4\xBD\xA0" "b\", 1, 1); "
                           "var gr = grapheme_reverse(\"a" "\xE4\xBD\xA0" "b\"); "
                           "var split = grapheme_split(\"a" "\xE4\xBD\xA0" "b\"); "
                           "var titled = title(\"hello turbo script\"); "
                           "var cap = capitalize(\"hELLO\"); "
                           "var swapped = swapcase(\"AbC1\"); "
                           "var folded = casefold(\"" "\xC3\x89" "\"); "
                           "var removed = remove_chars(\"a" "\xE4\xBD\xA0" "b" "\xE4\xBD\xA0" "\", \"" "\xE4\xBD\xA0" "\"); "
                           "var kept = keep_chars(\"a1b2c3\", \"123\"); "
                           "var trans = translate(\"abcxyz\", \"abc\", \"123\"); "
                           "var score = gl + split.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Python/JS string helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 6.0, 0.001);
      check_str_eq(ts_get_str(ctx, "fmt"), "x:42:{}");
      check_str_eq(ts_get_str(ctx, "norm"), "\xC3\xA9");
      check_str_eq(ts_get_str(ctx, "gs"), "\xE4\xBD\xA0");
      check_str_eq(ts_get_str(ctx, "gr"), "b" "\xE4\xBD\xA0" "a");
      check_str_eq(ts_get_str(ctx, "titled"), "Hello Turbo Script");
      check_str_eq(ts_get_str(ctx, "cap"), "Hello");
      check_str_eq(ts_get_str(ctx, "swapped"), "aBc1");
      check_str_eq(ts_get_str(ctx, "folded"), "\xC3\xA9");
      check_str_eq(ts_get_str(ctx, "removed"), "ab");
      check_str_eq(ts_get_str(ctx, "kept"), "123");
      check_str_eq(ts_get_str(ctx, "trans"), "123xyz");

      turbo_script_free(ctx);
    }

    it("should support string predicates and Python JS search helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var score = is_empty(\"\") + is_blank(\" \\t\\n\") + "
                           "is_ascii(\"abc\") + is_digit(\"123\") + is_alpha(\"abc\") + "
                           "is_alnum(\"abc123\") + is_space(\" \\n\") + "
                           "is_lower(\"abc1\") + is_upper(\"ABC1\"); "
                           "var last = last_index_of(\"ababa\", \"ba\"); "
                           "var f = find(\"ababa\", \"ba\", 2); "
                           "var rf = rfind(\"ababa\", \"ba\"); "
                           "var sliced = slice(\"abcdef\", -4, -1); "
                           "var ch = char_at(\"abc\", 1); "
                           "var neg = is_digit(\"12a\") + is_ascii(\"" "\xE4\xBD\xA0" "\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String predicate/search Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 9.0, 0.001);
      check_float_eq(ts_get_num(ctx, "last"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "f"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rf"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "neg"), 0.0, 0.001);
      check_str_eq(ts_get_str(ctx, "sliced"), "cde");
      check_str_eq(ts_get_str(ctx, "ch"), "b");

      turbo_script_free(ctx);
    }

    it("should support html url base64 and hex string codecs") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var html = html_escape(\"<a&\\\"'>\"); "
                           "var html_raw = html_unescape(html); "
                           "var url = url_encode(\"a b+c/" "\xE4\xBD\xA0" "\"); "
                           "var url_raw = url_decode(url); "
                           "var b64 = base64_encode(\"hello\"); "
                           "var b64_raw = base64_decode(b64); "
                           "var hx = hex_encode(\"Az\"); "
                           "var hx_raw = hex_decode(hx);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String codec Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "html"), "&lt;a&amp;&quot;&#39;&gt;");
      check_str_eq(ts_get_str(ctx, "html_raw"), "<a&\"'>");
      check_str_eq(ts_get_str(ctx, "url"), "a%20b%2Bc%2F%E4%BD%A0");
      check_str_eq(ts_get_str(ctx, "url_raw"), "a b+c/" "\xE4\xBD\xA0");
      check_str_eq(ts_get_str(ctx, "b64"), "aGVsbG8=");
      check_str_eq(ts_get_str(ctx, "b64_raw"), "hello");
      check_str_eq(ts_get_str(ctx, "hx"), "417a");
      check_str_eq(ts_get_str(ctx, "hx_raw"), "Az");

      turbo_script_free(ctx);
    }

    it("should support advanced string search split predicates and codecs") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var hits = find_all(\"a--b--c--\", \"--\"); "
                           "var limited = split_limit(\"a,b,c,d\", \",\", 2); "
                           "var b64u = base64url_encode(\"hello?\"); "
                           "var b64u_raw = base64url_decode(b64u); "
                           "var hex_ok = is_hex(\"0aF9\"); "
                           "var hex_bad = is_hex(\"0x10\"); "
                           "var printable_ok = is_printable(\"line\\n\"); "
                           "var printable_bad = is_printable(chr(1)); "
                           "var eq_ok = constant_time_eq(\"secret\", \"secret\"); "
                           "var eq_bad = constant_time_eq(\"secret\", \"secreT\"); "
                           "var score = hits.length() + hits[0] + hits[1] + hits[2] + "
                           "limited.length() + hex_ok + printable_ok + eq_ok + "
                           "hex_bad + printable_bad + eq_bad; "
                           "var part0 = limited[0]; "
                           "var part1 = limited[1]; "
                           "var part2 = limited[2];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Advanced string helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 21.0, 0.001);
      check_str_eq(ts_get_str(ctx, "part0"), "a");
      check_str_eq(ts_get_str(ctx, "part1"), "b");
      check_str_eq(ts_get_str(ctx, "part2"), "c,d");
      check_str_eq(ts_get_str(ctx, "b64u"), "aGVsbG8_");
      check_str_eq(ts_get_str(ctx, "b64u_raw"), "hello?");

      turbo_script_free(ctx);
    }

    it("should support overlapping search wrap case conversion and bytes") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var hits = find_all_overlapping(\"aaaa\", \"aa\"); "
                           "var overlap_count = str_count_overlapping(\"aaaa\", \"aa\"); "
                           "var wrapped = word_wrap(\"alpha beta gamma\", 10); "
                           "var hard_wrap = word_wrap(\"abcdefgh\", 3); "
                           "var unbroken = word_wrap(\"abcdefgh\", 3, 0); "
                           "var short = shorten(\"alpha beta gamma\", 14); "
                           "var tiny = shorten(\"abcdef\", 2); "
                           "var snake = snake_case(\"Hello HTTP response_code42\"); "
                           "var kebab = kebab_case(\"Hello HTTP response_code42\"); "
                           "var camel = camel_case(\"Hello HTTP response_code42\"); "
                           "var pascal = pascal_case(\"Hello HTTP response_code42\"); "
                           "var slug = slugify(\"Hello, Turbo Script!\"); "
                           "var bs = bytes(\"Az\"); "
                           "var raw = from_bytes(bs); "
                           "var raw_vec = from_bytes([65,66]); "
                           "var type_score = (typeof(bs) == \"bytes\") + is_bytes(bs); "
                           "var score = hits.length() + hits[0] + hits[1] + hits[2] + "
                           "overlap_count + bs.length() + bs[0] + bs[1] + type_score;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String extended helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 200.0, 0.001);
      check_str_eq(ts_get_str(ctx, "wrapped"), "alpha beta\ngamma");
      check_str_eq(ts_get_str(ctx, "hard_wrap"), "abc\ndef\ngh");
      check_str_eq(ts_get_str(ctx, "unbroken"), "abcdefgh");
      check_str_eq(ts_get_str(ctx, "short"), "alpha beta...");
      check_str_eq(ts_get_str(ctx, "tiny"), "..");
      check_str_eq(ts_get_str(ctx, "snake"), "hello_http_response_code42");
      check_str_eq(ts_get_str(ctx, "kebab"), "hello-http-response-code42");
      check_str_eq(ts_get_str(ctx, "camel"), "helloHttpResponseCode42");
      check_str_eq(ts_get_str(ctx, "pascal"), "HelloHttpResponseCode42");
      check_str_eq(ts_get_str(ctx, "slug"), "hello-turbo-script");
      check_str_eq(ts_get_str(ctx, "raw"), "Az");
      check_str_eq(ts_get_str(ctx, "raw_vec"), "AB");

      turbo_script_free(ctx);
    }

    it("should support whitespace normalization and split variants") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var normalized = normalize_space(\"  alpha\\t beta\\n  gamma  \"); "
                           "var centered = center(\"x\", 5, \".\"); "
                           "var truncated = truncate(\"abcdef\", 5); "
                           "var short_suffix = truncate(\"abcdef\", 2); "
                           "var stripped = strip_suffix(strip_prefix(\"pre-name.txt\", \"pre-\"), \".txt\"); "
                           "var words = split_whitespace(\"  a\\t b\\n c  \"); "
                           "var once = split_once(\"a=b=c\", \"=\"); "
                           "var ronce = rsplit_once(\"a=b=c\", \"=\"); "
                           "var rparts = rsplit(\"a,b,c\", \",\", 1); "
                           "var score = words.length() + once.length() + ronce.length() + rparts.length(); "
                           "var word1 = words[1]; "
                           "var once0 = once[0]; "
                           "var once1 = once[1]; "
                           "var ronce0 = ronce[0]; "
                           "var ronce1 = ronce[1]; "
                           "var rpart0 = rparts[0]; "
                           "var rpart1 = rparts[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String split variant Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 9.0, 0.001);
      check_str_eq(ts_get_str(ctx, "normalized"), "alpha beta gamma");
      check_str_eq(ts_get_str(ctx, "centered"), "..x..");
      check_str_eq(ts_get_str(ctx, "truncated"), "ab...");
      check_str_eq(ts_get_str(ctx, "short_suffix"), "..");
      check_str_eq(ts_get_str(ctx, "stripped"), "name");
      check_str_eq(ts_get_str(ctx, "word1"), "b");
      check_str_eq(ts_get_str(ctx, "once0"), "a");
      check_str_eq(ts_get_str(ctx, "once1"), "b=c");
      check_str_eq(ts_get_str(ctx, "ronce0"), "a=b");
      check_str_eq(ts_get_str(ctx, "ronce1"), "c");
      check_str_eq(ts_get_str(ctx, "rpart0"), "a,b");
      check_str_eq(ts_get_str(ctx, "rpart1"), "c");

      turbo_script_free(ctx);
    }

    it("should support line and fixed width string helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var text = \"a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82" "b\"; "
                           "var left_part = left(text, 3); "
                           "var right_part = right(text, 2); "
                           "var taken = str_take(\"abcdef\", 2); "
                           "var dropped = str_drop(\"abcdef\", 2); "
                           "var filled = zfill(\"-42\", 5); "
                           "var expanded = expand_tabs(\"a\\tb\\n\\tc\", 4); "
                           "var chomped = chomp(\"line\\r\\n\"); "
                           "var lines = split_lines(\"a\\r\\nb\\nc\\r\"); "
                           "var line1 = lines[1]; "
                           "var score = lines.length() + line_count(\"a\\r\\nb\\nc\\r\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String line/fixed helper Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 6.0, 0.001);
      check_str_eq(ts_get_str(ctx, "left_part"), "a" "\xE4\xBD\xA0" "\xF0\x9F\x99\x82");
      check_str_eq(ts_get_str(ctx, "right_part"), "\xF0\x9F\x99\x82" "b");
      check_str_eq(ts_get_str(ctx, "taken"), "ab");
      check_str_eq(ts_get_str(ctx, "dropped"), "cdef");
      check_str_eq(ts_get_str(ctx, "filled"), "-0042");
      check_str_eq(ts_get_str(ctx, "expanded"), "a   b\n    c");
      check_str_eq(ts_get_str(ctx, "chomped"), "line");
      check_str_eq(ts_get_str(ctx, "line1"), "b");

      turbo_script_free(ctx);
    }

    it("should support block indentation and wrapping helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var indented = indent(\"a\\nb\", \"> \"); "
                           "var skipped = indent(\"a\\nb\", \"--\", 0); "
                           "var dedented = dedent(\"  a\\n    b\\n\"); "
                           "var surrounded = surround(\"value\", \"[\", \"]\"); "
                           "var unwrapped = unwrap(surrounded, \"[\", \"]\"); "
                           "var unchanged = unwrap(\"value\", \"[\", \"]\"); "
                           "var score = line_count(indented) + line_count(dedented);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String indent/wrap Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 4.0, 0.001);
      check_str_eq(ts_get_str(ctx, "indented"), "> a\n> b");
      check_str_eq(ts_get_str(ctx, "skipped"), "a\n--b");
      check_str_eq(ts_get_str(ctx, "dedented"), "a\n  b\n");
      check_str_eq(ts_get_str(ctx, "surrounded"), "[value]");
      check_str_eq(ts_get_str(ctx, "unwrapped"), "value");
      check_str_eq(ts_get_str(ctx, "unchanged"), "value");

      turbo_script_free(ctx);
    }

    it("should support string range editing helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var inserted = insert(\"ab\", 1, \"XX\"); "
                           "var deleted = delete_range(\"abcdef\", 2, 3); "
                           "var replaced = replace_range(\"abcdef\", -3, 2, \"XY\"); "
                           "var appended = insert(\"ab\", 20, \"!\"); "
                           "var clipped = delete_range(\"abcdef\", 4, 20);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String range edit Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "inserted"), "aXXb");
      check_str_eq(ts_get_str(ctx, "deleted"), "abf");
      check_str_eq(ts_get_str(ctx, "replaced"), "abcXYf");
      check_str_eq(ts_get_str(ctx, "appended"), "ab!");
      check_str_eq(ts_get_str(ctx, "clipped"), "abcd");

      turbo_script_free(ctx);
    }

    it("should support common string aliases and json escaping") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var inc = includes(\"hello\", \"ell\"); "
                           "var repeated = repeat(\"ab\", 2); "
                           "var counted = count(\"aaaa\", \"aa\"); "
                           "var replaced = replace_all(\"aabb\", \"a\", \"x\"); "
                           "var padded_left = padStart(\"x\", 3, \"0\"); "
                           "var padded_right = padEnd(\"x\", 3, \".\"); "
                           "var stripped = strip(\"  hi\\t\"); "
                           "var left = lstrip(\"  hi\"); "
                           "var right = rstrip(\"hi  \"); "
                           "var trimmed_start = trimStart(\"  js\"); "
                           "var trimmed_end = trimEnd(\"js  \"); "
                           "var json = json_escape(\"a\\n\\\"b\\\\c\"); "
                           "var raw = json_unescape(json); "
                           "var unicode = json_unescape(\"\\\\u4F60\\\\uD83D\\\\uDE42\"); "
                           "var score = inc + counted;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("String alias/json Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 3.0, 0.001);
      check_str_eq(ts_get_str(ctx, "repeated"), "abab");
      check_str_eq(ts_get_str(ctx, "replaced"), "xxbb");
      check_str_eq(ts_get_str(ctx, "padded_left"), "00x");
      check_str_eq(ts_get_str(ctx, "padded_right"), "x..");
      check_str_eq(ts_get_str(ctx, "stripped"), "hi");
      check_str_eq(ts_get_str(ctx, "left"), "hi");
      check_str_eq(ts_get_str(ctx, "right"), "hi");
      check_str_eq(ts_get_str(ctx, "trimmed_start"), "js");
      check_str_eq(ts_get_str(ctx, "trimmed_end"), "js");
      check_str_eq(ts_get_str(ctx, "json"), "a\\n\\\"b\\\\c");
      check_str_eq(ts_get_str(ctx, "raw"), "a\n\"b\\c");
      check_str_eq(ts_get_str(ctx, "unicode"), "\xE4\xBD\xA0" "\xF0\x9F\x99\x82");

      turbo_script_free(ctx);
    }

    it("should render mustache templates from script maps and lists") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "name:\"Ada\","
          "user:map{city:\"Paris\"},"
          "html:\"<b>\","
          "items:list(map{name:\"one\"}, map{name:\"two\"}),"
          "nums:[1,2],"
          "empty:list(),"
          "flag:1"
          "};"
          "var rendered = template_render(\"Hi {{name}} {{user.city}} {{html}} {{{html}}} "
          "{{#items}}{{name}},{{/items}}{{#nums}}{{.}};{{/nums}}{{^empty}}none{{/empty}} "
          "{{#flag}}yes{{/flag}}\", data);"
          "var alias = mustache_render(\"{{name}}\", data);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache template Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "rendered"),
                   "Hi Ada Paris &lt;b&gt; <b> one,two,1;2;none yes");
      check_str_eq(ts_get_str(ctx, "alias"), "Ada");

      turbo_script_free(ctx);
    }

    it("should render mustache lambdas from script functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "name:\"Ada\","
          "emit:func(raw) { return \"{{name}}\"; },"
          "wrap:func(raw) { return \"[{{name}}:\" + raw + \"]\"; }"
          "};"
          "var rendered = template_render(\"{{emit}} {{#wrap}}inside{{/wrap}}\", data);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache lambda Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "rendered"), "Ada [Ada:inside]");

      turbo_script_free(ctx);
    }

    it("should render mustache partials from a script map") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "items:list(map{name:\"one\"}, map{name:\"two\"}),"
          "total:2"
          "};"
          "var partials = map{"
          "item:\"{{name}};\","
          "summary:\"count={{total}}\","
          "outer:\"{{#items}}{{>item}}{{/items}}{{>summary}}\""
          "};"
          "var rendered = template_render(\"{{>outer}}\", data, partials);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache partial Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "rendered"), "one;two;count=2");

      turbo_script_free(ctx);
    }

    it("should render repeated mustache partials and ignore missing partials") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{name:\"Ada\"};"
          "var partials = map{item:\"{{name}}\"};"
          "var rendered = template_render(\"{{>item}}/{{>item}}/{{>missing}}\", data, partials);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache repeated partial Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "rendered"), "Ada/Ada/");

      turbo_script_free(ctx);
    }

    it("should stringify scalar mustache lambda results") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "num:func(raw) { return 42; },"
          "null_value:func(raw) { return null; }"
          "};"
          "var rendered = template_render(\"{{num}}/{{null_value}}\", data);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache scalar lambda Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "rendered"), "42/null");

      turbo_script_free(ctx);
    }

    it("should render nested mustache partial names with parent context lookup") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "var data = map{"
          "total:2,"
          "items:list(map{name:\"one\"}, map{name:\"two\"})"
          "};"
          "var partials = map{layout:map{item:\"{{name}}/{{total}};\"}};"
          "var rendered = template_render(\"{{#items}}{{>layout.item}}{{/items}}\", data, partials);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Mustache nested partial Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_str_eq(ts_get_str(ctx, "rendered"), "one/2;two/2;");

      turbo_script_free(ctx);
    }

  }

  describe("Scientific: Regex Builtins") {
    it("should expose core regex functions without loading a plugin") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var h = regex.compile(\"[0-9]+\"); "
                           "func has_digits(s) { return regex.search(h, s); } "
                           "var full = regex.match(h, \"12345\"); "
                           "var no_full = regex.match(h, \"123abc\"); "
                           "var pos = has_digits(\"abc123def\"); "
                           "var direct = regex.test(\"[a-z]+\", \"hello\"); "
                           "var freed = regex.free(h); "
                           "var score = full + no_full + pos + direct + freed;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Core regex Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 6.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support regex replace split and find_all") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var replaced = regex.replace(\"[0-9]+\", \"a12b345c\", \"#\"); "
                           "var once = regex.replace(\"[0-9]+\", \"a12b345c\", \"#\", 1); "
                           "var parts = regex.split(\"[|,]+\", \"a,b|c\"); "
                           "var matches = regex.find_all(\"[0-9]+\", \"a12b345c\"); "
                           "var p0 = parts[0]; "
                           "var p1 = parts[1]; "
                           "var p2 = parts[2]; "
                           "var m0 = matches[0]; "
                           "var m1 = matches[1]; "
                           "var score = parts.length() + matches.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Core regex transform Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 5.0, 0.001);
      check_str_eq(ts_get_str(ctx, "replaced"), "a#b#c");
      check_str_eq(ts_get_str(ctx, "once"), "a#b345c");
      check_str_eq(ts_get_str(ctx, "p0"), "a");
      check_str_eq(ts_get_str(ctx, "p1"), "b");
      check_str_eq(ts_get_str(ctx, "p2"), "c");
      check_str_eq(ts_get_str(ctx, "m0"), "12");
      check_str_eq(ts_get_str(ctx, "m1"), "345");

      turbo_script_free(ctx);
    }

    it("should reject legacy regex plugin loading paths") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check(turbo_script_load_plugin(ctx, "regex") != 0);
      turbo_script_free(ctx);

      ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check(turbo_script_run(ctx, "import(\"regex\");") != 0);
      turbo_script_free(ctx);
    }

    it("should support regex flags match info and iterators") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var ci = regex.match(\"abc\", \"ABC\", \"i\"); "
                           "var h = regex.compile(\"[a-z]+\", \"i\"); "
                           "var info = regex.match_info(h, \"12ABC34\"); "
                           "var iter = regex.find_iter(\"[0-9]+\", \"a12b345\"); "
                           "var info_text = info.text; "
                           "var iter0 = iter[0].text; "
                           "var iter1 = iter[1].text; "
                           "var score = ci + info.start + info.end + iter.length();";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Regex match info Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 10.0, 0.001);
      check_str_eq(ts_get_str(ctx, "info_text"), "ABC");
      check_str_eq(ts_get_str(ctx, "iter0"), "12");
      check_str_eq(ts_get_str(ctx, "iter1"), "345");

      turbo_script_free(ctx);
    }

    it("should support RegExp object API") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var re = RegExp(\"[0-9]+\"); "
                           "var ci = RegExp(\"abc\", \"i\"); "
                           "var found = re.test(\"a12b\"); "
                           "var full = re.match(\"123\"); "
                           "var no_full = re.match(\"a123\"); "
                           "var pos = re.search(\"a12b\"); "
                           "var info = re.exec(\"a12b\"); "
                           "var none = re.exec(\"abc\"); "
                           "var all = re.find_all(\"a12b345\"); "
                           "var source = re.toString(); "
                           "var score = found + full + no_full + pos + info.start + info.end + "
                           "            all.length() + ci.test(\"ABC\") + (none == null);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("RegExp object Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "score"), 11.0, 0.001);
      check_str_eq(ts_get_str(ctx, "source"), "[0-9]+");
      turbo_script_free(ctx);
    }

    it("should not expose capture groups in core libfsm regex") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "var info = regex.match_info(\"([0-9]+)\", \"a12b\"); "
                           "var has_groups = info.has(\"groups\"); "
                           "var dot_caught = try { regex.groups(\"([0-9]+)\", \"a12b\"); 0 } catch (e) { 1 }; "
                           "var flat_caught = try { regex_groups(\"([0-9]+)\", \"a12b\"); 0 } catch (e) { 1 }; "
                           "var score = has_groups + dot_caught + flat_caught;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Core regex groups rejection Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "score"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "has_groups"), 0.0, 0.001);
      turbo_script_free(ctx);
    }

  }

  describe("Scientific: Matrix Operations") {
    it("should compute determinant, inverse, matmul for 2x2") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          // Column-major matrix A = [[1,3],[2,4]], det = 1*4 - 3*2 = -2
          "A = [1, 2, 3, 4]; "
          "d = matrix.det2(A); "
          "Ainv = matrix.inv2(A); "
          "Ainv_len = vec.len(Ainv); "
          // Identity check: A * Ainv should be ~identity
          "I = matrix.matmul(A, Ainv, 2, 2, 2); "
          "i00 = I[0]; "
          "i11 = I[3];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "d"), -2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "Ainv_len"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "i00"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "i11"), 1.0, 0.01);

      turbo_script_free(ctx);
    }

    it("should use miniblas linalg backend for matrix helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "I3 = matrix.eye(3); "
          "A = [1, 2, 3, 4]; "
          "B = [5, 6, 7, 8]; "
          "G = matrix.gemm(A, B, 2, 2, 2); "
          "SPD = [4, 2, 2, 3]; "
          "L = matrix.cholesky(SPD, 2); "
          "X = matrix.solve_tri(L, [2, 1], 2, 1); "
          "UD = matrix.udu([4, 0, 0, 9], 2); "
          "U = UD[0]; "
          "D = UD[1]; "
          "i30 = I3[0]; "
          "g0 = G[0]; "
          "g3 = G[3]; "
          "l0 = L[0]; "
          "l1 = L[1]; "
          "l2 = L[2]; "
          "x0 = X[0]; "
          "x1 = X[1]; "
          "u0 = U[0]; "
          "u3 = U[3]; "
          "d0 = D[0]; "
          "d1 = D[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix backend Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "i30"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "g0"), 23.0, 0.01);
      check_float_eq(ts_get_num(ctx, "g3"), 46.0, 0.01);
      check_float_eq(ts_get_num(ctx, "l0"), 2.0, 0.01);
      check_float_eq(ts_get_num(ctx, "l1"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "l2"), 0.0, 0.01);
      check_float_eq(ts_get_num(ctx, "x0"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "x1"), 0.0, 0.01);
      check_float_eq(ts_get_num(ctx, "u0"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "u3"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "d0"), 4.0, 0.01);
      check_float_eq(ts_get_num(ctx, "d1"), 9.0, 0.01);
      turbo_script_free(ctx);
    }

    it("should allow explicit row-major matrix layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [1, 2, 3, 4]; "
          "B = [5, 6, 7, 8]; "
          "R = matrix.matmul(A, B, 2, 2, 2, 1); "
          "G = matrix.gemm(A, B, 2, 2, 2, 1); "
          "G2 = matrix.gemm(A, B, 2, 2, 2, 2, 0, 1); "
          "SPD = [4, 2, 2, 3]; "
          "L = matrix.cholesky(SPD, 2, 1); "
          "X = matrix.solve_tri(L, [2, 1], 2, 1, 1); "
          "UD = matrix.udu([4, 0, 0, 9], 2, 1); "
          "U = UD[0]; "
          "D = UD[1]; "
          "r0 = R[0]; "
          "r1 = R[1]; "
          "r2 = R[2]; "
          "r3 = R[3]; "
          "g0 = G[0]; "
          "g3 = G[3]; "
          "g20 = G2[0]; "
          "g23 = G2[3]; "
          "l0 = L[0]; "
          "l1 = L[1]; "
          "l2 = L[2]; "
          "x0 = X[0]; "
          "x1 = X[1]; "
          "u0 = U[0]; "
          "u3 = U[3]; "
          "d0 = D[0]; "
          "d1 = D[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Row-major matrix Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "r0"), 19.0, 0.01);
      check_float_eq(ts_get_num(ctx, "r1"), 22.0, 0.01);
      check_float_eq(ts_get_num(ctx, "r2"), 43.0, 0.01);
      check_float_eq(ts_get_num(ctx, "r3"), 50.0, 0.01);
      check_float_eq(ts_get_num(ctx, "g0"), 19.0, 0.01);
      check_float_eq(ts_get_num(ctx, "g3"), 50.0, 0.01);
      check_float_eq(ts_get_num(ctx, "g20"), 38.0, 0.01);
      check_float_eq(ts_get_num(ctx, "g23"), 100.0, 0.01);
      check_float_eq(ts_get_num(ctx, "l0"), 2.0, 0.01);
      check_float_eq(ts_get_num(ctx, "l1"), 0.0, 0.01);
      check_float_eq(ts_get_num(ctx, "l2"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "x0"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "x1"), 0.0, 0.01);
      check_float_eq(ts_get_num(ctx, "u0"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "u3"), 1.0, 0.01);
      check_float_eq(ts_get_num(ctx, "d0"), 4.0, 0.01);
      check_float_eq(ts_get_num(ctx, "d1"), 9.0, 0.01);
      turbo_script_free(ctx);
    }

    it("should expose matrix metadata and generic solve helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [2, 1, 1, 3]; "
          "shape = matrix.shape(A, 2, 2); "
          "info = matrix.info(A, 2, 2); "
          "order = info.order; "
          "dtype = info.dtype; "
          "Inv = matrix.inv(A, 2); "
          "X = matrix.solve(A, [5, 10], 2, 1); "
          "R = matrix.solve([2, 1, 1, 3], [5, 10], 2, 1, 1); "
          "rows = matrix.rows(A, 2, 2); "
          "cols = matrix.cols(A, 2, 2); "
          "s0 = shape[0]; "
          "s1 = shape[1]; "
          "inv0 = Inv[0]; "
          "inv3 = Inv[3]; "
          "x0 = X[0]; "
          "x1 = X[1]; "
          "r0 = R[0]; "
          "r1 = R[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix metadata/solve Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "s0"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "s1"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rows"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cols"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "inv0"), 0.6, 0.001);
      check_float_eq(ts_get_num(ctx, "inv3"), 0.4, 0.001);
      check_float_eq(ts_get_num(ctx, "x0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "x1"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r1"), 3.0, 0.001);
      check_str_eq(ts_get_str(ctx, "order"), "col");
      check_str_eq(ts_get_str(ctx, "dtype"), "f64");
      turbo_script_free(ctx);
    }

    it("should compute generic determinant and matrix norms") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Acol = [1, 0, 5, 2, 1, 6, 3, 4, 0]; "
          "Arow = [1, 2, 3, 0, 1, 4, 5, 6, 0]; "
          "Mcol = [1, -4, -2, 5, 3, -6]; "
          "Mrow = [1, -2, 3, -4, 5, -6]; "
          "d_col = matrix.det(Acol, 3); "
          "d_row = matrix.det(Arow, 3, 1); "
          "d_alias = mat_det(Arow, 3, 1); "
          "nf = matrix.norm([1, 2, 3, 4], 2, 2); "
          "nf_row = matrix.norm(Mrow, 2, 3, 1); "
          "nl1 = matrix.norm(Mcol, 2, 3, \"l1\"); "
          "ninf = matrix.norm(Mrow, 2, 3, \"inf\", 1); "
          "n_alias = mat_norm(Mcol, 2, 3, \"inf\");";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix determinant/norm Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "d_col"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "d_row"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "d_alias"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "nf"), sqrt(30.0), 0.001);
      check_float_eq(ts_get_num(ctx, "nf_row"), sqrt(91.0), 0.001);
      check_float_eq(ts_get_num(ctx, "nl1"), 9.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ninf"), 15.0, 0.001);
      check_float_eq(ts_get_num(ctx, "n_alias"), 15.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should extract generic matrix trace and diagonal") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 3, 5, 2, 4, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "t_col = matrix.trace(Mcol, 3, 2); "
          "t_row = matrix.trace(Mrow, 3, 2, 1); "
          "t_alias = mat_trace(Mrow, 3, 2, 1); "
          "d_col = matrix.diag(Mcol, 3, 2); "
          "d_row = matrix.diag(Mrow, 3, 2, 1); "
          "d_alias = mat_diag(Mrow, 3, 2, 1); "
          "d0 = d_col[0]; "
          "d1 = d_col[1]; "
          "r0 = d_row[0]; "
          "r1 = d_row[1]; "
          "a0 = d_alias[0]; "
          "a1 = d_alias[1]; "
          "dn = vec.len(d_col);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix trace/diag Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "t_col"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "t_row"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "t_alias"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "d0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "d1"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r1"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a1"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "dn"), 2.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should transpose generic matrices with explicit layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 4, 2, 5, 3, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "Tcol = matrix.transpose(Mcol, 2, 3); "
          "Trow = matrix.transpose(Mrow, 2, 3, 1); "
          "Talias = mat_transpose(Mrow, 2, 3, 1); "
          "Told = transpose(Mrow, 2, 3); "
          "c0 = Tcol[0]; c1 = Tcol[1]; c2 = Tcol[2]; c3 = Tcol[3]; c4 = Tcol[4]; c5 = Tcol[5]; "
          "r0 = Trow[0]; r1 = Trow[1]; r2 = Trow[2]; r3 = Trow[3]; r4 = Trow[4]; r5 = Trow[5]; "
          "a0 = Talias[0]; a5 = Talias[5]; "
          "o0 = Told[0]; o5 = Told[5];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix transpose Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "c0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c1"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c2"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c3"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c4"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c5"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r1"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r2"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r3"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r4"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r5"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a5"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "o0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "o5"), 6.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should construct dense matrices") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Z = matrix.zeros(2, 3); "
          "O = matrix.ones(2, 3, 1); "
          "F = matrix.full(2, 3, 7); "
          "A = mat_full(2, 3, 4, 1); "
          "S = matrix.shape(F, 2, 3); "
          "zn = vec.len(Z); "
          "z0 = Z[0]; z5 = Z[5]; "
          "o0 = O[0]; o5 = O[5]; "
          "f0 = F[0]; f5 = F[5]; "
          "a0 = A[0]; a5 = A[5]; "
          "s0 = S[0]; s1 = S[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix constructors Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "zn"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "z0"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "z5"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "o0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "o5"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "f0"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "f5"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a0"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a5"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "s0"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "s1"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should compute elementwise matrix operations") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [1, 2, 3, 4]; "
          "B = [10, 20, 30, 40]; "
          "S = matrix.add(A, B, 2, 2); "
          "D = matrix.sub(B, A, 2, 2, 1); "
          "H = matrix.hadamard(A, B, 2, 2); "
          "M = matrix.mul(A, B, 2, 2); "
          "C = matrix.scale(A, 2, 2, 2); "
          "Alias = mat_hadamard(A, B, 2, 2); "
          "s0 = S[0]; s3 = S[3]; "
          "d1 = D[1]; "
          "h2 = H[2]; "
          "m3 = M[3]; "
          "c3 = C[3]; "
          "a2 = Alias[2];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix elementwise Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "s0"), 11.0, 0.001);
      check_float_eq(ts_get_num(ctx, "s3"), 44.0, 0.001);
      check_float_eq(ts_get_num(ctx, "d1"), 18.0, 0.001);
      check_float_eq(ts_get_num(ctx, "h2"), 90.0, 0.001);
      check_float_eq(ts_get_num(ctx, "m3"), 160.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c3"), 8.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a2"), 90.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should reduce matrices globally and by axis") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 4, 2, 5, 3, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "s = matrix.sum(Mcol, 2, 3); "
          "avg = matrix.mean(Mcol, 2, 3); "
          "mn = matrix.min(Mcol, 2, 3); "
          "mx = matrix.max(Mcol, 2, 3); "
          "cs = matrix.sum(Mcol, 2, 3, \"col\"); "
          "rs = matrix.sum(Mrow, 2, 3, 1, 1); "
          "cm = matrix.mean(Mrow, 2, 3, 0, 1); "
          "rmax = matrix.max(Mrow, 2, 3, \"row\", 1); "
          "alias = mat_min(Mrow, 2, 3, 0, 1); "
          "cs0 = cs[0]; cs2 = cs[2]; "
          "rs0 = rs[0]; rs1 = rs[1]; "
          "cm0 = cm[0]; cm2 = cm[2]; "
          "rm0 = rmax[0]; rm1 = rmax[1]; "
          "a0 = alias[0]; a2 = alias[2];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix reduce Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "s"), 21.0, 0.001);
      check_float_eq(ts_get_num(ctx, "avg"), 3.5, 0.001);
      check_float_eq(ts_get_num(ctx, "mn"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "mx"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cs0"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cs2"), 9.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rs0"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rs1"), 15.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cm0"), 2.5, 0.001);
      check_float_eq(ts_get_num(ctx, "cm2"), 4.5, 0.001);
      check_float_eq(ts_get_num(ctx, "rm0"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rm1"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a2"), 3.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should extract matrix rows and columns with explicit layout") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "Mcol = [1, 4, 2, 5, 3, 6]; "
          "Mrow = [1, 2, 3, 4, 5, 6]; "
          "r = matrix.row(Mcol, 2, 3, 1); "
          "c = matrix.col(Mrow, 2, 3, 2, 1); "
          "ra = mat_row(Mrow, 2, 3, 0, 1); "
          "ca = mat_col(Mcol, 2, 3, 1); "
          "r0 = r[0]; r2 = r[2]; "
          "c0 = c[0]; c1 = c[1]; "
          "ra0 = ra[0]; ra2 = ra[2]; "
          "ca0 = ca[0]; ca1 = ca[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix row/col Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "r0"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "r2"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c0"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c1"), 6.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ra0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ra2"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ca0"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "ca1"), 5.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support matrix shape utilities dot products and broadcasting") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "M = [1, 2, 3, 4, 5, 6]; "
          "S = matrix.slice(M, 2, 3, 0, 2, 1, 2); "
          "R = matrix.reshape(M, 2, 3, 3, 2); "
          "F = matrix.flatten(M, 2, 3); "
          "C = matrix.copy(M, 2, 3); "
          "E = matrix.eye_like(M, 2, 3); "
          "I = matrix.identity(2); "
          "O = matrix.outer([1, 2], [10, 20, 30]); "
          "AR = matrix.add_row(M, [10, 20, 30], 2, 3); "
          "MC = matrix.mul_col(M, [2, 3], 2, 3); "
          "dotv = matrix.dot([1, 2, 3], [4, 5, 6]); "
          "result = S[0] + S[3] + R[5] + F[0] + C[5] + E[0] + E[3] + I[3] + "
          "O[0] + O[5] + AR[0] + AR[4] + MC[1] + dotv;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix utility Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "result"), 179.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should compute matrix variance std argmin and argmax") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "M = [1, 4, 2, 5, 3, 6]; "
          "v = matrix.variance(M, 2, 3); "
          "s = matrix.std(M, 2, 3); "
          "amin = matrix.argmin(M, 2, 3); "
          "amax = matrix.argmax(M, 2, 3); "
          "cv = mat_var(M, 2, 3, \"col\"); "
          "rs = matrix.std([1, 2, 3, 4, 5, 6], 2, 3, \"row\", 1); "
          "result = round(v * 1000) + round(s * 1000) + amin + amax + "
          "round(cv[0] * 1000) + round(rs[0] * 1000);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Matrix stats Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "result"), 7696.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should expose linalg helpers and distribution functions") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "QR = linalg.qr([1, 0, 0, 1], 2, 2); "
          "Q = QR[0]; R = QR[1]; "
          "P = linalg.pinv([1, 0, 0, 1], 2, 2); "
          "X = linalg.lstsq([1, 0, 0, 1], [7, 9], 2, 2); "
          "SV = linalg.svd([3, 0, 0, 4], 2, 2); "
          "EV = linalg.eig2([2, 0, 0, 3]); "
          "np = round(stats.normal_pdf(0) * 1000); "
          "nc = round(stats.normal_cdf(0) * 1000); "
          "nq = round(stats.normal_quantile(0.5) * 1000); "
          "tp = round(stats.t_pdf(0, 10) * 1000); "
          "tc = round(stats.t_cdf(0, 10) * 1000); "
          "tq = round(stats.t_quantile(0.5, 10) * 1000); "
          "tt = stats.t_test_1samp([1, 2, 3, 4], 2.5); "
          "result = Q[0] + R[3] + P[0] + X[0] + X[1] + SV[0] + SV[1] + "
          "EV[0] + EV[1] + np + nc + nq + tp + tc + tq + tt.df;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Linalg/stats Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "result"), 1822.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should expose extended linear algebra helpers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "A = [4, 2, 2, 3]; "
          "LU = linalg.lu(A, 2); "
          "L = LU[0]; U = LU[1]; P = LU[2]; "
          "X = linalg.lu_solve(A, [8, 8], 2, 1); "
          "C = linalg.solve_cholesky(A, [8, 8], 2, 1); "
          "rank_full = matrix.rank(A, 2, 2); "
          "rank_def = matrix.rank([1, 2, 2, 4], 2, 2); "
          "cond = matrix.cond([2, 0, 0, 4], 2, 2); "
          "E = linalg.eigh([2, 1, 1, 2], 2); "
          "S = linalg.svd([1, 0, 0, 0, 2, 0], 3, 2); "
          "det_lu = linalg.det_lu(A, 2); "
          "SL = matrix.slogdet(A, 2); "
          "RLU = linalg.lu([0, 1, 2, 3], 2, 1); "
          "RP = RLU[2]; "
          "RX = linalg.lu_solve([0, 1, 2, 3], [1, 5], 2, 1, 1); "
          "l0 = L[0]; l1 = L[1]; u0 = U[0]; u3 = U[3]; "
          "p0 = P[0]; p1 = P[1]; "
          "x0 = X[0]; x1 = X[1]; c0 = C[0]; c1 = C[1]; "
          "e0 = E[0]; e1 = E[1]; s0 = S[0]; s1 = S[1]; "
          "sl0 = SL[0]; sl1 = round(SL[1] * 1000); "
          "rp0 = RP[0]; rp1 = RP[1]; rx0 = RX[0]; rx1 = RX[1];";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Extended linalg Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "l0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "l1"), 0.5, 0.001);
      check_float_eq(ts_get_num(ctx, "u0"), 4.0, 0.001);
      check_float_eq(ts_get_num(ctx, "u3"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "p0"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "p1"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "x0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "x1"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "c1"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rank_full"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rank_def"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cond"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "e0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "e1"), 3.0, 0.001);
      check_float_eq(ts_get_num(ctx, "s0"), 2.0, 0.001);
      check_float_eq(ts_get_num(ctx, "s1"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "det_lu"), 8.0, 0.001);
      check_float_eq(ts_get_num(ctx, "sl0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "sl1"), 2079.0, 1.0);
      check_float_eq(ts_get_num(ctx, "rp0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rp1"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rx0"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "rx1"), 1.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support table select filter groupby and join") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script =
          "rows = list(map{id:1, group:\"a\", value:10}, map{id:2, group:\"a\", value:20}, "
          "map{id:3, group:\"b\", value:5}); "
          "right = list(map{id:1, name:\"one\"}, map{id:3, name:\"three\"}); "
          "func keep_big(row) { return row.value > 9; } "
          "sel = table.select(rows, \"id\", \"value\"); "
          "flt = table.filter(rows, \"keep_big\"); "
          "grp = table.groupby(rows, \"group\", \"value\", \"mean\"); "
          "joined = table.join(rows, right, \"id\"); "
          "result = sel.length() + sel[0].value + flt.length() + grp.a.value + grp.b.value + "
          "joined.length() + joined[1].id;";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Table Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "result"), 40.0, 0.001);
      turbo_script_free(ctx);
    }
  }

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
