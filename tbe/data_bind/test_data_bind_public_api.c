/**
 * @file test_data_bind_public_api.c
 * @brief Third-party style smoke test for the public data_bind C API.
 */

#include "data_bind.h"
#include "tinytest.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void write_schema_file(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  check_not_null(f);
  if (!f) return;
  fwrite(content, 1, strlen(content), f);
  fclose(f);
}

spec("data_bind public API") {
  it("should expose version and ABI metadata") {
    check_int_eq(data_bind_library_version(), DATA_BIND_VERSION);
    check_int_eq(data_bind_abi_version(), DATA_BIND_ABI_VERSION);
    check_str_eq(data_bind_version_string(), "1.6.0");
    check_str_eq(data_bind_status_name(DATA_BIND_ERR_TYPE_MISMATCH), "type_mismatch");
  }

  it("should bind JSON and XML through public opaque handles") {
    write_schema_file("test_public_api.tbe",
                      "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
                      "message Order { uint32 id; Side side; bool active; string symbol; }\n");

    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status = data_bind_create("test_public_api.tbe", &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      const char *json = "{\"id\":7,\"side\":\"Buy\",\"active\":true,\"symbol\":\"ABCD\"}";
      const char *xml = "<order id=\"8\" active=\"false\"><side>Sell</side><symbol>WXYZ</symbol></order>";
      DataBindValue *from_json = NULL;
      DataBindValue *from_xml = NULL;
      int32_t id = 0;
      int active = 0;
      const char *symbol = NULL;
      size_t symbol_len = 0;
      status = data_bind_parse_json(codec, "Order", json, strlen(json), &from_json, &err);
      check_int_eq(status, DATA_BIND_OK);
      status = data_bind_parse_xml(codec, "Order", xml, strlen(xml), &from_xml, &err);
      check_int_eq(status, DATA_BIND_OK);

      check_not_null(from_json);
      check_not_null(from_xml);
      check_int_eq(data_bind_value_get_int32(data_bind_value_get(from_json, "id"), &id), DATA_BIND_OK);
      check_int_eq(id, 7);
      check_int_eq(data_bind_value_as_int(data_bind_value_get(from_json, "side")), 1);
      check(data_bind_value_kind(data_bind_value_get(from_json, "active")) == DATA_BIND_VALUE_BOOL);
      check_int_eq(data_bind_value_get_bool(data_bind_value_get(from_json, "active"), &active), DATA_BIND_OK);
      check_int_eq(active, 1);
      check_int_eq(data_bind_value_get_string(data_bind_value_get(from_json, "symbol"),
                                             &symbol, &symbol_len), DATA_BIND_OK);
      check_size_eq(symbol_len, 4);
      check_str_eq(symbol, "ABCD");
      check_int_eq(data_bind_value_as_int(data_bind_value_get(from_xml, "id")), 8);
      check_int_eq(data_bind_value_as_int(data_bind_value_get(from_xml, "side")), 2);
      check(data_bind_value_kind(data_bind_value_get(from_xml, "active")) == DATA_BIND_VALUE_BOOL);
      check_int_eq(data_bind_value_as_bool(data_bind_value_get(from_xml, "active")), 0);
      check_str_eq(data_bind_value_as_string(data_bind_value_get(from_xml, "symbol")), "WXYZ");

      data_bind_value_free(from_json);
      data_bind_value_free(from_xml);
      data_bind_free(codec);
    }

    remove("test_public_api.tbe");
  }

  it("should create codecs from schema text and report structured errors") {
    const char *schema =
        "message Order { uint32 id; string symbol; }\n";
    const char *json = "{\"id\":9,\"symbol\":\"TEXT\"}";
    DataBind *codec = NULL;
    DataBindValue *order = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status = data_bind_create_from_text(schema, strlen(schema), &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      status = data_bind_parse_json(codec, "Order", json, strlen(json), &order, &err);
      check_int_eq(status, DATA_BIND_OK);
      check_not_null(order);
      check_int_eq(data_bind_value_as_int(data_bind_value_get(order, "id")), 9);
      data_bind_value_free(order);
      order = NULL;

      status = data_bind_parse_json(codec, "Missing", json, strlen(json), &order, &err);
      check_int_eq(status, DATA_BIND_ERR_TYPE_NOT_FOUND);
      check_null(order);
      check(err.message[0] != '\0');

      data_bind_free(codec);
    }
  }

  it("should strictly validate JSON CSV and XML documents") {
    const char *schema =
        "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
        "message Order { uint32 id; Side side; string symbol; }\n"
        "union Choice { Side side; Order order; }\n";
    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status = data_bind_create_from_text(schema, strlen(schema), &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      const char *json_good =
          "[{\"id\":1,\"side\":\"Buy\",\"symbol\":\"ABCD\"},"
          "{\"id\":2,\"side\":\"Sell\",\"symbol\":\"WXYZ\"}]";
      const char *json_bad =
          "[{\"id\":1,\"side\":\"Buy\",\"symbol\":\"ABCD\"},"
          "{\"id\":\"bad\",\"side\":\"Sell\",\"symbol\":\"WXYZ\"}]";
      const char *csv_good =
          "id,side,symbol\n"
          "1,Buy,ABCD\n"
          "2,Sell,WXYZ\n";
      const char *csv_bad =
          "id,side,symbol\n"
          "1,Buy,ABCD\n"
          "bad,Sell,WXYZ\n";
      const char *csv_union =
          "side,order.id,order.side,order.symbol\n"
          "Buy,,,\n"
          ",2,Sell,WXYZ\n";
      const char *xml_good =
          "<orders><order><id>1</id><side>Buy</side><symbol>ABCD</symbol></order>"
          "<order><id>2</id><side>Sell</side><symbol>WXYZ</symbol></order></orders>";
      const char *xml_bad =
          "<orders><order><id>1</id><side>Buy</side><symbol>ABCD</symbol></order>"
          "<order><id>bad</id><side>Sell</side><symbol>WXYZ</symbol></order></orders>";

      status = data_bind_validate_json(codec, "Order", json_good, strlen(json_good), &err);
      check_int_eq(status, DATA_BIND_OK);
      status = data_bind_validate_json(codec, "Order", json_bad, strlen(json_bad), &err);
      check_int_eq(status, DATA_BIND_ERR_TYPE_MISMATCH);

      status = data_bind_validate_csv(codec, "Order", csv_good, strlen(csv_good), &err);
      check_int_eq(status, DATA_BIND_OK);
      status = data_bind_validate_csv(codec, "Order", csv_bad, strlen(csv_bad), &err);
      check_int_eq(status, DATA_BIND_ERR_TYPE_MISMATCH);
      status = data_bind_validate_csv(codec, "Choice", csv_union, strlen(csv_union), &err);
      check_int_eq(status, DATA_BIND_OK);

      status = data_bind_validate_xml(codec, "Order", xml_good, strlen(xml_good), "//order", &err);
      check_int_eq(status, DATA_BIND_OK);
      status = data_bind_validate_xml(codec, "Order", xml_bad, strlen(xml_bad), "//order", &err);
      check_int_eq(status, DATA_BIND_ERR_TYPE_MISMATCH);

      data_bind_free(codec);
    }
  }

  it("should bind temporal scalars without the script parser module") {
    const char *schema = "message Event { datetime at; date d; time t; duration span; }\n";
    const char *json =
        "{\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\",\"d\":\"2026-06-28\","
        "\"t\":\"09:30:05.123\",\"span\":\"1h30m5s250ms\"}";
    DataBind *codec = NULL;
    DataBindValue *event = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status = data_bind_create_from_text(schema, strlen(schema), &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      turbo_datetime_t dt;
      DataBindDate date;
      DataBindTime time;
      int64_t span_ms = 0;
      char text[64];

      status = data_bind_parse_json(codec, "Event", json, strlen(json), &event, &err);
      check_int_eq(status, DATA_BIND_OK);
      check_not_null(event);
      check_int_eq(data_bind_value_get_datetime(data_bind_value_get(event, "at"), &dt),
                   DATA_BIND_OK);
      check_int_eq(dt.year, 2006);
      check_int_eq(data_bind_value_get_date(data_bind_value_get(event, "d"), &date),
                   DATA_BIND_OK);
      check_int_eq(date.year, 2026);
      check_int_eq(data_bind_value_get_time(data_bind_value_get(event, "t"), &time),
                   DATA_BIND_OK);
      check_int_eq(time.millisecond, 123);
      check_int_eq(data_bind_value_get_duration_milliseconds(data_bind_value_get(event, "span"),
                                                             &span_ms),
                   DATA_BIND_OK);
      check_int_eq((int)span_ms, 5405250);
      check_str_eq(data_bind_value_as_duration_string(data_bind_value_get(event, "span"),
                                                      text, sizeof(text)),
                   "1:30:05.250");
      data_bind_value_free(event);
      data_bind_free(codec);
    }
  }

  it("should bind decimal scalars through JSON CSV and XML") {
    const char *schema = "message Quote { decimal price; }\n";
    const char *json = "{\"price\":\"123.4500\"}";
    const char *csv = "price\n-0.1250\n";
    const char *xml = "<quote><price>42.00</price></quote>";
    DataBind *codec = NULL;
    DataBindValue *quote = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindDecimal decimal;
    char text[64];
    DataBindStatus status = data_bind_create_from_text(schema, strlen(schema), &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      status = data_bind_parse_json(codec, "Quote", json, strlen(json), &quote, &err);
      check_int_eq(status, DATA_BIND_OK);
      check_not_null(quote);
      check(data_bind_value_kind(data_bind_value_get(quote, "price")) ==
            DATA_BIND_VALUE_DECIMAL);
      check_int_eq(data_bind_value_get_decimal(data_bind_value_get(quote, "price"), &decimal),
                   DATA_BIND_OK);
      check_int_eq((int)decimal.mantissa, 12345);
      check_int_eq(decimal.scale, 2);
      check_str_eq(data_bind_value_as_decimal_string(data_bind_value_get(quote, "price"),
                                                     text, sizeof(text)),
                   "123.45");
      data_bind_value_free(quote);
      quote = NULL;

      status = data_bind_parse_csv(codec, "Quote", csv, strlen(csv), 0, &quote, &err);
      check_int_eq(status, DATA_BIND_OK);
      check_int_eq(data_bind_value_get_decimal(data_bind_value_get(quote, "price"), &decimal),
                   DATA_BIND_OK);
      check_int_eq((int)decimal.mantissa, -125);
      check_int_eq(decimal.scale, 3);
      data_bind_value_free(quote);
      quote = NULL;

      status = data_bind_parse_xml(codec, "Quote", xml, strlen(xml), &quote, &err);
      check_int_eq(status, DATA_BIND_OK);
      check_str_eq(data_bind_value_as_decimal_string(data_bind_value_get(quote, "price"),
                                                     text, sizeof(text)),
                   "42");
      data_bind_value_free(quote);
      quote = NULL;

      {
        const char *bad_json = "{\"price\":\"bad\"}";
        status = data_bind_validate_json(codec, "Quote", bad_json, strlen(bad_json), &err);
      }
      check_int_eq(status, DATA_BIND_ERR_TYPE_MISMATCH);
      data_bind_free(codec);
    }
  }

  it("should bind bigint and money scalars through the public API") {
    const char *schema = "message Invoice { bigint id; money total; }\n";
    const char *json =
        "{\"id\":\"000123456789012345678901234567890\","
        "\"total\":{\"amount\":\"123.4500\",\"currency\":\"USD\"}}";
    DataBind *codec = NULL;
    DataBindValue *invoice = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    const char *bigint = NULL;
    size_t bigint_len = 0;
    DataBindMoney money;
    char text[64];
    DataBindStatus status = data_bind_create_from_text(schema, strlen(schema), &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      status = data_bind_parse_json(codec, "Invoice", json, strlen(json), &invoice, &err);
      check_int_eq(status, DATA_BIND_OK);
      check_not_null(invoice);
      check(data_bind_value_kind(data_bind_value_get(invoice, "id")) == DATA_BIND_VALUE_BIGINT);
      check_int_eq(data_bind_value_get_bigint(data_bind_value_get(invoice, "id"),
                                             &bigint, &bigint_len),
                   DATA_BIND_OK);
      check_str_eq(bigint, "123456789012345678901234567890");
      check_size_eq(bigint_len, strlen("123456789012345678901234567890"));
      check(data_bind_value_kind(data_bind_value_get(invoice, "total")) == DATA_BIND_VALUE_MONEY);
      check_int_eq(data_bind_value_get_money(data_bind_value_get(invoice, "total"), &money),
                   DATA_BIND_OK);
      check_str_eq(money.currency, "USD");
      check_int_eq((int)money.amount.mantissa, 12345);
      check_int_eq(money.amount.scale, 2);
      check_str_eq(data_bind_value_as_money_string(data_bind_value_get(invoice, "total"),
                                                   text, sizeof(text)),
                   "USD 123.45");

      data_bind_value_free(invoice);
      data_bind_free(codec);
    }
  }

  it("should honor reflection struct size guards") {
    const char *schema =
        "schema Market [id(1)];\n"
        "enum Side <uint8> { Buy = 1; }\n"
        "message Order { uint32 id; Side side; }\n";
    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status = data_bind_create_from_text(schema, strlen(schema), &codec, &err);
    check_int_eq(status, DATA_BIND_OK);
    check_not_null(codec);

    if (codec) {
      struct {
        DataBindSchemaType type;
        unsigned char guard[8];
      } type_buf;
      struct {
        DataBindSchemaField field;
        unsigned char guard[8];
      } field_buf;
      struct {
        DataBindSchemaEnumItem item;
        unsigned char guard[8];
      } item_buf;
      struct {
        DataBindSchemaAttribute attr;
        unsigned char guard[8];
      } attr_buf;
      const unsigned char guard[8] = {0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};

      memset(&type_buf, 0, sizeof(type_buf));
      memcpy(type_buf.guard, guard, sizeof(guard));
      type_buf.type.size = offsetof(DataBindSchemaType, fixed_block_size);
      check(data_bind_schema_find_type(codec, "Order", &type_buf.type) == 1);
      check_size_eq(type_buf.type.size, offsetof(DataBindSchemaType, fixed_block_size));
      check_str_eq(type_buf.type.name, "Order");
      check_mem_eq(type_buf.guard, guard, sizeof(guard));

      memset(&field_buf, 0, sizeof(field_buf));
      memcpy(field_buf.guard, guard, sizeof(guard));
      field_buf.field.size = offsetof(DataBindSchemaField, offset);
      check(data_bind_schema_field_at(codec, "Order", 1, &field_buf.field) == 1);
      check_size_eq(field_buf.field.size, offsetof(DataBindSchemaField, offset));
      check_str_eq(field_buf.field.name, "side");
      check_str_eq(field_buf.field.kind, "enum");
      check_mem_eq(field_buf.guard, guard, sizeof(guard));

      memset(&item_buf, 0, sizeof(item_buf));
      memcpy(item_buf.guard, guard, sizeof(guard));
      item_buf.item.size = offsetof(DataBindSchemaEnumItem, value);
      check(data_bind_schema_enum_item_at(codec, "Side", 0, &item_buf.item) == 1);
      check_size_eq(item_buf.item.size, offsetof(DataBindSchemaEnumItem, value));
      check_str_eq(item_buf.item.name, "Buy");
      check_mem_eq(item_buf.guard, guard, sizeof(guard));

      memset(&attr_buf, 0, sizeof(attr_buf));
      memcpy(attr_buf.guard, guard, sizeof(guard));
      attr_buf.attr.size = offsetof(DataBindSchemaAttribute, value);
      check(data_bind_schema_attribute_at(codec, 0, &attr_buf.attr) == 1);
      check_size_eq(attr_buf.attr.size, offsetof(DataBindSchemaAttribute, value));
      check_str_eq(attr_buf.attr.name, "id");
      check_mem_eq(attr_buf.guard, guard, sizeof(guard));

      type_buf.type.size = offsetof(DataBindSchemaType, fixed_block_size);
      memcpy(type_buf.guard, guard, sizeof(guard));
      check(data_bind_schema_find_type(codec, "Missing", &type_buf.type) == 0);
      check_size_eq(type_buf.type.size, offsetof(DataBindSchemaType, fixed_block_size));
      check_mem_eq(type_buf.guard, guard, sizeof(guard));

      data_bind_free(codec);
    }
  }
}
