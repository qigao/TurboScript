/**
 * @file test_data_bind.c
 * @brief Unit tests for data_bind dynamic schema/value runtime.
 */

#include "data_bind.h"
#include "tbe_wire.h"
#include "tinytest.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const DataBindValue *require_field(const DataBindValue *obj, const char *name) {
  const DataBindValue *field = data_bind_value_get(obj, name);
  check_not_null(field);
  return field;
}

static const DataBindValue *require_index(const DataBindValue *list, size_t index) {
  const DataBindValue *item = data_bind_value_at(list, index);
  check_not_null(item);
  return item;
}

static void write_schema(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  check_not_null(f);
  if (!f) return;
  fwrite(content, 1, strlen(content), f);
  fclose(f);
}

static void write_u16_le(uint8_t *buf, size_t offset, uint16_t value) {
  tbe_wire_write_u16(buf + offset, 0, value);
}

static void write_u32_le(uint8_t *buf, size_t offset, uint32_t value) {
  tbe_wire_write_u32(buf + offset, 0, value);
}

static void write_i32_le(uint8_t *buf, size_t offset, int32_t value) {
  tbe_wire_write_i32(buf + offset, 0, value);
}

static void write_u64_le(uint8_t *buf, size_t offset, uint64_t value) {
  tbe_wire_write_u64(buf + offset, 0, value);
}

static void write_f32_le(uint8_t *buf, size_t offset, float value) {
  tbe_wire_write_f32(buf + offset, 0, value);
}

static void write_f64_le(uint8_t *buf, size_t offset, double value) {
  tbe_wire_write_f64(buf + offset, 0, value);
}

static DataBind *test_data_bind_create(const char *schema_path) {
  DataBind *codec = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_create(schema_path, &codec, &err) != DATA_BIND_OK) return NULL;
  return codec;
}

static DataBindValue *test_data_bind_parse(DataBind *codec, const char *type_name,
                                           const uint8_t *buf, size_t len) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse(codec, type_name, buf, len, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

static DataBindStatus test_data_bind_parse_status(DataBind *codec, const char *type_name,
                                                  const uint8_t *buf, size_t len,
                                                  DataBindValue **out_value,
                                                  DataBindError *err) {
  return data_bind_parse(codec, type_name, buf, len, out_value, err);
}

static DataBindValue *test_data_bind_parse_json(DataBind *codec, const char *type_name,
                                                const char *json, size_t len) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse_json(codec, type_name, json, len, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

static DataBindValue *test_data_bind_parse_json_all(DataBind *codec, const char *type_name,
                                                    const char *json, size_t len) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse_json_all(codec, type_name, json, len, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

static DataBindValue *test_data_bind_parse_csv(DataBind *codec, const char *type_name,
                                               const char *csv, size_t len, size_t row) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse_csv(codec, type_name, csv, len, row, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

static DataBindValue *test_data_bind_parse_csv_all(DataBind *codec, const char *type_name,
                                                   const char *csv, size_t len) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse_csv_all(codec, type_name, csv, len, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

static DataBindValue *test_data_bind_parse_xml(DataBind *codec, const char *type_name,
                                               const char *xml, size_t len) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse_xml(codec, type_name, xml, len, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

static DataBindValue *test_data_bind_parse_xml_all(DataBind *codec, const char *type_name,
                                                   const char *xml, size_t len, const char *xpath) {
  DataBindValue *value = NULL;
  DataBindError err = DATA_BIND_ERROR_INIT;
  if (data_bind_parse_xml_all(codec, type_name, xml, len, xpath, &value, &err) != DATA_BIND_OK) return NULL;
  return value;
}

#define data_bind_create(path) test_data_bind_create(path)
#define data_bind_parse(codec, type_name, buf, len) \
  test_data_bind_parse((codec), (type_name), (buf), (len))
#define data_bind_parse_json(codec, type_name, json, len) \
  test_data_bind_parse_json((codec), (type_name), (json), (len))
#define data_bind_parse_json_all(codec, type_name, json, len) \
  test_data_bind_parse_json_all((codec), (type_name), (json), (len))
#define data_bind_parse_csv(codec, type_name, csv, len, row) \
  test_data_bind_parse_csv((codec), (type_name), (csv), (len), (row))
#define data_bind_parse_csv_all(codec, type_name, csv, len) \
  test_data_bind_parse_csv_all((codec), (type_name), (csv), (len))
#define data_bind_parse_xml(codec, type_name, xml, len) \
  test_data_bind_parse_xml((codec), (type_name), (xml), (len))
#define data_bind_parse_xml_all(codec, type_name, xml, len, xpath) \
  test_data_bind_parse_xml_all((codec), (type_name), (xml), (len), (xpath))

suite("Data Bind") {
  section("Codec Creation") {
    given("a valid schema file") {
      write_schema("test_create.tbe", "message Ping { uint32 seq; }\n");

      when("creating codec from schema") {
        DataBind *codec = data_bind_create("test_create.tbe");
        then("codec should be non-null") { check_not_null(codec); }
        data_bind_free(codec);
      }

      remove("test_create.tbe");
    }

    given("a nonexistent schema file") {
      when("creating codec") {
        DataBind *codec = data_bind_create("no_such_file.tbe");
        then("should return NULL") { check_null(codec); }
      }
    }

    given("a NULL schema path") {
      when("creating codec") {
        DataBind *codec = data_bind_create(NULL);
        then("should return NULL") { check_null(codec); }
      }
    }

    given("a schema with a datetime field") {
      const char *dt_text = "Sat, 04 Mar 2006 13:27:54 GMT";
      write_schema("test_datetime.tbe", "message Event { datetime at; }\n");

      DataBind *codec = data_bind_create("test_datetime.tbe");
      check_not_null(codec);

      if (codec) {
        when("binding JSON CSV and XML text") {
          const char *json = "{\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\"}";
          const char *csv = "at\n\"Sat, 04 Mar 2006 13:27:54 GMT\"\n";
          const char *xml = "<event><at>Sat, 04 Mar 2006 13:27:54 GMT</at></event>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Event", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Event", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Event", xml, strlen(xml));

          then("all text formats should produce native datetime values") {
            const DataBindValue *at;
            turbo_datetime_t dt;
            char text[64];
            check_not_null(from_json);
            at = require_field(from_json, "at");
            check(data_bind_value_kind(at) == DATA_BIND_VALUE_DATETIME);
            check(data_bind_value_as_datetime(at, &dt));
            check_int_eq(dt.year, 2006);
            check_double_eq(data_bind_value_as_datetime_timestamp(at), 1141478874.0, 0.001);
            check_not_null(data_bind_value_as_datetime_string(at, text, sizeof(text)));

            check_not_null(from_csv);
            at = require_field(from_csv, "at");
            check(data_bind_value_kind(at) == DATA_BIND_VALUE_DATETIME);
            check_double_eq(data_bind_value_as_datetime_timestamp(at), 1141478874.0, 0.001);

            check_not_null(from_xml);
            at = require_field(from_xml, "at");
            check(data_bind_value_kind(at) == DATA_BIND_VALUE_DATETIME);
            check_double_eq(data_bind_value_as_datetime_timestamp(at), 1141478874.0, 0.001);
          }

          (void)dt_text;
          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        data_bind_free(codec);
      }

      remove("test_datetime.tbe");
    }

    given("a schema with date time and duration fields") {
      write_schema("test_temporal_scalars.tbe",
                   "message Event { date d; time t; duration span; }\n");

      DataBind *codec = data_bind_create("test_temporal_scalars.tbe");
      check_not_null(codec);

      if (codec) {
        when("binding JSON CSV and XML text") {
          const char *json = "{\"d\":\"2026-06-28\",\"t\":\"09:30:05.123\",\"span\":\"1h30m5s250ms\"}";
          const char *csv = "d,t,span\n2026-06-28,09:30:05.123,1h30m5s250ms\n";
          const char *xml =
              "<event><d>2026-06-28</d><t>09:30:05.123</t>"
              "<span>1h30m5s250ms</span></event>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Event", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Event", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Event", xml, strlen(xml));

          then("all text formats should produce native temporal values") {
            const DataBindValue *d;
            const DataBindValue *t;
            const DataBindValue *span;
            DataBindDate date;
            DataBindTime time;
            int64_t duration_ms = 0;
            char text[64];

            check_not_null(from_json);
            d = require_field(from_json, "d");
            t = require_field(from_json, "t");
            span = require_field(from_json, "span");
            check(data_bind_value_kind(d) == DATA_BIND_VALUE_DATE);
            check(data_bind_value_kind(t) == DATA_BIND_VALUE_TIME);
            check(data_bind_value_kind(span) == DATA_BIND_VALUE_DURATION);
            check(data_bind_value_get_date(d, &date) == DATA_BIND_OK);
            check_int_eq(date.year, 2026);
            check_int_eq(date.month, 6);
            check_int_eq(date.day, 28);
            check(data_bind_value_get_time(t, &time) == DATA_BIND_OK);
            check_int_eq(time.hour, 9);
            check_int_eq(time.millisecond, 123);
            check(data_bind_value_get_duration_milliseconds(span, &duration_ms) == DATA_BIND_OK);
            check_int_eq((int)duration_ms, 5405250);
            check_str_eq(data_bind_value_as_date_string(d, text, sizeof(text)), "2026-06-28");
            check_str_eq(data_bind_value_as_time_string(t, text, sizeof(text)), "09:30:05.123");
            check_str_eq(data_bind_value_as_duration_string(span, text, sizeof(text)),
                         "1:30:05.250");

            check_not_null(from_csv);
            d = require_field(from_csv, "d");
            t = require_field(from_csv, "t");
            span = require_field(from_csv, "span");
            check(data_bind_value_kind(d) == DATA_BIND_VALUE_DATE);
            check(data_bind_value_kind(t) == DATA_BIND_VALUE_TIME);
            check(data_bind_value_as_duration_milliseconds(span) == 5405250LL);

            check_not_null(from_xml);
            d = require_field(from_xml, "d");
            t = require_field(from_xml, "t");
            span = require_field(from_xml, "span");
            check(data_bind_value_kind(d) == DATA_BIND_VALUE_DATE);
            check(data_bind_value_kind(t) == DATA_BIND_VALUE_TIME);
            check(data_bind_value_as_duration_milliseconds(span) == 5405250LL);
          }

          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        when("validating invalid temporal text") {
          DataBindError err = DATA_BIND_ERROR_INIT;
          const char *bad_json = "{\"d\":\"2026-02-31\",\"t\":\"25:00:00\",\"span\":\"soon\"}";
          then("validation should reject malformed temporal values") {
            check(data_bind_validate_json(codec, "Event", bad_json, strlen(bad_json), &err) !=
                  DATA_BIND_OK);
          }
        }

        data_bind_free(codec);
      }

      remove("test_temporal_scalars.tbe");
    }

    given("a schema with a decimal field") {
      write_schema("test_decimal_scalars.tbe", "message Quote { decimal price; }\n");

      DataBind *codec = data_bind_create("test_decimal_scalars.tbe");
      check_not_null(codec);

      if (codec) {
        when("binding JSON CSV and XML text") {
          const char *json = "{\"price\":\"123.4500\"}";
          const char *csv = "price\n-0.1250\n";
          const char *xml = "<quote><price>42.00</price></quote>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Quote", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Quote", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Quote", xml, strlen(xml));

          then("all text formats should produce native decimal values") {
            const DataBindValue *price;
            DataBindDecimal decimal;
            char text[64];

            check_not_null(from_json);
            price = require_field(from_json, "price");
            check(data_bind_value_kind(price) == DATA_BIND_VALUE_DECIMAL);
            check(data_bind_value_as_decimal(price, &decimal));
            check_int_eq((int)decimal.mantissa, 12345);
            check_int_eq(decimal.scale, 2);
            check_str_eq(data_bind_value_as_decimal_string(price, text, sizeof(text)),
                         "123.45");

            check_not_null(from_csv);
            price = require_field(from_csv, "price");
            check(data_bind_value_kind(price) == DATA_BIND_VALUE_DECIMAL);
            check(data_bind_value_get_decimal(price, &decimal) == DATA_BIND_OK);
            check_int_eq((int)decimal.mantissa, -125);
            check_int_eq(decimal.scale, 3);

            check_not_null(from_xml);
            price = require_field(from_xml, "price");
            check(data_bind_value_kind(price) == DATA_BIND_VALUE_DECIMAL);
            check_str_eq(data_bind_value_as_decimal_string(price, text, sizeof(text)), "42");
          }

          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        when("validating invalid decimal text") {
          DataBindError err = DATA_BIND_ERROR_INIT;
          const char *bad_json = "{\"price\":\"12.3.4\"}";
          then("validation should reject malformed decimal values") {
            check(data_bind_validate_json(codec, "Quote", bad_json, strlen(bad_json), &err) !=
                  DATA_BIND_OK);
          }
        }

        data_bind_free(codec);
      }

      remove("test_decimal_scalars.tbe");
    }

    given("a schema with bigint and money fields") {
      write_schema("test_bigint_money_scalars.tbe",
                   "message Invoice { bigint id; money total; }\n");

      DataBind *codec = data_bind_create("test_bigint_money_scalars.tbe");
      check_not_null(codec);

      if (codec) {
        when("binding JSON CSV and XML text") {
          const char *json =
              "{\"id\":\"000123456789012345678901234567890\","
              "\"total\":{\"amount\":\"123.4500\",\"currency\":\"USD\"}}";
          const char *csv = "id,total\n-00042,EUR 99.9900\n";
          const char *xml =
              "<invoice><id>+0007</id><total>12.3400 JPY</total></invoice>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Invoice", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Invoice", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Invoice", xml, strlen(xml));

          then("bigint should stay exact and money should keep decimal plus currency") {
            const DataBindValue *id;
            const DataBindValue *total;
            DataBindMoney money;
            const char *bigint = NULL;
            size_t bigint_len = 0;
            char text[64];

            check_not_null(from_json);
            id = require_field(from_json, "id");
            total = require_field(from_json, "total");
            check(data_bind_value_kind(id) == DATA_BIND_VALUE_BIGINT);
            check(data_bind_value_kind(total) == DATA_BIND_VALUE_MONEY);
            check_int_eq(data_bind_value_get_bigint(id, &bigint, &bigint_len), DATA_BIND_OK);
            check_str_eq(bigint, "123456789012345678901234567890");
            check_size_eq(bigint_len, strlen("123456789012345678901234567890"));
            check(data_bind_value_as_money(total, &money));
            check_int_eq((int)money.amount.mantissa, 12345);
            check_int_eq(money.amount.scale, 2);
            check_str_eq(money.currency, "USD");
            check_str_eq(data_bind_value_as_money_string(total, text, sizeof(text)),
                         "USD 123.45");

            check_not_null(from_csv);
            check_str_eq(data_bind_value_as_bigint_string(require_field(from_csv, "id")),
                         "-42");
            check_int_eq(data_bind_value_get_money(require_field(from_csv, "total"), &money),
                         DATA_BIND_OK);
            check_str_eq(money.currency, "EUR");
            check_int_eq((int)money.amount.mantissa, 9999);
            check_int_eq(money.amount.scale, 2);

            check_not_null(from_xml);
            check_str_eq(data_bind_value_as_bigint_string(require_field(from_xml, "id")), "7");
            check_str_eq(data_bind_value_as_money_string(require_field(from_xml, "total"),
                                                        text, sizeof(text)),
                         "JPY 12.34");
          }

          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        when("validating invalid bigint and money values") {
          DataBindError err = DATA_BIND_ERROR_INIT;
          const char *bad_json = "{\"id\":\"12.3\",\"total\":\"usd 1.00\"}";
          then("validation should reject malformed scalar values") {
            check(data_bind_validate_json(codec, "Invoice", bad_json, strlen(bad_json), &err) !=
                  DATA_BIND_OK);
          }
        }

        data_bind_free(codec);
      }

      remove("test_bigint_money_scalars.tbe");
    }

    given("a schema with a bytes field") {
      write_schema("test_bytes_text.tbe", "message Blob { bytes raw; }\n");

      DataBind *codec = data_bind_create("test_bytes_text.tbe");
      check_not_null(codec);

      if (codec) {
        when("binding JSON CSV and XML text") {
          const char *json = "{\"raw\":\"Az\"}";
          const char *csv = "raw\nAz\n";
          const char *xml = "<blob><raw>Az</raw></blob>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Blob", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Blob", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Blob", xml, strlen(xml));

          then("all text formats should produce native bytes values") {
            const DataBindValue *raw;
            const uint8_t *bytes;
            size_t len = 0;

            check_not_null(from_json);
            raw = require_field(from_json, "raw");
            check(data_bind_value_kind(raw) == DATA_BIND_VALUE_BYTES);
            bytes = data_bind_value_as_bytes(raw, &len);
            check_size_eq(len, 2);
            check_mem_eq(bytes, "Az", 2);

            check_not_null(from_csv);
            raw = require_field(from_csv, "raw");
            check(data_bind_value_kind(raw) == DATA_BIND_VALUE_BYTES);
            bytes = data_bind_value_as_bytes(raw, &len);
            check_size_eq(len, 2);
            check_mem_eq(bytes, "Az", 2);

            check_not_null(from_xml);
            raw = require_field(from_xml, "raw");
            check(data_bind_value_kind(raw) == DATA_BIND_VALUE_BYTES);
            bytes = data_bind_value_as_bytes(raw, &len);
            check_size_eq(len, 2);
            check_mem_eq(bytes, "Az", 2);
          }

          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        data_bind_free(codec);
      }

      remove("test_bytes_text.tbe");
    }

    given("a schema with formatted string fields") {
      write_schema("test_string_formats.tbe",
                   "message Endpoint {\n"
                   "  [format(ipaddr)] string ip;\n"
                   "  [format(cidr)] string net;\n"
                   "  [format(url)] string href;\n"
                   "  [format(email)] string owner;\n"
                   "  [format(macaddr)] string mac;\n"
                   "  [format(semver)] string version;\n"
                   "  [format(base64url)] string token;\n"
                   "  [format(currency)] string currency;\n"
                   "  [format(json_pointer)] string pointer;\n"
                   "  [format(jsonpath)] string jsonpath;\n"
                   "  [format(xpath)] string xpath;\n"
                   "  [format(cron)] string cron;\n"
                   "  [format(color)] string color;\n"
                   "  [format(mime)] string mime;\n"
                   "  [format(regex)] string regex;\n"
                   "}\n");

      DataBind *codec = data_bind_create("test_string_formats.tbe");
      check_not_null(codec);

      if (codec) {
        DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;

        when("binding JSON CSV and XML text") {
          const char *json =
              "{\"ip\":\"2001:db8::1\",\"net\":\"192.168.1.0/24\","
              "\"href\":\"https://example.com/a?q=1\",\"owner\":\"dev@example.com\","
              "\"mac\":\"aa:bb:cc:dd:ee:ff\",\"version\":\"1.2.3-alpha+7\","
              "\"token\":\"aGVsbG8_\",\"currency\":\"USD\","
              "\"pointer\":\"/items/0/name\",\"jsonpath\":\"$.items[0].name\","
              "\"xpath\":\"//item[@id='a']\",\"cron\":\"*/5 * * * *\","
              "\"color\":\"#33aaff\",\"mime\":\"application/json\","
              "\"regex\":\"^[A-Z]+$\"}";
          const char *csv =
              "ip,net,href,owner,mac,version,token,currency,pointer,jsonpath,xpath,cron,color,mime,regex\n"
              "127.0.0.1,2001:db8::/32,https://example.com,dev@example.com,"
              "AA-BB-CC-DD-EE-FF,1.2.3,aGVsbG8,EUR,/a~1b,$.a,//a,0 0 * * *,#abc,text/plain,^[0-9]+$\n";
          const char *xml =
              "<endpoint><ip>127.0.0.1</ip><net>192.168.0.0/16</net>"
              "<href>https://example.com</href><owner>dev@example.com</owner>"
              "<mac>aa:bb:cc:dd:ee:ff</mac><version>1.2.3</version>"
              "<token>aGVsbG8_</token><currency>JPY</currency>"
              "<pointer>/</pointer><jsonpath>$</jsonpath><xpath>/endpoint/ip</xpath>"
              "<cron>0 12 * * MON-FRI</cron><color>#11223344</color>"
              "<mime>image/png</mime><regex>^[a-z]{2}$</regex></endpoint>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Endpoint", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Endpoint", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Endpoint", xml, strlen(xml));

          then("formatted values should remain strings after validation") {
            const DataBindValue *ip;
            check_not_null(from_json);
            ip = require_field(from_json, "ip");
            check(data_bind_value_kind(ip) == DATA_BIND_VALUE_STRING);
            check_str_eq(data_bind_value_as_string(ip), "2001:db8::1");

            check_not_null(from_csv);
            check_str_eq(data_bind_value_as_string(require_field(from_csv, "currency")), "EUR");

            check_not_null(from_xml);
            check_str_eq(data_bind_value_as_string(require_field(from_xml, "owner")),
                         "dev@example.com");

            check(data_bind_schema_field_at(codec, "Endpoint", 0, &field) == 1);
            check_str_eq(field.format, "ipaddr");
            check(data_bind_schema_field_at(codec, "Endpoint", 2, &field) == 1);
            check_str_eq(field.format, "url");
            check(data_bind_schema_field_at(codec, "Endpoint", 14, &field) == 1);
            check_str_eq(field.format, "regex");
          }

          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        when("validating invalid formatted strings") {
          DataBindError err = DATA_BIND_ERROR_INIT;
          const char *bad_json =
              "{\"ip\":\"999.1.1.1\",\"net\":\"192.168.1.0/99\","
              "\"href\":\"not a url\",\"owner\":\"not-email\","
              "\"mac\":\"aa:bb:cc\",\"version\":\"01.2.3\","
              "\"token\":\"###\",\"currency\":\"usd\","
              "\"pointer\":\"bad\",\"jsonpath\":\"items[0]\",\"xpath\":\"//a[\","
              "\"cron\":\"* * *\",\"color\":\"#12xz\",\"mime\":\"bad mime\","
              "\"regex\":\"[unterminated\"}";
          then("validation should reject values that do not match their format") {
            check(data_bind_validate_json(codec, "Endpoint", bad_json, strlen(bad_json), &err) !=
                  DATA_BIND_OK);
          }
        }

        data_bind_free(codec);
      }

      remove("test_string_formats.tbe");
    }
  }

  section("Schema Reflection") {
    given("a schema with records enums flags unions and containers") {
      write_schema("test_reflect.tbe", "schema Market [id(1), version(2), byte_order(little)];\n"
                                      "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
                                      "flags Perms <uint8> { Read = 1; Write = 2; }\n"
                                      "composite Header { uint32 seq; uint64 ts; }\n"
                                      "group Level { uint64 price; uint32 qty; }\n"
                                      "union Choice { Side side; Header header; }\n"
                                      "message Book {\n"
                                      "    Header header;\n"
                                      "    optional uint32 venue default 7;\n"
                                      "    bytes(16) digest;\n"
                                      "    group<Level> bids;\n"
                                      "    string symbol;\n"
                                      "    map<string,uint32> attrs;\n"
                                      "}\n");

      DataBind *codec = data_bind_create("test_reflect.tbe");
      check_not_null(codec);

      if (codec) {
        DataBindSchemaType type = DATA_BIND_SCHEMA_TYPE_INIT;
        DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
        DataBindSchemaEnumItem item = DATA_BIND_SCHEMA_ENUM_ITEM_INIT;
        DataBindSchemaAttribute attr = DATA_BIND_SCHEMA_ATTRIBUTE_INIT;

        then("schema should expose declared types") {
          check_str_eq(data_bind_schema_name(codec), "Market");
          check_size_eq(data_bind_schema_attribute_count(codec), 3);
          check(data_bind_schema_attribute_at(codec, 2, &attr) == 1);
          check_str_eq(attr.name, "byte_order");
          check_str_eq(attr.value, "little");
          check_str_eq(data_bind_schema_attribute_get(codec, "version"), "2");

          check_size_eq(data_bind_schema_type_count(codec), 6);
          check_size_eq(data_bind_schema_enum_count(codec), 2);

          check(data_bind_schema_find_type(codec, "Book", &type) == 1);
          check_str_eq(type.name, "Book");
          check(type.kind == DATA_BIND_SCHEMA_MESSAGE);
          check_str_eq(data_bind_schema_kind_name(type.kind), "message");
          check_size_eq(type.field_count, 6);
          check(type.has_fixed_block_size == 1);
          check_size_eq(type.fixed_block_size, 33);

          check(data_bind_schema_find_type(codec, "Header", &type) == 1);
          check(type.kind == DATA_BIND_SCHEMA_COMPOSITE);
          check_size_eq(type.fixed_block_size, 12);

          check(data_bind_schema_find_type(codec, "Level", &type) == 1);
          check(type.kind == DATA_BIND_SCHEMA_GROUP);
          check_size_eq(type.fixed_block_size, 12);

          check(data_bind_schema_find_type(codec, "Choice", &type) == 1);
          check(type.kind == DATA_BIND_SCHEMA_UNION);
          check_size_eq(type.field_count, 2);

          check(data_bind_schema_find_type(codec, "Perms", &type) == 1);
          check(type.kind == DATA_BIND_SCHEMA_FLAGS);
          check_size_eq(type.item_count, 2);
        }

        then("schema should expose field metadata") {
          check_size_eq(data_bind_schema_field_count(codec, "Book"), 6);

          check(data_bind_schema_field_at(codec, "Book", 0, &field) == 1);
          check_str_eq(field.name, "header");
          check_str_eq(field.kind, "composite");
          check(field.is_composite == 1);
          check_size_eq(field.offset, 1);

          check(data_bind_schema_field_at(codec, "Book", 1, &field) == 1);
          check_str_eq(field.name, "venue");
          check_str_eq(field.kind, "scalar");
          check(field.is_optional == 1);
          check(field.has_default == 1);
          check_str_eq(field.default_value, "7");
          check_size_eq(field.offset, 13);

          check(data_bind_schema_field_at(codec, "Book", 2, &field) == 1);
          check_str_eq(field.name, "digest");
          check_str_eq(field.kind, "bytes");
          check_size_eq(field.size_bytes, 16);

          check(data_bind_schema_field_at(codec, "Book", 3, &field) == 1);
          check_str_eq(field.name, "bids");
          check_str_eq(field.kind, "group");
          check_str_eq(field.group_type, "Level");

          check(data_bind_schema_field_at(codec, "Book", 5, &field) == 1);
          check_str_eq(field.name, "attrs");
          check_str_eq(field.kind, "map");
          check_str_eq(field.key_type, "string");
          check_str_eq(field.value_type, "uint32");
        }

        then("schema should expose union fields and enum items") {
          check(data_bind_schema_field_at(codec, "Choice", 0, &field) == 1);
          check_str_eq(field.name, "side");
          check_str_eq(field.kind, "enum");
          check(field.is_enum == 1);

          check(data_bind_schema_enum_at(codec, 0, &type) == 1);
          check_str_eq(type.name, "Side");
          check(type.kind == DATA_BIND_SCHEMA_ENUM);
          check_str_eq(type.underlying_type, "uint8");
          check_size_eq(type.item_count, 2);
          check(data_bind_schema_enum_item_at(codec, "Side", 0, &item) == 1);
          check_str_eq(item.name, "Buy");
          check_str_eq(item.value, "1");
        }

        data_bind_free(codec);
      }

      remove("test_reflect.tbe");
    }
  }

  section("Primitive Type Parsing") {
    given("a schema with uint8, uint16, uint32, uint64") {
      write_schema("test_prim.tbe", "message Primitives {\n"
                                    "    uint8 a;\n"
                                    "    uint16 b;\n"
                                    "    uint32 c;\n"
                                    "    uint64 d;\n"
                                    "}\n");

      DataBind *codec = data_bind_create("test_prim.tbe");
      check_not_null(codec);

      if (codec) {
        when("parsing binary data with known values") {
          uint8_t buf[15];
          DataBindValue *v;
          memset(buf, 0, sizeof(buf));
          buf[0] = 0xAB;
          write_u16_le(buf, 1, 0x1234);
          write_u32_le(buf, 3, 42);
          write_u64_le(buf, 7, 1000000ULL);

          v = data_bind_parse(codec, "Primitives", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }
          then("fields should contain parsed values") {
            check_int_eq(data_bind_value_as_int(require_field(v, "a")), 171);
            check_int_eq(data_bind_value_as_int(require_field(v, "b")), 4660);
            check_int_eq(data_bind_value_as_int(require_field(v, "c")), 42);
            check_int_eq(data_bind_value_as_int64(require_field(v, "d")), 1000000);
          }

          data_bind_value_free(v);
        }

        data_bind_free(codec);
      }

      remove("test_prim.tbe");
    }

    given("a schema with a uuid field") {
      const char *id_text = "01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001";
      uuid_t expected;
      write_schema("test_uuid.tbe", "message Event { uuid id; }\n");

      DataBind *codec = data_bind_create("test_uuid.tbe");
      check_not_null(codec);
      check(uuid_from_s(id_text, &expected));

      if (codec) {
        when("parsing binary data") {
          DataBindValue *v = data_bind_parse(codec, "Event", expected.bytes, sizeof(expected.bytes));

          then("uuid should bind from its fixed 16 byte payload") {
            const DataBindValue *id;
            uuid_t actual;
            char text[UUID4_STR_BUFFER_SIZE];
            check_not_null(v);
            id = require_field(v, "id");
            check(data_bind_value_kind(id) == DATA_BIND_VALUE_UUID);
            check(data_bind_value_get_uuid(id, &actual) == DATA_BIND_OK);
            check_mem_eq(actual.bytes, expected.bytes, sizeof(expected.bytes));
            check_str_eq(data_bind_value_as_uuid_string(id, text, sizeof(text)), id_text);
          }

          data_bind_value_free(v);
        }

        when("binding JSON CSV and XML text") {
          const char *json = "{\"id\":\"01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001\"}";
          const char *csv = "id\n01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001\n";
          const char *xml = "<event><id>01890f3e-5c5a-7cc2-9f2b-8b7f47f0c001</id></event>";
          DataBindValue *from_json = data_bind_parse_json(codec, "Event", json, strlen(json));
          DataBindValue *from_csv = data_bind_parse_csv(codec, "Event", csv, strlen(csv), 0);
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Event", xml, strlen(xml));

          then("all text formats should produce native uuid values") {
            const DataBindValue *id;
            uuid_t actual;
            check_not_null(from_json);
            id = require_field(from_json, "id");
            check(data_bind_value_kind(id) == DATA_BIND_VALUE_UUID);
            check(data_bind_value_as_uuid(id, &actual));
            check_mem_eq(actual.bytes, expected.bytes, sizeof(expected.bytes));

            check_not_null(from_csv);
            id = require_field(from_csv, "id");
            check(data_bind_value_kind(id) == DATA_BIND_VALUE_UUID);
            check(data_bind_value_as_uuid(id, &actual));
            check_mem_eq(actual.bytes, expected.bytes, sizeof(expected.bytes));

            check_not_null(from_xml);
            id = require_field(from_xml, "id");
            check(data_bind_value_kind(id) == DATA_BIND_VALUE_UUID);
            check(data_bind_value_as_uuid(id, &actual));
            check_mem_eq(actual.bytes, expected.bytes, sizeof(expected.bytes));
          }

          data_bind_value_free(from_json);
          data_bind_value_free(from_csv);
          data_bind_value_free(from_xml);
        }

        data_bind_free(codec);
      }

      remove("test_uuid.tbe");
    }
  }

  section("Composite Type Parsing") {
    given("a schema with a composite header") {
      write_schema("test_comp.tbe", "composite Header { uint16 version; uint32 seq; }\n"
                                    "message Msg { Header header; uint32 payload; }\n");

      DataBind *codec = data_bind_create("test_comp.tbe");
      check_not_null(codec);

      if (codec) {
        when("parsing binary data") {
          uint8_t buf[10];
          DataBindValue *v;
          memset(buf, 0, sizeof(buf));
          write_u16_le(buf, 0, 3);
          write_u32_le(buf, 2, 99);
          write_u32_le(buf, 6, 7777);

          v = data_bind_parse(codec, "Msg", buf, sizeof(buf));

          then("flattened composite fields should be exposed") {
            check_not_null(v);
            check_int_eq(data_bind_value_as_int(require_field(v, "header.version")), 3);
            check_int_eq(data_bind_value_as_int(require_field(v, "header.seq")), 99);
            check_int_eq(data_bind_value_as_int(require_field(v, "payload")), 7777);
          }

          data_bind_value_free(v);
        }

        data_bind_free(codec);
      }

      remove("test_comp.tbe");
    }
  }

  section("Enum Type Parsing") {
    given("a schema with an enum field") {
      write_schema("test_enum.tbe", "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
                                    "message Order { uint32 id; Side side; uint32 qty; }\n");

      DataBind *codec = data_bind_create("test_enum.tbe");
      check_not_null(codec);

      if (codec) {
        when("parsing with side=Buy") {
          uint8_t buf[9];
          DataBindValue *v;
          memset(buf, 0, sizeof(buf));
          write_u32_le(buf, 0, 100);
          buf[4] = 1;
          write_u32_le(buf, 5, 500);

          v = data_bind_parse(codec, "Order", buf, sizeof(buf));

          then("enum should bind to numeric value") {
            check_not_null(v);
            check_int_eq(data_bind_value_as_int(require_field(v, "id")), 100);
            check_int_eq(data_bind_value_as_int(require_field(v, "side")), 1);
            check_int_eq(data_bind_value_as_int(require_field(v, "qty")), 500);
          }

          data_bind_value_free(v);
        }

        data_bind_free(codec);
      }

      remove("test_enum.tbe");
    }
  }

  section("Bounds and Errors") {
    given("a schema with uint32 field") {
      write_schema("test_bounds.tbe", "message Small { uint32 x; }\n");

      DataBind *codec = data_bind_create("test_bounds.tbe");
      check_not_null(codec);

      if (codec) {
        when("buffer is too short") {
          uint8_t buf[2] = {0x01, 0x02};
          DataBindValue *v = data_bind_parse(codec, "Small", buf, sizeof(buf));
          then("should return NULL") { check_null(v); }
        }

        when("buffer is exactly right size") {
          uint8_t buf[4];
          DataBindValue *v;
          write_u32_le(buf, 0, 12345);
          v = data_bind_parse(codec, "Small", buf, sizeof(buf));
          then("should succeed") {
            check_not_null(v);
            check_int_eq(data_bind_value_as_int(require_field(v, "x")), 12345);
          }
          data_bind_value_free(v);
        }

        when("parsing an unknown type name") {
          uint8_t buf[4] = {0};
          DataBindValue *v = NULL;
          DataBindError err = DATA_BIND_ERROR_INIT;
          DataBindStatus status =
              test_data_bind_parse_status(codec, "Missing", buf, sizeof(buf), &v, &err);
          then("should return NULL and set error") {
            check_int_eq(status, DATA_BIND_ERR_TYPE_NOT_FOUND);
            check_null(v);
            check(strstr(err.message, "Missing") != NULL);
          }
        }

        data_bind_free(codec);
      }

      remove("test_bounds.tbe");
    }
  }

  section("Memory Management") {
    given("multiple codec instances") {
      write_schema("test_mem.tbe", "message Msg { uint32 x; }\n");

      when("creating and freeing multiple codecs") {
        DataBind *c1 = data_bind_create("test_mem.tbe");
        DataBind *c2 = data_bind_create("test_mem.tbe");
        DataBind *c3 = data_bind_create("test_mem.tbe");

        then("all should be created") {
          check_not_null(c1);
          check_not_null(c2);
          check_not_null(c3);
        }

        data_bind_free(c1);
        data_bind_free(c2);
        data_bind_free(c3);
        data_bind_free(NULL);

        then("should not crash") { check(1); }
      }

      remove("test_mem.tbe");
    }
  }

  section("Dynamic function list") {
    given("a schema with multiple message types") {
      write_schema("test_many_msgs.tbe", "message Msg0 { uint32 x; }\n"
                                         "message Msg1 { uint32 x; }\n"
                                         "message Msg2 { uint32 x; }\n");

      DataBind *codec = data_bind_create("test_many_msgs.tbe");
      check_not_null(codec);

      if (codec) {
        when("parsing all types") {
          uint8_t buf[4] = {42, 0, 0, 0};
          int success_count = 0;
          int i;
          for (i = 0; i < 3; i++) {
            char type[32];
            DataBindValue *v;
            snprintf(type, sizeof(type), "Msg%d", i);
            v = data_bind_parse(codec, type, buf, sizeof(buf));
            if (v) {
              success_count++;
              data_bind_value_free(v);
            }
          }

          then("should parse all 3 types") { check_int_eq(success_count, 3); }
        }

        data_bind_free(codec);
      }

      remove("test_many_msgs.tbe");
    }
  }

  section("Variable-length String Parsing") {
    given("a schema with string field") {
      write_schema("test_varstr.tbe", "message Msg { string name; }\n");

      DataBind *codec = data_bind_create("test_varstr.tbe");
      check_not_null(codec);

      if (codec) {
        when("parsing buffer with varstr") {
          uint8_t buf[9];
          DataBindValue *v;
          write_u32_le(buf, 0, 5);
          memcpy(buf + 4, "Turbo", 5);

          v = data_bind_parse(codec, "Msg", buf, sizeof(buf));

          then("name should be parsed") {
            check_not_null(v);
            check_str_eq(data_bind_value_as_string(require_field(v, "name")), "Turbo");
          }

          data_bind_value_free(v);
        }

        data_bind_free(codec);
      }

      remove("test_varstr.tbe");
    }

    given("a schema with var-data followed by fixed field") {
      write_schema("test_varstr_tail.tbe", "message Msg { string name; uint32 qty; }\n");
      DataBind *codec = data_bind_create("test_varstr_tail.tbe");
      then("codec should support schema binding without requiring binary layout") {
        check_not_null(codec);
        if (codec) {
          const char *json = "{\"name\":\"Turbo\",\"qty\":42}";
          DataBindValue *v = data_bind_parse_json(codec, "Msg", json, strlen(json));
          check_not_null(v);
          if (v) {
            check_str_eq(data_bind_value_as_string(require_field(v, "name")), "Turbo");
            check_int_eq(data_bind_value_as_int(require_field(v, "qty")), 42);
            data_bind_value_free(v);
          }
          data_bind_free(codec);
        }
      }
      remove("test_varstr_tail.tbe");
    }
  }

  section("Array Set Map and Group Parsing") {
    given("a schema with uint32 fixed array") {
      write_schema("test_array.tbe", "message Arr { uint32[3] values; }\n");

      DataBind *codec = data_bind_create("test_array.tbe");
      check_not_null(codec);

      if (codec) {
        uint8_t buf[12];
        DataBindValue *v;
        const DataBindValue *values;
        write_u32_le(buf, 0, 11);
        write_u32_le(buf, 4, 22);
        write_u32_le(buf, 8, 33);
        v = data_bind_parse(codec, "Arr", buf, sizeof(buf));

        then("array should bind to list") {
          check_not_null(v);
          values = require_field(v, "values");
          check(data_bind_value_kind(values) == DATA_BIND_VALUE_LIST);
          check_size_eq(data_bind_value_count(values), 3);
          check_int_eq(data_bind_value_as_int(require_index(values, 0)), 11);
          check_int_eq(data_bind_value_as_int(require_index(values, 1)), 22);
          check_int_eq(data_bind_value_as_int(require_index(values, 2)), 33);
        }

        data_bind_value_free(v);
        data_bind_free(codec);
      }

      remove("test_array.tbe");
    }

    given("a schema with repeating group and trailing var-data") {
      write_schema("test_group.tbe",
                   "group Level { uint64 price; uint32 qty; }\n"
                   "message Book { uint32 seq; group<Level> bids; string symbol; bytes source; }\n");

      DataBind *codec = data_bind_create("test_group.tbe");
      check_not_null(codec);

      if (codec) {
        uint8_t buf[4 + 4 + 24 + 4 + 4 + 4 + 3];
        DataBindValue *v;
        const DataBindValue *bids;
        const DataBindValue *bid0;
        const DataBindValue *bid1;
        const DataBindValue *source;
        const uint8_t *bytes;
        size_t bytes_len = 0;

        memset(buf, 0, sizeof(buf));
        write_u32_le(buf, 0, 7);
        write_u16_le(buf, 4, 12);
        write_u16_le(buf, 6, 2);
        write_u64_le(buf, 8, 100);
        write_u32_le(buf, 16, 10);
        write_u64_le(buf, 20, 200);
        write_u32_le(buf, 28, 20);
        write_u32_le(buf, 32, 4);
        memcpy(buf + 36, "ABCD", 4);
        write_u32_le(buf, 40, 3);
        buf[44] = 1;
        buf[45] = 2;
        buf[46] = 3;

        v = data_bind_parse(codec, "Book", buf, sizeof(buf));

        then("group and trailing var-data should parse") {
          check_not_null(v);
          check_int_eq(data_bind_value_as_int(require_field(v, "seq")), 7);
          bids = require_field(v, "bids");
          check(data_bind_value_kind(bids) == DATA_BIND_VALUE_LIST);
          check_size_eq(data_bind_value_count(bids), 2);
          bid0 = require_index(bids, 0);
          bid1 = require_index(bids, 1);
          check_int_eq(data_bind_value_as_int64(require_field(bid0, "price")), 100);
          check_int_eq(data_bind_value_as_int(require_field(bid0, "qty")), 10);
          check_int_eq(data_bind_value_as_int64(require_field(bid1, "price")), 200);
          check_int_eq(data_bind_value_as_int(require_field(bid1, "qty")), 20);
          check_str_eq(data_bind_value_as_string(require_field(v, "symbol")), "ABCD");
          source = require_field(v, "source");
          bytes = data_bind_value_as_bytes(source, &bytes_len);
          check_size_eq(bytes_len, 3);
          check_not_null(bytes);
          if (bytes) {
            check_int_eq(bytes[0], 1);
            check_int_eq(bytes[1], 2);
            check_int_eq(bytes[2], 3);
          }
        }

        data_bind_value_free(v);
        data_bind_free(codec);
      }

      remove("test_group.tbe");
    }

    given("a schema with set<string> field") {
      write_schema("test_set.tbe", "message Tags { set<string> tags; }\n");

      DataBind *codec = data_bind_create("test_set.tbe");
      check_not_null(codec);

      if (codec) {
        uint8_t buf[14];
        DataBindValue *v;
        const DataBindValue *tags;
        memset(buf, 0, sizeof(buf));
        write_u32_le(buf, 0, 2);
        write_u32_le(buf, 4, 1);
        buf[8] = 'A';
        write_u32_le(buf, 9, 1);
        buf[13] = 'B';
        v = data_bind_parse(codec, "Tags", buf, sizeof(buf));

        then("set should bind to set value") {
          check_not_null(v);
          tags = require_field(v, "tags");
          check(data_bind_value_kind(tags) == DATA_BIND_VALUE_SET);
          check_size_eq(data_bind_value_count(tags), 2);
          check_str_eq(data_bind_value_as_string(require_index(tags, 0)), "A");
          check_str_eq(data_bind_value_as_string(require_index(tags, 1)), "B");
        }

        data_bind_value_free(v);
        data_bind_free(codec);
      }

      remove("test_set.tbe");
    }

    given("a schema with map<string,int32> field") {
      write_schema("test_map.tbe", "message Attrs { map<string,int32> attrs; }\n");

      DataBind *codec = data_bind_create("test_map.tbe");
      check_not_null(codec);

      if (codec) {
        uint8_t buf[22];
        DataBindValue *v;
        const DataBindValue *attrs;
        DataBindMapEntry e0;
        DataBindMapEntry e1;
        memset(buf, 0, sizeof(buf));
        write_u32_le(buf, 0, 2);
        write_u32_le(buf, 4, 1);
        buf[8] = 'x';
        write_i32_le(buf, 9, 30);
        write_u32_le(buf, 13, 1);
        buf[17] = 'y';
        write_i32_le(buf, 18, 40);
        v = data_bind_parse(codec, "Attrs", buf, sizeof(buf));

        then("map should expose key-value entries") {
          check_not_null(v);
          attrs = require_field(v, "attrs");
          check(data_bind_value_kind(attrs) == DATA_BIND_VALUE_MAP);
          check_size_eq(data_bind_value_count(attrs), 2);
          e0 = data_bind_value_map_entry_at(attrs, 0);
          e1 = data_bind_value_map_entry_at(attrs, 1);
          check_str_eq(e0.key, "x");
          check_int_eq(data_bind_value_as_int(e0.value), 30);
          check_str_eq(e1.key, "y");
          check_int_eq(data_bind_value_as_int(e1.value), 40);
        }

        data_bind_value_free(v);
        data_bind_free(codec);
      }

      remove("test_map.tbe");
    }
  }

  section("Extended Types Parsing") {
    given("a schema with bool float double and multi-size enums") {
      write_schema("test_extended.tbe", "enum LargeEnum <uint32> { Big = 0x12345678; }\n"
                                        "message Ext {\n"
                                        "    bool flag;\n"
                                        "    float f_val;\n"
                                        "    double d_val;\n"
                                        "    LargeEnum le;\n"
                                        "}\n");

      DataBind *codec = data_bind_create("test_extended.tbe");
      check_not_null(codec);

      if (codec) {
        uint8_t buf[1 + 4 + 8 + 4];
        DataBindValue *v;
        const DataBindValue *flag;
        int flag_value = 0;
        buf[0] = 1;
        write_f32_le(buf, 1, 3.14f);
        write_f64_le(buf, 5, 2.718281828);
        write_u32_le(buf, 13, 0x12345678);
        v = data_bind_parse(codec, "Ext", buf, sizeof(buf));

        then("extended values should parse") {
          check_not_null(v);
          flag = require_field(v, "flag");
          check(data_bind_value_kind(flag) == DATA_BIND_VALUE_BOOL);
          check_int_eq(data_bind_value_get_bool(flag, &flag_value), DATA_BIND_OK);
          check_int_eq(flag_value, 1);
          check_int_eq(data_bind_value_as_int(flag), 1);
          check(fabs(data_bind_value_as_double(require_field(v, "f_val")) - 3.14) < 1e-4);
          check(fabs(data_bind_value_as_double(require_field(v, "d_val")) - 2.718281828) < 1e-9);
          check_int_eq(data_bind_value_as_int(require_field(v, "le")), 0x12345678);
        }

        data_bind_value_free(v);
        data_bind_free(codec);
      }

      remove("test_extended.tbe");
    }
  }

  section("JSON and CSV Dynamic Binding") {
    given("a schema with records containers enums flags and unions") {
      const char *schema =
          "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
          "flags Perms <uint8> { Read = 1; Write = 2; Execute = 4; }\n"
          "composite Header { uint32 seq; uint64 ts; }\n"
          "union Choice { Side side; Header header; }\n"
          "message Book {\n"
          "  Header header;\n"
          "  Side side;\n"
          "  Perms perms;\n"
          "  Choice choice;\n"
          "  list<uint32> values;\n"
          "  map<string,int32> attrs;\n"
          "}\n";
      write_schema("test_dynamic_bind.tbe", schema);

      DataBind *codec = data_bind_create("test_dynamic_bind.tbe");
      check_not_null(codec);

      if (codec) {
        const char *json =
            "{\"header\":{\"seq\":7,\"ts\":99},\"side\":\"Buy\","
            "\"perms\":[\"Read\",\"Write\"],\"values\":[3,4],"
            "\"attrs\":{\"x\":30,\"y\":40},"
            "\"choice\":{\"side\":\"Sell\"}}";
        DataBindValue *v = data_bind_parse_json(codec, "Book", json, strlen(json));

        then("JSON should bind to a schema-shaped dynamic tree") {
          const DataBindValue *header;
          const DataBindValue *values;
          const DataBindValue *attrs;
          const DataBindValue *choice;
          DataBindMapEntry entry;
          check_not_null(v);
          header = require_field(v, "header");
          check_int_eq(data_bind_value_as_int(require_field(header, "seq")), 7);
          check_int_eq(data_bind_value_as_int64(require_field(header, "ts")), 99);
          check_int_eq(data_bind_value_as_int(require_field(v, "side")), 1);
          check_int_eq(data_bind_value_as_int(require_field(v, "perms")), 3);
          values = require_field(v, "values");
          check(data_bind_value_kind(values) == DATA_BIND_VALUE_LIST);
          check_size_eq(data_bind_value_count(values), 2);
          check_int_eq(data_bind_value_as_int(require_index(values, 1)), 4);
          attrs = require_field(v, "attrs");
          check(data_bind_value_kind(attrs) == DATA_BIND_VALUE_MAP);
          check_size_eq(data_bind_value_count(attrs), 2);
          entry = data_bind_value_map_entry_at(attrs, 0);
          check_str_eq(entry.key, "x");
          check_int_eq(data_bind_value_as_int(entry.value), 30);
          choice = require_field(v, "choice");
          check_int_eq(data_bind_value_as_int(require_field(choice, "side")), 2);
        }

        data_bind_value_free(v);

        {
          const char *json_all =
              "[{\"header\":{\"seq\":1,\"ts\":10},\"side\":\"Buy\",\"perms\":\"Read\","
              "\"values\":[1],\"attrs\":{\"a\":5},"
              "\"choice\":{\"side\":\"Buy\"}},"
              "{\"header\":{\"seq\":2,\"ts\":20},\"side\":\"Sell\",\"perms\":\"Write\","
              "\"values\":[2],\"attrs\":{\"b\":6},"
              "\"choice\":{\"side\":\"Sell\"}}]";
          DataBindValue *all = data_bind_parse_json_all(codec, "Book", json_all, strlen(json_all));
          then("JSON bind_all should bind each array item") {
            check_not_null(all);
            check(data_bind_value_kind(all) == DATA_BIND_VALUE_LIST);
            check_size_eq(data_bind_value_count(all), 2);
            check_int_eq(data_bind_value_as_int(require_field(require_index(all, 1), "side")), 2);
          }
          data_bind_value_free(all);
        }

        {
          const char *csv =
              "header.seq,header.ts,side,perms,values[0],values[1],attrs.x,attrs.y,"
              "choice.side\n"
              "7,99,Buy,Read|Write,3,4,30,40,Sell\n"
              "8,100,Sell,Execute,5,6,50,60,Buy\n";
          DataBindValue *row0 = data_bind_parse_csv(codec, "Book", csv, strlen(csv), 0);
          DataBindValue *rows = data_bind_parse_csv_all(codec, "Book", csv, strlen(csv));

          then("CSV should bind one row and all rows through header paths") {
            const DataBindValue *attrs;
            DataBindMapEntry entry;
            check_not_null(row0);
            check_int_eq(data_bind_value_as_int(require_field(require_field(row0, "header"), "seq")), 7);
            check_int_eq(data_bind_value_as_int(require_field(row0, "perms")), 3);
            check_int_eq(data_bind_value_as_int(require_index(require_field(row0, "values"), 1)), 4);
            attrs = require_field(row0, "attrs");
            check_size_eq(data_bind_value_count(attrs), 2);
            entry = data_bind_value_map_entry_at(attrs, 1);
            check_str_eq(entry.key, "y");
            check_int_eq(data_bind_value_as_int(entry.value), 40);
            check_int_eq(data_bind_value_as_int(require_field(require_field(row0, "choice"), "side")), 2);

            check_not_null(rows);
            check(data_bind_value_kind(rows) == DATA_BIND_VALUE_LIST);
            check_size_eq(data_bind_value_count(rows), 2);
            check_int_eq(data_bind_value_as_int(require_field(require_index(rows, 1), "side")), 2);
          }

          data_bind_value_free(row0);
          data_bind_value_free(rows);
        }

        {
          const char *csv =
              "side,header.seq,header.ts\n"
              "Buy,,\n"
              ",3,4\n";
          DataBindValue *rows = data_bind_parse_csv_all(codec, "Choice", csv, strlen(csv));

          then("CSV union binding should select variants from non-empty row payload") {
            check_not_null(rows);
            check(data_bind_value_kind(rows) == DATA_BIND_VALUE_LIST);
            check_size_eq(data_bind_value_count(rows), 2);
            check_int_eq(data_bind_value_as_int(require_field(require_index(rows, 0), "side")), 1);
            check_int_eq(data_bind_value_as_int(require_field(require_field(require_index(rows, 1), "header"), "seq")), 3);
            check_int_eq(data_bind_value_as_int64(require_field(require_field(require_index(rows, 1), "header"), "ts")), 4);
          }

          data_bind_value_free(rows);
        }

        {
          const char *xml =
              "<books>"
              "<book seq=\"7\"><header><seq>7</seq><ts>99</ts></header><side>Buy</side>"
              "<perms>Read|Write</perms><choice><side>Sell</side></choice>"
              "<values>3</values><values>4</values><attrs><x>30</x><y>40</y></attrs></book>"
              "<book seq=\"8\"><header><seq>8</seq><ts>100</ts></header><side>Sell</side>"
              "<perms>Execute</perms><choice><side>Buy</side></choice>"
              "<values>5</values><values>6</values><attrs><x>50</x><y>60</y></attrs></book>"
              "</books>";
          const char *one_xml =
              "<book seq=\"7\"><header><seq>7</seq><ts>99</ts></header><side>Buy</side>"
              "<perms>Read|Write</perms><choice><side>Sell</side></choice>"
              "<values>3</values><values>4</values><attrs><x>30</x><y>40</y></attrs></book>";
          DataBindValue *one = data_bind_parse_xml(codec, "Book", one_xml, strlen(one_xml));
          DataBindValue *all = data_bind_parse_xml_all(codec, "Book", xml, strlen(xml), "//book");

          then("XML should bind one document and XPath-selected nodes") {
            const DataBindValue *attrs;
            DataBindMapEntry entry;
            check_not_null(one);
            check_int_eq(data_bind_value_as_int(require_field(require_field(one, "header"), "seq")), 7);
            check_int_eq(data_bind_value_as_int(require_field(one, "side")), 1);
            check_int_eq(data_bind_value_as_int(require_field(one, "perms")), 3);
            check_int_eq(data_bind_value_as_int(require_index(require_field(one, "values"), 1)), 4);
            attrs = require_field(one, "attrs");
            check_size_eq(data_bind_value_count(attrs), 2);
            entry = data_bind_value_map_entry_at(attrs, 1);
            check_str_eq(entry.key, "y");
            check_int_eq(data_bind_value_as_int(entry.value), 40);
            check_int_eq(data_bind_value_as_int(require_field(require_field(one, "choice"), "side")), 2);

            check_not_null(all);
            check(data_bind_value_kind(all) == DATA_BIND_VALUE_LIST);
            check_size_eq(data_bind_value_count(all), 2);
            check_int_eq(data_bind_value_as_int(require_field(require_index(all, 1), "side")), 2);
          }

          data_bind_value_free(one);
          data_bind_value_free(all);
        }

        data_bind_free(codec);
      }

      remove("test_dynamic_bind.tbe");
    }

    given("a schema with group fields before var-data") {
      write_schema("test_dynamic_group_bind.tbe",
                   "group Level { uint64 price; uint32 qty; }\n"
                   "message Book {\n"
                   "  uint32 seq;\n"
                   "  group<Level> bids;\n"
                   "  string symbol;\n"
                   "}\n");

      DataBind *codec = data_bind_create("test_dynamic_group_bind.tbe");
      check_not_null(codec);

      if (codec) {
        const char *json =
            "{\"seq\":7,\"bids\":[{\"price\":100,\"qty\":10},{\"price\":200,\"qty\":20}],"
            "\"symbol\":\"ABCD\"}";
        const char *csv =
            "seq,bids[0].price,bids[0].qty,bids[1].price,bids[1].qty,symbol\n"
            "7,100,10,200,20,ABCD\n";
        DataBindValue *from_json = data_bind_parse_json(codec, "Book", json, strlen(json));
        DataBindValue *from_csv = data_bind_parse_csv(codec, "Book", csv, strlen(csv), 0);
        {
          const char *xml =
              "<book><seq>7</seq><bids><price>100</price><qty>10</qty></bids>"
              "<bids><price>200</price><qty>20</qty></bids><symbol>ABCD</symbol></book>";
          DataBindValue *from_xml = data_bind_parse_xml(codec, "Book", xml, strlen(xml));

          then("JSON CSV and XML should bind group rows") {
            const DataBindValue *json_bids;
            const DataBindValue *csv_bids;
            const DataBindValue *xml_bids;
            check_not_null(from_json);
            json_bids = require_field(from_json, "bids");
            check_size_eq(data_bind_value_count(json_bids), 2);
            check_int_eq(data_bind_value_as_int64(require_field(require_index(json_bids, 1), "price")), 200);
            check_str_eq(data_bind_value_as_string(require_field(from_json, "symbol")), "ABCD");

            check_not_null(from_csv);
            csv_bids = require_field(from_csv, "bids");
            check_size_eq(data_bind_value_count(csv_bids), 2);
            check_int_eq(data_bind_value_as_int(require_field(require_index(csv_bids, 0), "qty")), 10);
            check_str_eq(data_bind_value_as_string(require_field(from_csv, "symbol")), "ABCD");

            check_not_null(from_xml);
            xml_bids = require_field(from_xml, "bids");
            check_size_eq(data_bind_value_count(xml_bids), 2);
            check_int_eq(data_bind_value_as_int(require_field(require_index(xml_bids, 1), "qty")), 20);
            check_str_eq(data_bind_value_as_string(require_field(from_xml, "symbol")), "ABCD");
          }

          data_bind_value_free(from_xml);
        }

        data_bind_value_free(from_json);
        data_bind_value_free(from_csv);
        data_bind_free(codec);
      }

      remove("test_dynamic_group_bind.tbe");
    }
  }
}
