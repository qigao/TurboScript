/**
 * @file test_data_bind.c
 * @brief Unit tests for data_bind using tinytest
 *
 * Uses a proper mock Value that stores multiple named fields,
 * so we can verify actual parsed values from JIT-compiled functions.
 */

#include "data_bind.h"
#include "tbe_wire.h"
#include "tinytest.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───── Mock Value: stores up to 32 named fields ───── */

#define MAX_FIELDS 32
#define MAX_CONTAINER_ITEMS 16

typedef enum {
  FIELD_INT,
  FIELD_INT64,
  FIELD_DOUBLE,
  FIELD_STRING,
  FIELD_BYTES,
  FIELD_LIST,
  FIELD_SET,
  FIELD_MAP
} FieldType;

typedef struct {
  int count;
  int32_t int_items[MAX_CONTAINER_ITEMS];
  int64_t int64_items[MAX_CONTAINER_ITEMS];
  double dbl_items[MAX_CONTAINER_ITEMS];
  char str_items[MAX_CONTAINER_ITEMS][64];
  char map_keys[MAX_CONTAINER_ITEMS][64];
  struct Value *obj_items[MAX_CONTAINER_ITEMS];
} MockList;

typedef struct {
  char name[128];
  FieldType type;
  int32_t int_val;
  int64_t int64_val;
  double dbl_val;
  char str_val[256];
  uint8_t bytes_val[256];
  size_t bytes_len;
  MockList list_val;
} MockField;

typedef struct Value {
  MockField fields[MAX_FIELDS];
  int field_count;
} Value;

/* ───── Mock API functions ───── */

static Value *mock_create_object(void) {
  Value *v = (Value *)calloc(1, sizeof(Value));
  return v;
}

static void mock_set_field_int(Value *obj, const char *name, int32_t val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_INT;
  f->int_val = val;
}

static void mock_set_field_int64(Value *obj, const char *name, int64_t val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_INT64;
  f->int64_val = val;
}

static void mock_set_field_double(Value *obj, const char *name, double val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_DOUBLE;
  f->dbl_val = val;
}

static void mock_set_field_string(Value *obj, const char *name, const char *val) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_STRING;
  if (val) strncpy(f->str_val, val, sizeof(f->str_val) - 1);
}

static void mock_set_field_bytes(Value *obj, const char *name, const uint8_t *data, size_t len) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_BYTES;
  f->bytes_len = len < sizeof(f->bytes_val) ? len : sizeof(f->bytes_val);
  if (data) memcpy(f->bytes_val, data, f->bytes_len);
}

static Value *mock_create_list(void) { return (Value *)calloc(1, sizeof(Value)); }

static void mock_add_list_item_int(Value *list, int32_t val) {
  if (!list) return;
  MockList *l = &list->fields[0].list_val;
  if (l->count < MAX_CONTAINER_ITEMS) l->int_items[l->count++] = val;
}

static void mock_add_list_item_int64(Value *list, int64_t val) {
  if (!list) return;
  MockList *l = &list->fields[0].list_val;
  if (l->count < MAX_CONTAINER_ITEMS) l->int64_items[l->count++] = val;
}

static void mock_add_list_item_double(Value *list, double val) {
  if (!list) return;
  MockList *l = &list->fields[0].list_val;
  if (l->count < MAX_CONTAINER_ITEMS) l->dbl_items[l->count++] = val;
}

static void mock_add_list_item_string(Value *list, const char *val) {
  if (!list || val == NULL) return;
  MockList *l = &list->fields[0].list_val;
  if (l->count < MAX_CONTAINER_ITEMS) strncpy(l->str_items[l->count++], val, 63);
}

static void mock_add_list_item_object(Value *list, Value *obj) {
  if (!list) return;
  MockList *l = &list->fields[0].list_val;
  if (l->count < MAX_CONTAINER_ITEMS) l->obj_items[l->count++] = obj;
}

static void mock_set_field_list(Value *obj, const char *name, Value *list) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_LIST;
  if (list) f->list_val = list->fields[0].list_val;
  free(list);
}

static Value *mock_create_set(void) { return (Value *)calloc(1, sizeof(Value)); }
static void mock_add_set_item_int(Value *set, int32_t val) { mock_add_list_item_int(set, val); }
static void mock_add_set_item_double(Value *set, double val) {
  mock_add_list_item_double(set, val);
}
static void mock_add_set_item_string(Value *set, const char *val) {
  mock_add_list_item_string(set, val);
}
static void mock_set_field_set(Value *obj, const char *name, Value *set) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_SET;
  if (set) f->list_val = set->fields[0].list_val;
  free(set);
}

static Value *mock_create_map(void) { return (Value *)calloc(1, sizeof(Value)); }
static void mock_add_map_entry_string_string(Value *map, const char *key, const char *val) {
  if (!map || key == NULL || val == NULL) return;
  MockList *m = &map->fields[0].list_val;
  if (m->count < MAX_CONTAINER_ITEMS) {
    strncpy(m->map_keys[m->count], key, 63);
    strncpy(m->str_items[m->count], val, 63);
    m->count++;
  }
}
static void mock_add_map_entry_string_int(Value *map, const char *key, int32_t val) {
  if (!map || key == NULL) return;
  MockList *m = &map->fields[0].list_val;
  if (m->count < MAX_CONTAINER_ITEMS) {
    strncpy(m->map_keys[m->count], key, 63);
    m->int_items[m->count++] = val;
  }
}
static void mock_add_map_entry_string_double(Value *map, const char *key, double val) {
  if (!map || key == NULL) return;
  MockList *m = &map->fields[0].list_val;
  if (m->count < MAX_CONTAINER_ITEMS) {
    strncpy(m->map_keys[m->count], key, 63);
    m->dbl_items[m->count++] = val;
  }
}
static void mock_set_field_map(Value *obj, const char *name, Value *map) {
  if (!obj || obj->field_count >= MAX_FIELDS) return;
  MockField *f = &obj->fields[obj->field_count++];
  strncpy(f->name, name, sizeof(f->name) - 1);
  f->type = FIELD_MAP;
  if (map) f->list_val = map->fields[0].list_val;
  free(map);
}

static DataBindValueApi test_api = {
    .create_object = mock_create_object,
    .set_field_int = mock_set_field_int,
    .set_field_int64 = mock_set_field_int64,
    .set_field_double = mock_set_field_double,
    .set_field_string = mock_set_field_string,
    .set_field_bytes = mock_set_field_bytes,
    .create_list = mock_create_list,
    .add_list_item_int = mock_add_list_item_int,
    .add_list_item_int64 = mock_add_list_item_int64,
    .add_list_item_double = mock_add_list_item_double,
    .add_list_item_string = mock_add_list_item_string,
    .add_list_item_object = mock_add_list_item_object,
    .set_field_list = mock_set_field_list,
    .create_set = mock_create_set,
    .add_set_item_int = mock_add_set_item_int,
    .add_set_item_double = mock_add_set_item_double,
    .add_set_item_string = mock_add_set_item_string,
    .set_field_set = mock_set_field_set,
    .create_map = mock_create_map,
    .add_map_entry_string_string = mock_add_map_entry_string_string,
    .add_map_entry_string_int = mock_add_map_entry_string_int,
    .add_map_entry_string_double = mock_add_map_entry_string_double,
    .set_field_map = mock_set_field_map,
};

/* ───── Helpers to look up fields by name ───── */

static MockField *find_field(Value *v, const char *name) {
  if (!v) return NULL;
  for (int i = 0; i < v->field_count; i++) {
    if (strcmp(v->fields[i].name, name) == 0) return &v->fields[i];
  }
  return NULL;
}

static void write_schema(const char *path, const char *content) {
  FILE *f = fopen(path, "w");
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

/* ───── Tests ───── */

suite("Data Bind") {

  section("Codec Creation") {
    given("a valid schema file") {
      write_schema("test_create.tbe", "message Ping { uint32 seq; }\n");

      when("creating codec from schema") {
        DataBind *codec = data_bind_create("test_create.tbe", &test_api);

        then("codec should be non-null") { check_not_null(codec); }
        data_bind_free(codec);
      }
      remove("test_create.tbe");
    }

    given("a nonexistent schema file") {
      when("creating codec") {
        DataBind *codec = data_bind_create("no_such_file.tbe", &test_api);
        then("should return NULL") { check_null(codec); }
      }
    }

    given("a NULL api") {
      when("creating codec") {
        write_schema("test_null_api.tbe", "message M { uint32 x; }\n");
        DataBind *codec = data_bind_create("test_null_api.tbe", NULL);
        then("should return NULL") { check_null(codec); }
        remove("test_null_api.tbe");
      }
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

      DataBind *codec = data_bind_create("test_prim.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing binary data with known values") {
          uint8_t buf[15];
          memset(buf, 0, sizeof(buf));
          buf[0] = 0xAB;                    /* uint8:  a = 0xAB = 171 */
          write_u16_le(buf, 1, 0x1234);     /* uint16: b = 0x1234 = 4660 */
          write_u32_le(buf, 3, 42);         /* uint32: c = 42 */
          write_u64_le(buf, 7, 1000000ULL); /* uint64: d = 1000000 */

          Value *v = data_bind_parse(codec, "Primitives", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("uint8 field a should be 171") {
            MockField *f = find_field(v, "a");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 171);
            }
          }

          then("uint16 field b should be 4660") {
            MockField *f = find_field(v, "b");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 4660);
            }
          }

          then("uint32 field c should be 42") {
            MockField *f = find_field(v, "c");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 42);
            }
          }

          then("uint64 field d should be 1000000 (as int64)") {
            MockField *f = find_field(v, "d");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT64);
              check(f->int64_val == 1000000LL);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_prim.tbe");
    }
  }

  section("Composite Type Parsing") {
    given("a schema with a composite header") {
      write_schema("test_comp.tbe", "composite Header {\n"
                                    "    uint16 version;\n"
                                    "    uint32 seq;\n"
                                    "}\n"
                                    "message Msg {\n"
                                    "    Header header;\n"
                                    "    uint32 payload;\n"
                                    "}\n");

      DataBind *codec = data_bind_create("test_comp.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing binary data") {
          uint8_t buf[10];
          memset(buf, 0, sizeof(buf));
          write_u16_le(buf, 0, 3);    /* header.version = 3 */
          write_u32_le(buf, 2, 99);   /* header.seq = 99 */
          write_u32_le(buf, 6, 7777); /* payload = 7777 */

          Value *v = data_bind_parse(codec, "Msg", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("header.version should be 3") {
            MockField *f = find_field(v, "header.version");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 3);
            }
          }

          then("header.seq should be 99") {
            MockField *f = find_field(v, "header.seq");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 99);
            }
          }

          then("payload should be 7777") {
            MockField *f = find_field(v, "payload");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_INT);
              check(f->int_val == 7777);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_comp.tbe");
    }
  }

  section("Enum Type Parsing") {
    given("a schema with an enum field") {
      write_schema("test_enum.tbe", "enum Side <uint8> { Buy = 1; Sell = 2; }\n"
                                    "message Order {\n"
                                    "    uint32 id;\n"
                                    "    Side side;\n"
                                    "    uint32 qty;\n"
                                    "}\n");

      DataBind *codec = data_bind_create("test_enum.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing with side=Buy(1)") {
          uint8_t buf[9];
          memset(buf, 0, sizeof(buf));
          write_u32_le(buf, 0, 100); /* id = 100 */
          buf[4] = 1;                /* side = Buy(1) */
          write_u32_le(buf, 5, 500); /* qty = 500 */

          Value *v = data_bind_parse(codec, "Order", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("id should be 100") {
            MockField *f = find_field(v, "id");
            check_not_null(f);
            if (f) check(f->int_val == 100);
          }

          then("side should be 1 (Buy)") {
            MockField *f = find_field(v, "side");
            check_not_null(f);
            if (f) check(f->int_val == 1);
          }

          then("qty should be 500") {
            MockField *f = find_field(v, "qty");
            check_not_null(f);
            if (f) check(f->int_val == 500);
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_enum.tbe");
    }
  }

  section("Bounds Checking") {
    given("a schema with uint32 field") {
      write_schema("test_bounds.tbe", "message Small { uint32 x; }\n");

      DataBind *codec = data_bind_create("test_bounds.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("buffer is too short") {
          uint8_t buf[2] = {0x01, 0x02}; /* only 2 bytes, need 4 */
          Value *v = data_bind_parse(codec, "Small", buf, sizeof(buf));

          then("should return NULL (bounds violation)") { check_null(v); }
        }

        when("buffer is exactly right size") {
          uint8_t buf[4];
          write_u32_le(buf, 0, 12345);
          Value *v = data_bind_parse(codec, "Small", buf, sizeof(buf));

          then("should succeed") { check_not_null(v); }

          then("x should be 12345") {
            MockField *f = find_field(v, "x");
            check_not_null(f);
            if (f) check(f->int_val == 12345);
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_bounds.tbe");
    }
  }

  section("Error Handling") {
    given("a codec for a single message type") {
      write_schema("test_errh.tbe", "message Foo { uint32 x; }\n");

      DataBind *codec = data_bind_create("test_errh.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing an unknown type name") {
          uint8_t buf[4] = {0};
          Value *v = data_bind_parse(codec, "Bar", buf, sizeof(buf));

          then("should return NULL") { check_null(v); }

          then("error message should mention the type") {
            const char *err = data_bind_get_error(codec);
            check_not_null(err);
            if (err) check(strstr(err, "Bar") != NULL);
          }
        }

        when("passing NULL buffer") {
          Value *v = data_bind_parse(codec, "Foo", NULL, 0);
          then("should return NULL") { check_null(v); }
        }

        data_bind_free(codec);
      }
      remove("test_errh.tbe");
    }
  }

  section("Memory Management") {
    given("multiple codec instances") {
      write_schema("test_mem.tbe", "message Msg { uint32 x; }\n");

      when("creating and freeing multiple codecs") {
        DataBind *c1 = data_bind_create("test_mem.tbe", &test_api);
        DataBind *c2 = data_bind_create("test_mem.tbe", &test_api);
        DataBind *c3 = data_bind_create("test_mem.tbe", &test_api);

        then("all should be created") {
          check_not_null(c1);
          check_not_null(c2);
          check_not_null(c3);
        }

        data_bind_free(c1);
        data_bind_free(c2);
        data_bind_free(c3);
        data_bind_free(NULL); /* should not crash */

        then("should not crash") { check(1); }
      }

      remove("test_mem.tbe");
    }
  }

  section("NULL set_field_bytes callback") {
    given("an API without set_field_bytes") {
      DataBindValueApi api_no_bytes = {
          .create_object = mock_create_object,
          .set_field_int = mock_set_field_int,
          .set_field_int64 = mock_set_field_int64,
          .set_field_double = mock_set_field_double,
          .set_field_string = mock_set_field_string,
          .set_field_bytes = NULL /* NULL callback */
      };

      write_schema("test_no_bytes.tbe", "message SimpleMsg {\n"
                                        "    uint32 x;\n"
                                        "    uint32 y;\n"
                                        "}\n");

      when("creating codec") {
        DataBind *codec = data_bind_create("test_no_bytes.tbe", &api_no_bytes);

        then("should succeed") { check_not_null(codec); }

        when("parsing message without bytes fields") {
          uint8_t buf[8] = {42, 0, 0, 0, 99, 0, 0, 0};
          Value *v = data_bind_parse(codec, "SimpleMsg", buf, sizeof(buf));

          then("should parse without crash") { check_not_null(v); }

          then("should have correct values") {
            MockField *fx = find_field(v, "x");
            MockField *fy = find_field(v, "y");
            check_not_null(fx);
            check_not_null(fy);
            if (fx) check(fx->int_val == 42);
            if (fy) check(fy->int_val == 99);
          }

          free(v);
        }

        data_bind_free(codec);
      }

      remove("test_no_bytes.tbe");
    }
  }

  section("Dynamic function list (no MAX_FUNCS limit)") {
    given("a schema with multiple message types") {
      write_schema("test_many_msgs.tbe", "message Msg0 { uint32 x; }\n"
                                         "message Msg1 { uint32 x; }\n"
                                         "message Msg2 { uint32 x; }\n");

      when("creating codec") {
        DataBind *codec = data_bind_create("test_many_msgs.tbe", &test_api);

        then("should succeed") { check_not_null(codec); }

        when("parsing all 3 types") {
          uint8_t buf[4] = {42, 0, 0, 0};
          int success_count = 0;

          for (int i = 0; i < 3; i++) {
            char type[32];
            snprintf(type, sizeof(type), "Msg%d", i);
            Value *v = data_bind_parse(codec, type, buf, sizeof(buf));
            if (v) {
              success_count++;
              free(v);
            }
          }

          then("should parse all 3 types") { check(success_count == 3); }
        }

        data_bind_free(codec);
      }

      remove("test_many_msgs.tbe");
    }
  }

  section("Variable-length String Parsing") {
    given("a schema with string field") {
      write_schema("test_varstr.tbe", "message Msg { string name; }\n");

      DataBind *codec = data_bind_create("test_varstr.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing buffer with varstr 'Turbo'") {
          uint8_t buf[9];
          write_u32_le(buf, 0, 5); /* length = 5 */
          memcpy(buf + 4, "Turbo", 5);
          Value *v = data_bind_parse(codec, "Msg", buf, sizeof(buf));

          then("should succeed") { check_not_null(v); }

          then("name should be 'Turbo'") {
            MockField *f = find_field(v, "name");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_STRING);
              check(strcmp(f->str_val, "Turbo") == 0);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }
      remove("test_varstr.tbe");
    }
  }

  section("Variable-length String Field Order Validation") {
    given("a schema with var-data followed by fixed field") {
      write_schema("test_varstr_tail.tbe", "message Msg {\n"
                                           "  string name;\n"
                                           "  uint32 qty;\n"
                                           "}\n");

      DataBind *codec = data_bind_create("test_varstr_tail.tbe", &test_api);

      then("codec creation should fail because TBE requires fixed fields before var-data") {
        check_null(codec);
      }

      remove("test_varstr_tail.tbe");
    }
  }

  section("Fixed Array Parsing") {
    given("a schema with uint32 fixed array") {
      write_schema("test_array.tbe", "message Arr { uint32[3] values; }\n");

      DataBind *codec = data_bind_create("test_array.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing three values") {
          uint8_t buf[12];
          write_u32_le(buf, 0, 11);
          write_u32_le(buf, 4, 22);
          write_u32_le(buf, 8, 33);
          Value *v = data_bind_parse(codec, "Arr", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("values should be stored in list order") {
            MockField *f = find_field(v, "values");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_LIST);
              check(f->list_val.count == 3);
              check(f->list_val.int_items[0] == 11);
              check(f->list_val.int_items[1] == 22);
              check(f->list_val.int_items[2] == 33);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }

      remove("test_array.tbe");
    }
  }

  section("Group Parsing") {
    given("a schema with repeating group and trailing var-data") {
      write_schema(
          "test_group.tbe",
          "group Level { uint64 price; uint32 qty; }\n"
          "message Book { uint32 seq; group<Level> bids; string symbol; bytes source; }\n");

      DataBind *codec = data_bind_create("test_group.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing two group entries") {
          uint8_t buf[4 + 4 + 24 + 4 + 4 + 4 + 3];
          memset(buf, 0, sizeof(buf));
          write_u32_le(buf, 0, 7);
          write_u16_le(buf, 4, 12); /* blockLength */
          write_u16_le(buf, 6, 2);  /* numInGroup */
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

          Value *v = data_bind_parse(codec, "Book", buf, sizeof(buf));

          then("result should be non-null") { check_not_null(v); }

          then("bids should contain two objects") {
            MockField *f = find_field(v, "bids");
            check_not_null(f);
            if (f) {
              Value *bid0;
              Value *bid1;
              MockField *p0;
              MockField *q0;
              MockField *p1;
              MockField *q1;

              check(f->type == FIELD_LIST);
              check(f->list_val.count == 2);

              bid0 = f->list_val.obj_items[0];
              bid1 = f->list_val.obj_items[1];
              check_not_null(bid0);
              check_not_null(bid1);

              p0 = find_field(bid0, "price");
              q0 = find_field(bid0, "qty");
              p1 = find_field(bid1, "price");
              q1 = find_field(bid1, "qty");
              check_not_null(p0);
              check_not_null(q0);
              check_not_null(p1);
              check_not_null(q1);
              if (p0) check(p0->int64_val == 100);
              if (q0) check(q0->int_val == 10);
              if (p1) check(p1->int64_val == 200);
              if (q1) check(q1->int_val == 20);
            }
          }

          then("symbol and source should be parsed after the group") {
            MockField *sym = find_field(v, "symbol");
            MockField *src = find_field(v, "source");
            check_not_null(sym);
            check_not_null(src);
            if (sym) check(strcmp(sym->str_val, "ABCD") == 0);
            if (src) {
              check(src->bytes_len == 3);
              check(src->bytes_val[0] == 1);
              check(src->bytes_val[1] == 2);
              check(src->bytes_val[2] == 3);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }

      remove("test_group.tbe");
    }
  }

  section("Extended Types Parsing") {
    given("a schema with bool, float, double and multi-size enums") {
      write_schema("test_extended.tbe", "enum LargeEnum <uint32> { Big = 0x12345678; }\n"
                                        "message Ext {\n"
                                        "    bool flag;\n"
                                        "    float f_val;\n"
                                        "    double d_val;\n"
                                        "    LargeEnum le;\n"
                                        "}\n");

      DataBind *codec = data_bind_create("test_extended.tbe", &test_api);
      if (!codec) {
        fprintf(stderr, "[ERROR] data_bind_create failed for test_extended.tbe\n");
        fprintf(stderr, "[ERROR] Check if the file was created and is readable\n");
      }
      check_not_null(codec);

      when("parsing buffer with extended types") {
        uint8_t buf[1 + 4 + 8 + 4];
        buf[0] = 1;                        /* bool flag = true */
        write_f32_le(buf, 1, 3.14f);       /* float f_val */
        write_f64_le(buf, 5, 2.718281828); /* double d_val */
        write_u32_le(buf, 13, 0x12345678); /* LargeEnum le (uint32) */

        Value *v = data_bind_parse(codec, "Ext", buf, sizeof(buf));

        then("should succeed") { check_not_null(v); }

        then("bool flag should be 1") {
          MockField *f = find_field(v, "flag");
          check_not_null(f);
          if (f) {
            check(f->type == FIELD_INT);
            check(f->int_val == 1);
          }
        }

        then("float f_val should be approx 3.14") {
          MockField *f = find_field(v, "f_val");
          check_not_null(f);
          if (f) {
            check(f->type == FIELD_DOUBLE);
            check(fabs(f->dbl_val - 3.14) < 1e-4);
          }
        }

        then("double d_val should be approx 2.71828") {
          MockField *f = find_field(v, "d_val");
          check_not_null(f);
          if (f) {
            check(f->type == FIELD_DOUBLE);
            check(fabs(f->dbl_val - 2.718281828) < 1e-9);
          }
        }

        then("LargeEnum le should be 0x12345678") {
          MockField *f = find_field(v, "le");
          check_not_null(f);
          if (f) {
            check(f->type == FIELD_INT);
            check(f->int_val == 0x12345678);
          }
        }

        if (v) free(v);
      }

      data_bind_free(codec);
    }
    remove("test_extended.tbe");
  }

  section("Set Parsing") {
    given("a schema with set<string> field") {
      write_schema("test_set.tbe", "message Tags { set<string> tags; }\n");

      DataBind *codec = data_bind_create("test_set.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing buffer with two set items") {
          uint8_t buf[14];
          memset(buf, 0, sizeof(buf));
          write_u32_le(buf, 0, 2);
          write_u32_le(buf, 4, 1);
          buf[8] = 'A';
          write_u32_le(buf, 9, 1);
          buf[13] = 'B';

          Value *v = data_bind_parse(codec, "Tags", buf, sizeof(buf));

          then("codec should parse set data") { check_not_null(v); }

          then("tags should contain both entries") {
            MockField *f = find_field(v, "tags");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_SET);
              check(f->list_val.count == 2);
              check(strcmp(f->list_val.str_items[0], "A") == 0);
              check(strcmp(f->list_val.str_items[1], "B") == 0);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }

      remove("test_set.tbe");
    }
  }

  section("Map Parsing") {
    given("a schema with map<string, int32> field") {
      write_schema("test_map.tbe", "message Attrs { map<string,int32> attrs; }\n");

      DataBind *codec = data_bind_create("test_map.tbe", &test_api);
      check_not_null(codec);

      if (codec) {
        when("parsing buffer with two map entries") {
          uint8_t buf[22];
          memset(buf, 0, sizeof(buf));
          write_u32_le(buf, 0, 2);
          write_u32_le(buf, 4, 1);
          buf[8] = 'x';
          write_i32_le(buf, 9, 30);
          write_u32_le(buf, 13, 1);
          buf[17] = 'y';
          write_i32_le(buf, 18, 40);

          Value *v = data_bind_parse(codec, "Attrs", buf, sizeof(buf));

          then("codec should parse map data") { check_not_null(v); }

          then("attrs should contain key-value entries") {
            MockField *f = find_field(v, "attrs");
            check_not_null(f);
            if (f) {
              check(f->type == FIELD_MAP);
              check(f->list_val.count == 2);
              check(strcmp(f->list_val.map_keys[0], "x") == 0);
              check(strcmp(f->list_val.map_keys[1], "y") == 0);
              check(f->list_val.int_items[0] == 30);
              check(f->list_val.int_items[1] == 40);
            }
          }

          if (v) free(v);
        }

        data_bind_free(codec);
      }

      remove("test_map.tbe");
    }
  }
}
