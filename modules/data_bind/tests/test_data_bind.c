#include "exprtk.h"
#include "tinytest.h"
#include "ts_plugin_loader.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #define DATA_BIND_PLUGIN_DLL "tbs_data_bind.dll"
#else
  #define DATA_BIND_PLUGIN_DLL "tbs_data_bind.so"
#endif

enum { DATA_BIND_TEST_MAX_HANDLES = 32 };

typedef struct {
  ts_plugin_handle_t *db_plugin;
  exprtk_env_t env;
  mem_pool_t scratch;
} test_env_t;

static void test_env_init(test_env_t *t) {
  memset(t, 0, sizeof(*t));
  exprtk_env_init(&t->env);
  mem_init(&t->scratch, 65536);
  t->db_plugin = ts_plugin_load(DATA_BIND_PLUGIN_DLL);
  if (t->db_plugin)
    ts_plugin_init(t->db_plugin, &t->env, &t->scratch);
}

static void test_env_free(test_env_t *t) {
  ts_plugin_unload(t->db_plugin);
  exprtk_env_free(&t->env);
  mem_destroy(&t->scratch);
}

static exprtk_value_t call_fn(test_env_t *t, const char *name, size_t argc, exprtk_value_t *args) {
  exprtk_func_t *fn = t->env.funcs;
  while (fn) {
    if (strcmp(fn->name, name) == 0 && !fn->is_script) {
      return fn->data.native.fn(argc, args, fn->data.native.user_data);
    }
    fn = fn->next;
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = -999.0};
}

static exprtk_value_t make_str(test_env_t *t, const char *s) {
  size_t len = strlen(s);
  char *buf = mem_alloc(&t->env.arena, len + 1);
  memcpy(buf, s, len + 1);
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static exprtk_value_t make_bytes(test_env_t *t, const uint8_t *data, size_t len) {
  char *buf = mem_alloc(&t->env.arena, len);
  if (len > 0) memcpy(buf, data, len);
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
}

static void write_schema(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
  if (f) {
    fwrite(content, 1, strlen(content), f);
    fclose(f);
  }
}

static void write_u32_le(uint8_t *buf, size_t offset, uint32_t value) {
  buf[offset] = value & 0xFF;
  buf[offset + 1] = (value >> 8) & 0xFF;
  buf[offset + 2] = (value >> 16) & 0xFF;
  buf[offset + 3] = (value >> 24) & 0xFF;
}

spec("data_bind_module") {
  describe("plugin") {
    it("should load and unload data_bind_plugin via DLL") {
      ts_plugin_handle_t *h = ts_plugin_load(DATA_BIND_PLUGIN_DLL);
      check_not_null(h);
      check_not_null(h->plugin);
      check_str_eq(h->plugin->name, "data_bind");

      exprtk_env_t env;
      mem_pool_t scratch;
      exprtk_env_init(&env);
      mem_init(&scratch, 65536);
      check_int_eq(ts_plugin_init(h, &env, &scratch), 0);

      ts_plugin_unload(h);
      exprtk_env_free(&env);
      mem_destroy(&scratch);
    }
  }

  describe("data_bind.create / data_bind.close") {
    it("should create and close a codec handle") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.db_plugin);

      write_schema("test_create_plugin.tbe", "message Ping { uint32 seq; }\n");

      exprtk_value_t args[1] = {make_str(&t, "test_create_plugin.tbe")};
      exprtk_value_t handle = call_fn(&t, "data_bind.create", 1, args);
      check_int_eq(handle.type, EXPRTK_VAL_NUMBER);
      check(handle.data.number >= 0.0);

      exprtk_value_t close_args[1] = {handle};
      exprtk_value_t res = call_fn(&t, "data_bind.close", 1, close_args);
      check_int_eq(res.type, EXPRTK_VAL_NUMBER);
      check_float_eq(res.data.number, 0.0, 0.01);

      remove("test_create_plugin.tbe");
      test_env_free(&t);
    }

    it("should create a codec from schema text and validate structured data") {
      test_env_t t;
      exprtk_value_t schema_args[1];
      exprtk_value_t handle;
      exprtk_value_t json_args[3];
      exprtk_value_t csv_args[3];
      exprtk_value_t xml_args[3];

      test_env_init(&t);
      check_not_null(t.db_plugin);

      schema_args[0] = make_str(&t, "message Trade { double price; uint32 qty; string symbol; }");
      handle = call_fn(&t, "data_bind.create_from_text", 1, schema_args);
      check_int_eq(handle.type, EXPRTK_VAL_NUMBER);
      check(handle.data.number >= 0.0);

      json_args[0] = handle;
      json_args[1] = make_str(&t, "Trade");
      json_args[2] = make_str(&t, "{\"price\":10.5,\"qty\":2,\"symbol\":\"AAPL\"}");
      check_true(call_fn(&t, "data_bind.validate_json", 3, json_args).data.boolean);
      json_args[2] = make_str(&t, "{\"price\":\"bad\",\"qty\":2,\"symbol\":\"AAPL\"}");
      check_false(call_fn(&t, "data_bind.validate_json", 3, json_args).data.boolean);

      csv_args[0] = handle;
      csv_args[1] = make_str(&t, "Trade");
      csv_args[2] = make_str(&t, "symbol,price,qty\nAAPL,10.5,2\n");
      check_true(call_fn(&t, "data_bind.validate_csv", 3, csv_args).data.boolean);
      csv_args[2] = make_str(&t, "symbol,price,qty\nAAPL,bad,2\n");
      check_false(call_fn(&t, "data_bind.validate_csv", 3, csv_args).data.boolean);

      xml_args[0] = handle;
      xml_args[1] = make_str(&t, "Trade");
      xml_args[2] = make_str(&t, "<trade><price>10.5</price><qty>2</qty><symbol>AAPL</symbol></trade>");
      check_true(call_fn(&t, "data_bind.validate_xml", 3, xml_args).data.boolean);
      xml_args[2] = make_str(&t, "<trade><price>bad</price><qty>2</qty><symbol>AAPL</symbol></trade>");
      check_false(call_fn(&t, "data_bind.validate_xml", 3, xml_args).data.boolean);

      call_fn(&t, "data_bind.close", 1, &handle);
      test_env_free(&t);
    }
  }

  describe("data_bind.parse") {
    it("should parse message using JIT compiled codec") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.db_plugin);

      write_schema("test_parse_plugin.tbe", 
        "message Attrs {\n"
        "  map<string,int32> attrs;\n"
        "}\n"
      );

      exprtk_value_t create_args[1] = {make_str(&t, "test_parse_plugin.tbe")};
      exprtk_value_t handle = call_fn(&t, "data_bind.create", 1, create_args);
      check(handle.data.number >= 0.0);

      /* Binary payload with 2 entries for map */
      uint8_t buf[22];
      memset(buf, 0, sizeof(buf));
      write_u32_le(buf, 0, 2);      /* map entries count = 2 */
      write_u32_le(buf, 4, 1);      /* key length = 1 */
      buf[8] = 'x';
      write_u32_le(buf, 9, 30);     /* value = 30 (little endian) */
      write_u32_le(buf, 13, 1);     /* key length = 1 */
      buf[17] = 'y';
      write_u32_le(buf, 18, 40);    /* value = 40 (little endian) */

      exprtk_value_t parse_args[3] = {
        handle,
        make_str(&t, "Attrs"),
        make_bytes(&t, buf, sizeof(buf))
      };

      exprtk_value_t res_map = call_fn(&t, "data_bind.parse", 3, parse_args);
      check_int_eq(res_map.type, EXPRTK_VAL_OBJECT);

      /* Check map values using map helper functions */
      check(exprtk_map_has(&res_map, "attrs"));
      exprtk_value_t attrs_map = exprtk_map_get(&res_map, "attrs");
      check_int_eq(attrs_map.type, EXPRTK_VAL_MAP);

      check(exprtk_map_has(&attrs_map, "x"));
      exprtk_value_t x_val = exprtk_map_get(&attrs_map, "x");
      check_int_eq(x_val.type, EXPRTK_VAL_NUMBER);
      check_float_eq(x_val.data.number, 30.0, 0.01);

      check(exprtk_map_has(&attrs_map, "y"));
      exprtk_value_t y_val = exprtk_map_get(&attrs_map, "y");
      check_int_eq(y_val.type, EXPRTK_VAL_NUMBER);
      check_float_eq(y_val.data.number, 40.0, 0.01);

      exprtk_value_t close_args[1] = {handle};
      call_fn(&t, "data_bind.close", 1, close_args);

      remove("test_parse_plugin.tbe");
      test_env_free(&t);
    }
  }

  describe("data_bind.json / data_bind.csv / data_bind.xml and path variants") {
    it("should bind JSON CSV XML through the plugin API (inline and path)") {
      test_env_t t;
      test_env_init(&t);
      check_not_null(t.db_plugin);

      write_schema("test_dynamic_plugin.tbe",
        "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
        "message Order {\n"
        "  uint32 id;\n"
        "  Side side;\n"
        "  datetime at;\n"
        "  bool active;\n"
        "  list<uint32> qtys;\n"
        "  map<string,int32> attrs;\n"
        "}\n"
      );

      write_schema("test_order.json", "{\"id\":7,\"side\":\"Buy\",\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\",\"active\":true,\"qtys\":[10,20],\"attrs\":{\"x\":30}}");
      write_schema("test_orders.json", "[{\"id\":7,\"side\":\"Buy\",\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\",\"active\":true,\"qtys\":[10,20],\"attrs\":{\"x\":30}},"
                                  "{\"id\":8,\"side\":\"Sell\",\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\",\"active\":false,\"qtys\":[11,22],\"attrs\":{\"x\":44}}]");
      write_schema("test_orders.csv", "id,side,at,active,qtys[0],qtys[1],attrs.x\n8,Sell,\"Sat, 04 Mar 2006 13:27:54 GMT\",false,11,22,44\n");
      write_schema("test_orders_all.csv", "id,side,at,active,qtys[0],attrs.x\n1,Buy,\"Sat, 04 Mar 2006 13:27:54 GMT\",true,10,30\n2,Sell,\"Sat, 04 Mar 2006 13:27:54 GMT\",false,20,40\n");
      write_schema("test_order.xml", "<order id=\"9\" active=\"true\"><side>Buy</side><at>Sat, 04 Mar 2006 13:27:54 GMT</at><qtys>12</qtys><qtys>24</qtys><attrs><x>48</x></attrs></order>");
      write_schema("test_orders.xml", "<orders><order><id>1</id><side>Buy</side><at>Sat, 04 Mar 2006 13:27:54 GMT</at><active>true</active><qtys>10</qtys><attrs><x>30</x></attrs></order>"
                                 "<order><id>2</id><side>Sell</side><at>Sat, 04 Mar 2006 13:27:54 GMT</at><active>false</active><qtys>20</qtys><attrs><x>40</x></attrs></order></orders>");

      exprtk_value_t create_args[1] = {make_str(&t, "test_dynamic_plugin.tbe")};
      exprtk_value_t handle = call_fn(&t, "data_bind.create", 1, create_args);
      check(handle.data.number >= 0.0);

      {
        exprtk_value_t json_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "{\"id\":7,\"side\":\"Buy\",\"at\":\"Sat, 04 Mar 2006 13:27:54 GMT\",\"active\":true,\"qtys\":[10,20],\"attrs\":{\"x\":30}}")
        };
        exprtk_value_t order = call_fn(&t, "data_bind.json", 3, json_args);
        exprtk_value_t qtys;
        exprtk_value_t attrs;
        exprtk_value_t at;
        check_int_eq(order.type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&order, "id").data.number, 7.0, 0.01);
        check_float_eq(exprtk_map_get(&order, "side").data.number, 1.0, 0.01);
        at = exprtk_map_get(&order, "at");
        check_int_eq(at.type, EXPRTK_VAL_DATETIME);
        check_int_eq(at.data.datetime.year, 2006);
        check_int_eq(exprtk_map_get(&order, "active").type, EXPRTK_VAL_BOOL);
        check_int_eq(exprtk_map_get(&order, "active").data.boolean, 1);
        qtys = exprtk_map_get(&order, "qtys");
        check_int_eq(qtys.type, EXPRTK_VAL_LIST);
        check_size_eq(qtys.data.list.count, 2);
        check_float_eq(qtys.data.list.items[1].data.number, 20.0, 0.01);
        attrs = exprtk_map_get(&order, "attrs");
        check_int_eq(attrs.type, EXPRTK_VAL_MAP);
        check_float_eq(exprtk_map_get(&attrs, "x").data.number, 30.0, 0.01);
      }

      {
        exprtk_value_t csv_args[4] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "id,side,at,active,qtys[0],qtys[1],attrs.x\n8,Sell,\"Sat, 04 Mar 2006 13:27:54 GMT\",false,11,22,44\n"),
          exprtk_val_num(0.0)
        };
        exprtk_value_t order = call_fn(&t, "data_bind.csv", 4, csv_args);
        exprtk_value_t qtys;
        exprtk_value_t attrs;
        check_int_eq(order.type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&order, "id").data.number, 8.0, 0.01);
        check_float_eq(exprtk_map_get(&order, "side").data.number, 2.0, 0.01);
        check_int_eq(exprtk_map_get(&order, "at").type, EXPRTK_VAL_DATETIME);
        check_int_eq(exprtk_map_get(&order, "active").type, EXPRTK_VAL_BOOL);
        check_int_eq(exprtk_map_get(&order, "active").data.boolean, 0);
        qtys = exprtk_map_get(&order, "qtys");
        check_int_eq(qtys.type, EXPRTK_VAL_LIST);
        check_float_eq(qtys.data.list.items[0].data.number, 11.0, 0.01);
        attrs = exprtk_map_get(&order, "attrs");
        check_int_eq(attrs.type, EXPRTK_VAL_MAP);
        check_float_eq(exprtk_map_get(&attrs, "x").data.number, 44.0, 0.01);
      }

      {
        exprtk_value_t csv_all_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "id,side,at,active,qtys[0],attrs.x\n"
                       "1,Buy,\"Sat, 04 Mar 2006 13:27:54 GMT\",true,10,30\n"
                       "2,Sell,\"Sat, 04 Mar 2006 13:27:54 GMT\",false,20,40\n")
        };
        exprtk_value_t rows = call_fn(&t, "data_bind.csv_all", 3, csv_all_args);
        check_int_eq(rows.type, EXPRTK_VAL_LIST);
        check_size_eq(rows.data.list.count, 2);
        check_int_eq(rows.data.list.items[1].type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&rows.data.list.items[1], "id").data.number, 2.0, 0.01);
      }

      {
        exprtk_value_t xml_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "<order id=\"9\" active=\"true\"><side>Buy</side><at>Sat, 04 Mar 2006 13:27:54 GMT</at><qtys>12</qtys><qtys>24</qtys><attrs><x>48</x></attrs></order>")
        };
        exprtk_value_t order = call_fn(&t, "data_bind.xml", 3, xml_args);
        exprtk_value_t qtys;
        exprtk_value_t attrs;
        check_int_eq(order.type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&order, "id").data.number, 9.0, 0.01);
        check_float_eq(exprtk_map_get(&order, "side").data.number, 1.0, 0.01);
        check_int_eq(exprtk_map_get(&order, "at").type, EXPRTK_VAL_DATETIME);
        check_int_eq(exprtk_map_get(&order, "active").type, EXPRTK_VAL_BOOL);
        check_int_eq(exprtk_map_get(&order, "active").data.boolean, 1);
        qtys = exprtk_map_get(&order, "qtys");
        check_int_eq(qtys.type, EXPRTK_VAL_LIST);
        check_float_eq(qtys.data.list.items[1].data.number, 24.0, 0.01);
        attrs = exprtk_map_get(&order, "attrs");
        check_int_eq(attrs.type, EXPRTK_VAL_MAP);
        check_float_eq(exprtk_map_get(&attrs, "x").data.number, 48.0, 0.01);
      }

      {
        exprtk_value_t xml_all_args[4] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "<orders><order><id>1</id><side>Buy</side><at>Sat, 04 Mar 2006 13:27:54 GMT</at><active>true</active><qtys>10</qtys><attrs><x>30</x></attrs></order>"
                       "<order><id>2</id><side>Sell</side><at>Sat, 04 Mar 2006 13:27:54 GMT</at><active>false</active><qtys>20</qtys><attrs><x>40</x></attrs></order></orders>"),
          make_str(&t, "//order")
        };
        exprtk_value_t rows = call_fn(&t, "data_bind.xml_all", 4, xml_all_args);
        check_int_eq(rows.type, EXPRTK_VAL_LIST);
        check_size_eq(rows.data.list.count, 2);
        check_int_eq(rows.data.list.items[1].type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&rows.data.list.items[1], "id").data.number, 2.0, 0.01);
      }

      {
        exprtk_value_t json_path_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "test_order.json")
        };
        exprtk_value_t order = call_fn(&t, "data_bind.json_path", 3, json_path_args);
        exprtk_value_t attrs;
        check_int_eq(order.type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&order, "id").data.number, 7.0, 0.01);
        attrs = exprtk_map_get(&order, "attrs");
        check_int_eq(attrs.type, EXPRTK_VAL_MAP);
        check_float_eq(exprtk_map_get(&attrs, "x").data.number, 30.0, 0.01);
      }

      {
        exprtk_value_t json_all_path_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "test_orders.json")
        };
        exprtk_value_t rows = call_fn(&t, "data_bind.json_all_path", 3, json_all_path_args);
        check_int_eq(rows.type, EXPRTK_VAL_LIST);
        check_size_eq(rows.data.list.count, 2);
        check_int_eq(rows.data.list.items[0].type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&rows.data.list.items[0], "id").data.number, 7.0, 0.01);
        check_int_eq(rows.data.list.items[1].type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&rows.data.list.items[1], "id").data.number, 8.0, 0.01);
      }

      {
        exprtk_value_t csv_path_args[4] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "test_orders.csv"),
          exprtk_val_num(0.0)
        };
        exprtk_value_t order = call_fn(&t, "data_bind.csv_path", 4, csv_path_args);
        exprtk_value_t attrs;
        check_int_eq(order.type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&order, "id").data.number, 8.0, 0.01);
        attrs = exprtk_map_get(&order, "attrs");
        check_int_eq(attrs.type, EXPRTK_VAL_MAP);
        check_float_eq(exprtk_map_get(&attrs, "x").data.number, 44.0, 0.01);
      }

      {
        exprtk_value_t csv_all_path_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "test_orders_all.csv")
        };
        exprtk_value_t rows = call_fn(&t, "data_bind.csv_all_path", 3, csv_all_path_args);
        check_int_eq(rows.type, EXPRTK_VAL_LIST);
        check_size_eq(rows.data.list.count, 2);
        check_int_eq(rows.data.list.items[1].type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&rows.data.list.items[1], "id").data.number, 2.0, 0.01);
      }

      {
        exprtk_value_t xml_path_args[3] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "test_order.xml")
        };
        exprtk_value_t order = call_fn(&t, "data_bind.xml_path", 3, xml_path_args);
        exprtk_value_t attrs;
        check_int_eq(order.type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&order, "id").data.number, 9.0, 0.01);
        attrs = exprtk_map_get(&order, "attrs");
        check_int_eq(attrs.type, EXPRTK_VAL_MAP);
        check_float_eq(exprtk_map_get(&attrs, "x").data.number, 48.0, 0.01);
      }

      {
        exprtk_value_t xml_all_path_args[4] = {
          handle,
          make_str(&t, "Order"),
          make_str(&t, "test_orders.xml"),
          make_str(&t, "//order")
        };
        exprtk_value_t rows = call_fn(&t, "data_bind.xml_all_path", 4, xml_all_path_args);
        check_int_eq(rows.type, EXPRTK_VAL_LIST);
        check_size_eq(rows.data.list.count, 2);
        check_int_eq(rows.data.list.items[1].type, EXPRTK_VAL_OBJECT);
        check_float_eq(exprtk_map_get(&rows.data.list.items[1], "id").data.number, 2.0, 0.01);
      }

      exprtk_value_t close_args[1] = {handle};
      call_fn(&t, "data_bind.close", 1, close_args);

      remove("test_order.json");
      remove("test_orders.json");
      remove("test_orders.csv");
      remove("test_orders_all.csv");
      remove("test_order.xml");
      remove("test_orders.xml");
      remove("test_dynamic_plugin.tbe");
      test_env_free(&t);
    }
  }

  describe("data_bind YAML") {
    it("should bind YAML roots, sequences, YPATH matches, files, and strict validation") {
      test_env_t t;
      exprtk_value_t schema_args[1];
      exprtk_value_t handle;
      exprtk_value_t args[4];
      exprtk_value_t value;

      test_env_init(&t);
      check_not_null(t.db_plugin);
      schema_args[0] = make_str(&t,
        "message Order { uint32 id; string symbol; bool active; }\n");
      handle = call_fn(&t, "data_bind.create_from_text", 1, schema_args);
      check(handle.data.number >= 0.0);

      args[0] = handle;
      args[1] = make_str(&t, "Order");
      args[2] = make_str(&t, "id: 7\nsymbol: AAPL\nactive: true\n");
      value = call_fn(&t, "data_bind.yaml", 3, args);
      check_int_eq(value.type, EXPRTK_VAL_OBJECT);
      check_float_eq(exprtk_map_get(&value, "id").data.number, 7.0, 0.01);
      check_str_eq(exprtk_map_get(&value, "symbol").data.string.data, "AAPL");

      args[2] = make_str(&t,
        "- id: 1\n  symbol: AAPL\n  active: true\n"
        "- id: 2\n  symbol: MSFT\n  active: false\n");
      value = call_fn(&t, "data_bind.yaml_all", 3, args);
      check_int_eq(value.type, EXPRTK_VAL_LIST);
      check_size_eq(value.data.list.count, 2);
      check_float_eq(exprtk_map_get(&value.data.list.items[1], "id").data.number, 2.0, 0.01);

      args[2] = make_str(&t,
        "orders:\n"
        "  - id: 3\n    symbol: NVDA\n    active: true\n"
        "  - id: 4\n    symbol: AMD\n    active: false\n");
      args[3] = make_str(&t, "/orders[*]");
      value = call_fn(&t, "data_bind.yaml_ypath", 4, args);
      check_int_eq(value.type, EXPRTK_VAL_OBJECT);
      check_float_eq(exprtk_map_get(&value, "id").data.number, 3.0, 0.01);
      value = call_fn(&t, "data_bind.yaml_ypath_all", 4, args);
      check_int_eq(value.type, EXPRTK_VAL_LIST);
      check_size_eq(value.data.list.count, 2);
      check_str_eq(exprtk_map_get(&value.data.list.items[1], "symbol").data.string.data, "AMD");

      args[2] = make_str(&t, "id: 9\nsymbol: INTC\nactive: true\n");
      check_true(call_fn(&t, "data_bind.validate_yaml", 3, args).data.boolean);
      args[2] = make_str(&t, "id: bad\nsymbol: INTC\nactive: true\n");
      check_false(call_fn(&t, "data_bind.validate_yaml", 3, args).data.boolean);
      args[2] = make_str(&t,
        "orders:\n  - id: 5\n    symbol: QCOM\n    active: true\n");
      args[3] = make_str(&t, "/orders[*]");
      check_true(call_fn(&t, "data_bind.validate_yaml_path", 4, args).data.boolean);

      write_schema("test_order.yaml", "id: 10\nsymbol: ARM\nactive: true\n");
      args[2] = make_str(&t, "test_order.yaml");
      value = call_fn(&t, "data_bind.yaml_path", 3, args);
      check_int_eq(value.type, EXPRTK_VAL_OBJECT);
      check_float_eq(exprtk_map_get(&value, "id").data.number, 10.0, 0.01);

      call_fn(&t, "data_bind.close", 1, &handle);
      remove("test_order.yaml");
      test_env_free(&t);
    }
  }

  describe("data_bind SAX streams") {
    it("should bind buffered YAML and propagate callback errors") {
      test_env_t t;
      exprtk_value_t schema_args[1];
      exprtk_value_t handle;
      exprtk_value_t args[4];
      exprtk_value_t stream_handle;
      exprtk_value_t callback;
      exprtk_value_t value;
      exprtk_node_t *callback_ast;

      test_env_init(&t);
      check_not_null(t.db_plugin);
      schema_args[0] = make_str(&t,
        "message Order { uint32 id; string symbol; bool active; }\n");
      handle = call_fn(&t, "data_bind.create_from_text", 1, schema_args);
      check(handle.data.number >= 0.0);

      args[0] = handle;
      args[1] = make_str(&t, "Order");
      args[2] = make_str(&t, "id: 11\nsymbol: IBM\nactive: true\n");
      value = call_fn(&t, "data_bind.sax.yaml", 3, args);
      check_int_eq(value.type, EXPRTK_VAL_OBJECT);
      check_float_eq(exprtk_map_get(&value, "id").data.number, 11.0, 0.01);

      args[2] = make_str(&t,
        "orders:\n"
        "  - id: 12\n    symbol: ORCL\n    active: true\n"
        "  - id: 13\n    symbol: SAP\n    active: false\n");
      args[3] = make_str(&t, "/orders[*]");
      value = call_fn(&t, "data_bind.yaml_path_all_stream", 4, args);
      check_int_eq(value.type, EXPRTK_VAL_LIST);
      check_size_eq(value.data.list.count, 2);

      args[0] = handle;
      args[1] = make_str(&t, "Order");
      args[2] = make_str(&t, "/orders/*");
      stream_handle = call_fn(&t, "data_bind.sax.yaml_path_all_create", 3, args);
      check(stream_handle.data.number >= 0.0);

      callback_ast = exprtk_parse(
        "record_callback = (record, index) => record.id + index - 15;", 0);
      check_not_null(callback_ast);
      exprtk_eval(callback_ast, &t.env);
      callback = exprtk_env_get(&t.env, "record_callback");
      check_int_eq(callback.type, EXPRTK_VAL_FUNCTION);
      args[0] = stream_handle;
      args[1] = callback;
      check_true(call_fn(&t, "data_bind.sax.set_callback", 2, args).data.boolean);

      args[0] = stream_handle;
      args[1] = make_str(&t,
        "orders:\n  - id: 14\n    symbol: CRM\n    active: true\n");
      call_fn(&t, "data_bind.sax.feed", 2, args);
      t.env.aborted = 0;
      value = call_fn(&t, "data_bind.sax.finish", 1, &stream_handle);
      check_int_eq(t.env.aborted, 1);
      check_int_eq(value.type, EXPRTK_VAL_NUMBER);

      exprtk_free(callback_ast);
      call_fn(&t, "data_bind.close", 1, &handle);
      test_env_free(&t);
    }

    it("should bind inline JSON through the stream API") {
      test_env_t t;
      exprtk_value_t create_args[1];
      exprtk_value_t handle;
      exprtk_value_t stream_args[3];
      exprtk_value_t order;

      test_env_init(&t);
      check_not_null(t.db_plugin);
      write_schema("test_stream_plugin.tbe",
        "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
        "message Order { uint32 id; Side side; string symbol; }\n");

      create_args[0] = make_str(&t, "test_stream_plugin.tbe");
      handle = call_fn(&t, "data_bind.create", 1, create_args);
      check(handle.data.number >= 0.0);

      stream_args[0] = handle;
      stream_args[1] = make_str(&t, "Order");
      stream_args[2] = make_str(&t, "{\"id\":7,\"side\":\"Buy\",\"symbol\":\"ABCD\"}");
      order = call_fn(&t, "data_bind.sax.json", 3, stream_args);
      check_int_eq(order.type, EXPRTK_VAL_OBJECT);
      check_float_eq(exprtk_map_get(&order, "id").data.number, 7.0, 0.01);
      check_str_eq(exprtk_map_get(&order, "symbol").data.string.data, "ABCD");

      call_fn(&t, "data_bind.close", 1, &handle);
      remove("test_stream_plugin.tbe");
      test_env_free(&t);
    }

    it("should finish chunked and file-fed streams") {
      test_env_t t;
      exprtk_value_t create_args[1];
      exprtk_value_t handle;
      exprtk_value_t sax_create_args[3];
      exprtk_value_t stream_handle;
      exprtk_value_t feed_args[2];
      exprtk_value_t rows;

      test_env_init(&t);
      check_not_null(t.db_plugin);
      write_schema("test_stream_plugin.tbe",
        "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
        "message Order { uint32 id; Side side; string symbol; }\n");
      write_schema("test_stream_orders.csv",
        "id,side,symbol\n10,Buy,ABCD\n11,Sell,WXYZ\n");

      create_args[0] = make_str(&t, "test_stream_plugin.tbe");
      handle = call_fn(&t, "data_bind.create", 1, create_args);
      check(handle.data.number >= 0.0);

      sax_create_args[0] = handle;
      sax_create_args[1] = make_str(&t, "Order");
      sax_create_args[2] = make_str(&t, "$[*]");
      stream_handle = call_fn(&t, "data_bind.sax.json_path_all_create", 3, sax_create_args);
      check(stream_handle.data.number >= 0.0);

      feed_args[0] = stream_handle;
      feed_args[1] = make_str(&t, "[{\"id\":1,\"side\":\"Buy\",\"symbol\":\"AB");
      call_fn(&t, "data_bind.sax.feed", 2, feed_args);
      feed_args[1] = make_str(&t, "CD\"},{\"id\":2,\"side\":\"Sell\",\"symbol\":\"WXYZ\"}]");
      call_fn(&t, "data_bind.sax.feed", 2, feed_args);
      rows = call_fn(&t, "data_bind.sax.finish", 1, &stream_handle);
      check_int_eq(rows.type, EXPRTK_VAL_LIST);
      check_size_eq(rows.data.list.count, 2);
      check_float_eq(exprtk_map_get(&rows.data.list.items[1], "id").data.number, 2.0, 0.01);

      sax_create_args[2] = make_str(&t, "side == \"Sell\"");
      stream_handle = call_fn(&t, "data_bind.sax.csv_path_create", 3, sax_create_args);
      check(stream_handle.data.number >= 0.0);
      feed_args[0] = stream_handle;
      feed_args[1] = make_str(&t, "test_stream_orders.csv");
      call_fn(&t, "data_bind.sax.feed_path", 2, feed_args);
      rows = call_fn(&t, "data_bind.sax.finish", 1, &stream_handle);
      check_int_eq(rows.type, EXPRTK_VAL_LIST);
      check_size_eq(rows.data.list.count, 1);
      check_float_eq(exprtk_map_get(&rows.data.list.items[0], "id").data.number, 11.0, 0.01);

      stream_handle = call_fn(&t, "data_bind.sax.json_create", 2, sax_create_args);
      check(stream_handle.data.number >= 0.0);
      call_fn(&t, "data_bind.close", 1, &handle);
      t.env.aborted = 0;
      call_fn(&t, "data_bind.sax.finish", 1, &stream_handle);
      check_int_eq(t.env.aborted, 1);

      remove("test_stream_orders.csv");
      remove("test_stream_plugin.tbe");
      test_env_free(&t);
    }

    it("should reject streams beyond the handle limit") {
      test_env_t t;
      exprtk_value_t create_args[1];
      exprtk_value_t handle;
      exprtk_value_t stream_args[2];
      exprtk_value_t streams[DATA_BIND_TEST_MAX_HANDLES];
      exprtk_value_t overflow;
      size_t i;

      test_env_init(&t);
      check_not_null(t.db_plugin);
      write_schema("test_stream_limit.tbe", "message Order { uint32 id; }\n");
      create_args[0] = make_str(&t, "test_stream_limit.tbe");
      handle = call_fn(&t, "data_bind.create", 1, create_args);
      check(handle.data.number >= 0.0);

      stream_args[0] = handle;
      stream_args[1] = make_str(&t, "Order");
      for (i = 0; i < DATA_BIND_TEST_MAX_HANDLES; ++i) {
        streams[i] = call_fn(&t, "data_bind.sax.json_create", 2, stream_args);
        check(streams[i].data.number >= 0.0);
      }
      overflow = call_fn(&t, "data_bind.sax.json_create", 2, stream_args);
      check_float_eq(overflow.data.number, -1.0, 0.01);
      check_int_eq(t.env.aborted, 1);

      call_fn(&t, "data_bind.close", 1, &handle);
      remove("test_stream_limit.tbe");
      test_env_free(&t);
    }
  }
}
