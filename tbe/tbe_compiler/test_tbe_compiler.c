#include "mustache.h"
#include "mustache_helpers.h"
#include "compiler_core.h"
#include "node_tree.h"
#include "tbe_wire.h"
#include "schema_parser_dsl.h"
#include "tinytest.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define tt_close _close
#define tt_dup _dup
#define tt_dup2 _dup2
#define tt_fileno _fileno
#define TT_NULL_DEVICE "NUL"
#else
#define tt_close close
#define tt_dup dup
#define tt_dup2 dup2
#define tt_fileno fileno
#define TT_NULL_DEVICE "/dev/null"
#endif

static void cleanup_test_file(const char *path);

static Node *find_child(Node *parent, const char *name) {
  if (!parent || parent->type != NODE_MAP) return NULL;

  for (size_t i = 0; i < parent->data.map.count; ++i) {
    Node *child = parent->data.map.items[i];
    if (child->name && strcmp(child->name, name) == 0) return child;
  }

  return NULL;
}

static char *render_c_template(const char *schema) {
  size_t template_size = 0;
  char *template_text = tt_read_file(C_STRUCT_TEMPLATE_FILE, &template_size);
  Node *root = NULL;
  MUSTACHE_TEMPLATE *templ = NULL;
  MUSTACHE_STRING_RENDERER renderer;
  MUSTACHE_DATAPROVIDER provider = mustache_helpers_provider();
  char *output = NULL;
  int renderer_ready = 0;

  if (!template_text) return NULL;

  root = create_node_map(NULL);
  if (!root) goto cleanup;

  if (parse_schema(schema, strlen(schema), root, NULL) != 0) goto cleanup;

  templ = mustache_compile(template_text, template_size, NULL, NULL, 0);
  if (!templ) goto cleanup;

  if (mustache_string_renderer_init(&renderer) != 0) goto cleanup;
  renderer_ready = 1;

  if (mustache_process(templ, (MUSTACHE_RENDERER *)&renderer, &renderer, &provider, root) !=
      MUSTACHE_ERR_SUCCESS) {
    goto cleanup;
  }

  output = mustache_string_renderer_get(&renderer);

cleanup:
  if (renderer_ready) {
    mustache_string_renderer_free(&renderer);
  }
  mustache_release(templ);
  node_free(root);
  free(template_text);
  return output;
}

static int write_test_file(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (!file) return -1;
  if (fwrite(text, 1, strlen(text), file) != strlen(text)) {
    fclose(file);
    return -1;
  }
  return fclose(file);
}

static char *render_compiler_template_from_schema(const char *schema,
                                                  const char *schema_path,
                                                  const char *template_file,
                                                  const char *output_path) {
  Node *root = NULL;
  char *schema_data = NULL;
  char *output = NULL;
  size_t output_size = 0;

  cleanup_test_file(schema_path);
  cleanup_test_file(output_path);
  if (write_test_file(schema_path, schema) != 0) goto cleanup;
  if (tbe_compiler_parse_schema_file(schema_path, &root, &schema_data) != 0) goto cleanup;
  if (tbe_compiler_render_file(root, template_file, output_path) != 0) goto cleanup;
  output = tt_read_file(output_path, &output_size);
  if (!output || output_size == 0) {
    free(output);
    output = NULL;
    goto cleanup;
  }

cleanup:
  free(schema_data);
  node_free(root);
  cleanup_test_file(schema_path);
  cleanup_test_file(output_path);
  return output;
}

static void cleanup_test_file(const char *path) {
  if (path) {
    remove(path);
  }
}

static int parse_schema_quietly(const char *schema, size_t size, Node *root) {
  int saved_stdout = -1;
  int saved_stderr = -1;
  FILE *null_file = NULL;
  int result;

  fflush(stdout);
  fflush(stderr);
  saved_stdout = tt_dup(tt_fileno(stdout));
  saved_stderr = tt_dup(tt_fileno(stderr));
  if (saved_stdout < 0 || saved_stderr < 0) {
    if (saved_stdout >= 0) tt_close(saved_stdout);
    if (saved_stderr >= 0) tt_close(saved_stderr);
    return parse_schema(schema, size, root, NULL);
  }

  null_file = freopen(TT_NULL_DEVICE, "w", stdout);
  if (!null_file) {
    tt_close(saved_stdout);
    tt_close(saved_stderr);
    return parse_schema(schema, size, root, NULL);
  }

  null_file = freopen(TT_NULL_DEVICE, "w", stderr);
  if (!null_file) {
    tt_dup2(saved_stdout, tt_fileno(stdout));
    tt_close(saved_stdout);
    tt_close(saved_stderr);
    return parse_schema(schema, size, root, NULL);
  }

  result = parse_schema(schema, size, root, NULL);
  fflush(stdout);
  fflush(stderr);
  tt_dup2(saved_stdout, tt_fileno(stdout));
  tt_dup2(saved_stderr, tt_fileno(stderr));
  tt_close(saved_stdout);
  tt_close(saved_stderr);
  return result;
}

spec("tbe_compiler") {
  describe("Node creation") {
    it("should create string node") {
      Node *n = create_node_string("test_key", "test_val");
      check_not_null(n);
      check_int_eq(n->type, NODE_STRING);
      check_str_eq(n->name, "test_key");
      check_str_eq(n->data.string_val, "test_val");
      node_free(n);
    }

    it("should create list node") {
      Node *n = create_node_list("list_key");
      check_not_null(n);
      check_int_eq(n->type, NODE_LIST);
      check_str_eq(n->name, "list_key");
      check_int_eq(n->data.list.count, 0);
      node_free(n);
    }

    it("should map_add element") {
      Node *m = create_node_map(NULL);
      Node *v = create_node_string("key", "val");
      map_add(m, v);
      check_int_eq(m->data.map.count, 1);
      check_ptr_eq(m->data.map.items[0], v);
      node_free(m); /* recursively frees v */
    }
  }

  describe("Mustache helpers") {
    it("provider and renderer should be valid") {
      MUSTACHE_DATAPROVIDER p = mustache_helpers_provider();
      MUSTACHE_RENDERER r = mustache_helpers_renderer();
      check_not_null(p.get_root);
      check_not_null(p.dump);
      check_not_null(p.get_child_by_name);
      check_not_null(p.get_child_by_index);
      check_not_null(r.out_verbatim);
      check_not_null(r.out_escaped);
    }

    it("get_child_by_name should find key in map") {
      Node *m = create_node_map(NULL);
      Node *v = create_node_string("key", "val");
      map_add(m, v);

      MUSTACHE_DATAPROVIDER p = mustache_helpers_provider();
      void *res = p.get_child_by_name(m, "key", 3, NULL);
      check_ptr_eq(res, v);
      void *res2 = p.get_child_by_name(m, "nokey", 5, NULL);
      check_null(res2);
      node_free(m);
    }

    it("get_child_by_index should iterate list") {
      Node *l = create_node_list(NULL);
      Node *v1 = create_node_string("1", "val1");
      Node *v2 = create_node_string("2", "val2");
      list_add(l, v1);
      list_add(l, v2);

      MUSTACHE_DATAPROVIDER p = mustache_helpers_provider();
      void *r1 = p.get_child_by_index(l, 0, NULL);
      void *r2 = p.get_child_by_index(l, 1, NULL);
      void *r3 = p.get_child_by_index(l, 2, NULL);

      check_ptr_eq(r1, v1);
      check_ptr_eq(r2, v2);
      check_null(r3);
      node_free(l);
    }

    it("get_child_by_name should handle dot notation for nested lookups") {
      Node *m = create_node_map(NULL);
      Node *child = create_node_map("child");
      Node *grandchild = create_node_string("grandchild", "secret");
      map_add(child, grandchild);
      map_add(m, child);

      MUSTACHE_DATAPROVIDER p = mustache_helpers_provider();
      void *res = p.get_child_by_name(m, "child.grandchild", 16, NULL);
      check_ptr_eq(res, grandchild);

      node_free(m);
    }

    it("get_child_by_index should iterate maps too") {
      Node *m = create_node_map(NULL);
      Node *v1 = create_node_string("k1", "v1");
      Node *v2 = create_node_string("k2", "v2");
      map_add(m, v1);
      map_add(m, v2);

      MUSTACHE_DATAPROVIDER p = mustache_helpers_provider();
      void *r1 = p.get_child_by_index(m, 0, NULL);
      void *r2 = p.get_child_by_index(m, 1, NULL);

      check_ptr_eq(r1, v1);
      check_ptr_eq(r2, v2);
      node_free(m);
    }
  }

  describe("Wire helpers") {
    it("should round-trip fixed primitive writes and reads") {
      uint8_t buf[8] = {0};

      tbe_wire_write_u32(buf, 0, 0x11223344u);
      check_uint_eq(tbe_wire_read_u32(buf, 0), 0x11223344u);

      tbe_wire_write_i16(buf, 1, -1234);
      check_int_eq(tbe_wire_read_i16(buf, 1), -1234);
    }

    it("should round-trip variable data writes and reads") {
      uint8_t buf[32] = {0};
      tbe_var_data_t value;
      const char payload[] = "abc";

      check(tbe_wire_write_var_data(buf, sizeof(buf), 0, payload, 3));
      check(tbe_wire_read_var_data(buf, sizeof(buf), 0, &value));
      check_uint_eq(value.size, 3);
      check(memcmp(value.data, payload, 3) == 0);
    }
  }

  describe("Schema Parser") {
    it("should parse empty schema") {
      Node *root = create_node_map(NULL);
      const char *empty = "";
      int res = parse_schema(empty, strlen(empty), root, NULL);
      check_int_eq(res, 0);
      node_free(root);
    }

    it("should parse simple composite") {
      Node *root = create_node_map(NULL);
      const char *schema = "composite Point { uint32_t x; uint32_t y; }";
      int res = parse_schema(schema, strlen(schema), root, NULL);
      check_int_eq(res, 0);

      Node *composites = find_child(root, "composites");
      check_not_null(composites);
      check_str_eq(composites->name, "composites");
      check_int_eq(composites->type, NODE_LIST);
      check_int_eq(composites->data.list.count, 1);

      Node *point = composites->data.list.items[0];
      check_int_eq(point->type, NODE_MAP);

      node_free(root);
    }

    it("should fail on invalid syntax") {
      Node *root = create_node_map(NULL);
      const char *schema = "message Bad { uint32_t no_semi }";
      int res = parse_schema_quietly(schema, strlen(schema), root);
      check_int_eq(res, -1);
      node_free(root);
    }

    it("should reject legacy struct declarations") {
      Node *root = create_node_map(NULL);
      const char *schema = "struct Point { uint32_t x; uint32_t y; }";
      int res = parse_schema_quietly(schema, strlen(schema), root);
      check_int_eq(res, -1);
      node_free(root);
    }

    it("should parse full example.schema") {
#ifndef SCHEMA_EXAMPLE_FILE
  #define SCHEMA_EXAMPLE_FILE "example.schema"
#endif
      FILE *f = fopen(SCHEMA_EXAMPLE_FILE, "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        size_t size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *dat = malloc(size + 1);
        fread(dat, 1, size, f);
        dat[size] = '\0';
        fclose(f);

        Node *root = create_node_map(NULL);
        int res = parse_schema(dat, size, root, NULL);
        check_int_eq(res, 0);

        Node *schema = NULL;
        Node *messages = NULL;
        Node *composites = NULL;
        Node *enums = NULL;
        for (size_t i = 0; i < root->data.map.count; ++i) {
          Node *child = root->data.map.items[i];
          if (child->name && strcmp(child->name, "schema") == 0) schema = child;
          if (child->name && strcmp(child->name, "messages") == 0) messages = child;
          if (child->name && strcmp(child->name, "composites") == 0) composites = child;
          if (child->name && strcmp(child->name, "enums") == 0) enums = child;
        }

        check_not_null(schema);
        check_not_null(messages);
        check_not_null(composites);
        check_not_null(enums);
        check_str_eq(find_child(schema, "schema_name")->data.string_val, "Session");
        check_int_eq(messages->data.list.count, 2);
        check_int_eq(composites->data.list.count, 1);
        check_int_eq(enums->data.list.count, 1);

        node_free(root);
        free(dat);
      } else {
        check_int_eq(1, 0); // Fail test if file not found
      }
    }
  }

  describe("C template rendering") {
    it("should resolve built-in templates through compiler core") {
      check_str_eq(tbe_compiler_resolve_template(NULL, 0), "templates/c_structs.mustache");
      check_str_eq(tbe_compiler_resolve_template(NULL, 1),
                   "templates/python_dataclass.mustache");
      check_str_eq(tbe_compiler_resolve_template(NULL, 2), "templates/rust_structs.mustache");
      check_str_eq(tbe_compiler_resolve_template(NULL, TBE_COMPILER_LANG_CPP),
                   "templates/cpp_types.mustache");
      check_str_eq(tbe_compiler_resolve_template(NULL, TBE_COMPILER_LANG_GO),
                   "templates/go_types.mustache");
      check_str_eq(tbe_compiler_resolve_template(NULL, TBE_COMPILER_LANG_TS),
                   "templates/ts_types.mustache");
      check_str_eq(tbe_compiler_resolve_template("custom.mustache", 0), "custom.mustache");
    }

    it("should parse schema files through compiler core") {
      Node *root = NULL;
      char *schema_data = NULL;

      check_int_eq(tbe_compiler_parse_schema_file(SCHEMA_EXAMPLE_FILE, &root, &schema_data), 0);
      check_not_null(root);
      check_not_null(schema_data);
      check(find_child(root, "schema") != NULL);
      check(find_child(root, "messages") != NULL);

      free(schema_data);
      node_free(root);
    }

    it("should render template output through compiler core") {
      const char *output_path = "test_tbe_compiler_render.out";
      size_t output_size = 0;
      Node *root = NULL;
      char *schema_data = NULL;
      char *output = NULL;

      cleanup_test_file(output_path);
      check_int_eq(tbe_compiler_parse_schema_file(SCHEMA_EXAMPLE_FILE, &root, &schema_data), 0);
      check_int_eq(tbe_compiler_render_file(root, C_STRUCT_TEMPLATE_FILE, output_path), 0);

      output = tt_read_file(output_path, &output_size);
      check_not_null(output);
      check(output_size > 0);
      check_str_contains(output, "typedef struct Header_s {");
      check_str_contains(output, "typedef struct LoginMessage_s {");
      check_str_contains(output, "typedef struct Heartbeat_s {");

      free(output);
      free(schema_data);
      node_free(root);
      cleanup_test_file(output_path);
    }

    it("should run compiler core end-to-end with custom template") {
      const char *output_path = "test_tbe_compiler_run.out";
      const char *template_path = C_STRUCT_TEMPLATE_FILE;
      size_t output_size = 0;
      char *output = NULL;
      tbe_compiler_options_t options = {
          .schema_path = SCHEMA_EXAMPLE_FILE,
          .template_path = template_path,
          .output_path = output_path,
          .dsl_output_path = NULL,
          .lang_enum = 0,
      };

      cleanup_test_file(output_path);
      check_int_eq(tbe_compiler_run(&options), 0);

      output = tt_read_file(output_path, &output_size);
      check_not_null(output);
      check(output_size > 0);
      check_str_contains(output, "Session_WIRE_BIG_ENDIAN");
      check_str_contains(output, "LoginMessage_builder_bind");

      free(output);
      cleanup_test_file(output_path);
    }

    it("should generate textual MIR parser modules") {
      const char *output_path = "test_tbe_compiler_mir.out";
      size_t output_size = 0;
      char *output = NULL;
      tbe_compiler_options_t options = {
          .schema_path = SCHEMA_EXAMPLE_FILE,
          .template_path = NULL,
          .output_path = output_path,
          .dsl_output_path = NULL,
          .lang_enum = TBE_COMPILER_LANG_MIR,
      };

      cleanup_test_file(output_path);
      check_int_eq(tbe_compiler_run(&options), 0);

      output = tt_read_file(output_path, &output_size);
      check_not_null(output);
      check(output_size > 0);
      check_str_contains(output, "data_bind_binary");
      check_str_contains(output, "parse_LoginMessage");
      check_str_contains(output, "parse_Heartbeat");
      check_str_contains(output, "create_obj");
      check_str_contains(output, "read_varstr");

      free(output);
      cleanup_test_file(output_path);
    }

    it("should generate binary MIR parser modules") {
      const char *output_path = "test_tbe_compiler_bmir.out";
      size_t output_size = 0;
      char *output = NULL;
      tbe_compiler_options_t options = {
          .schema_path = SCHEMA_EXAMPLE_FILE,
          .template_path = NULL,
          .output_path = output_path,
          .dsl_output_path = NULL,
          .lang_enum = TBE_COMPILER_LANG_BMIR,
      };

      cleanup_test_file(output_path);
      check_int_eq(tbe_compiler_run(&options), 0);

      output = tt_read_file(output_path, &output_size);
      check_not_null(output);
      check(output_size > 0);

      free(output);
      cleanup_test_file(output_path);
    }

    it("should render C++ Go Python Rust and TypeScript type outputs") {
      const char *schema =
          "schema Market [byte_order(little)];"
          "enum Side <uint8> { Buy = 1; Sell = 2; }"
          "composite Header { uint32_t seq; }"
          "group Level { uint64 price; uint32 qty; }"
          "message Book { Header header; bytes(16) digest; group<Level> bids; string symbol; }";
      char *cpp_output = render_compiler_template_from_schema(
          schema, "test_tbe_compiler_lang.schema", CPP_TYPES_TEMPLATE_FILE,
          "test_tbe_compiler_lang.cpp.out");
      char *go_output = render_compiler_template_from_schema(
          schema, "test_tbe_compiler_lang.schema", GO_TYPES_TEMPLATE_FILE,
          "test_tbe_compiler_lang.go.out");
      char *py_output = render_compiler_template_from_schema(
          schema, "test_tbe_compiler_lang.schema", PYTHON_DATACLASS_TEMPLATE_FILE,
          "test_tbe_compiler_lang.py.out");
      char *rust_output = render_compiler_template_from_schema(
          schema, "test_tbe_compiler_lang.schema", RUST_STRUCTS_TEMPLATE_FILE,
          "test_tbe_compiler_lang.rs.out");
      char *ts_output = render_compiler_template_from_schema(
          schema, "test_tbe_compiler_lang.schema", TS_TYPES_TEMPLATE_FILE,
          "test_tbe_compiler_lang.ts.out");

      check_not_null(cpp_output);
      check_not_null(go_output);
      check_not_null(py_output);
      check_not_null(rust_output);
      check_not_null(ts_output);

      check_str_contains(cpp_output, "enum class Side : std::uint8_t");
      check_str_contains(cpp_output, "std::vector<Level> bids;");
      check_str_contains(cpp_output, "std::string symbol;");
      check_str_contains(cpp_output, "std::vector<std::uint8_t> digest;");

      check_str_contains(go_output, "package market");
      check_str_contains(go_output, "Bids []Level");
      check_str_contains(go_output, "Symbol string");
      check_str_contains(go_output, "Digest []byte");

      check_str_contains(py_output, "bids: list[Level]");
      check_str_contains(py_output, "symbol: str");
      check_str_contains(py_output, "digest: bytes");

      check_str_contains(rust_output, "#[repr(u8)]");
      check_str_contains(rust_output, "pub bids: Vec<Level>");
      check_str_contains(rust_output, "pub symbol: String");
      check_str_contains(rust_output, "pub digest: Vec<u8>");

      check_str_contains(ts_output, "export enum Side");
      check_str_contains(ts_output, "bids: Array<Level>;");
      check_str_contains(ts_output, "symbol: string;");
      check_str_contains(ts_output, "digest: Uint8Array;");

      free(cpp_output);
      free(go_output);
      free(py_output);
      free(rust_output);
      free(ts_output);
    }

    it("should render implicit enum values and variable bytes safely") {
      const char *schema = "enum Color { Red; Green = 5; Blue; } "
                           "message Blob { bytes(16) digest; bytes payload; }";
      char *output = render_c_template(schema);

      check_not_null(output);
      check_str_contains(output, "Color_Red = 0,");
      check_str_contains(output, "Color_Green = 5,");
      check_str_contains(output, "Color_Blue = 6,");
      check_str_contains(output, "bytes payload;");
      check_str_contains(output, "uint8_t digest[16];");
      check_str_contains(output, "typedef struct Blob_builder_s {");
      check_str_contains(output, "static inline bool Blob_builder_bind");
      check_str_contains(output, "static inline bool Blob_payload_set(");
      check_str_contains(output, "return tbe_wire_write_var_data(view->data + payload_offset,");
      check_str_contains(output, "static inline bool Blob_payload(");
      check_str_contains(output, "tbe_var_data_t *value");
      check_str_contains(output, "return tbe_wire_read_var_data(view->data + payload_offset,");
      check_str_contains(output, "return tbe_wire_read_var_data(view->data + payload_offset,");
      check(strstr(output, "uint8_t payload[") == NULL);

      free(output);
    }

    it("should render typed nested composite view and builder accessors") {
      const char *schema = "composite Header { uint32 seq_num; uint64 timestamp; } "
                           "message Envelope { uint32 channel; Header header; }";
      char *output = render_c_template(schema);

      check_not_null(output);
      check_str_contains(output, "typedef struct Header_builder_s {");
      check_str_contains(output, "typedef struct Envelope_builder_s {");
      check_str_contains(output, "static inline bool Envelope_builder_bind");
      check_str_contains(output, "static inline bool Envelope_channel_set");
      check_str_contains(output, "enum { Envelope_header_OFFSET = 4 };");
      check_str_contains(output, "static inline bool Envelope_header(");
      check_str_contains(output, "Header_view_t *value");
      check_str_contains(output, "return Header_view_bind(value, view->data + 4, view->size - 4);");
      check_str_contains(output, "static inline bool Envelope_header_builder(");
      check_str_contains(output, "Header_builder_t *value");
      check_str_contains(output,
                         "return Header_builder_bind(value, view->data + 4, view->size - 4);");
      check_str_contains(output, "static inline const uint8_t *Envelope_header_ptr");

      free(output);
    }

    it("should keep enum fields fixed-size and generate enum writers") {
      const char *schema = "enum Side <uint8> { Buy = 1; Sell = 2; } "
                           "message Quote { Side side; uint32 qty; }";
      char *output = render_c_template(schema);

      check_not_null(output);
      check_str_contains(output, "typedef struct Quote_builder_s {");
      check_str_contains(output, "static inline bool Quote_builder_bind");
      check_str_contains(output, "enum { Quote_BLOCK_LENGTH = 5 };");
      check_str_contains(output, "enum { Quote_side_OFFSET = 0 };");
      check_str_contains(output, "enum { Quote_qty_OFFSET = 1 };");
      check_str_contains(output, "Side_t side;");
      check_str_contains(output, "static inline Side_t Quote_side_get");
      check_str_contains(output, "static inline bool Quote_side_set");
      check_str_contains(
          output,
          "tbe_wire_write_u8(view->data + 0, GeneratedSchema_WIRE_BIG_ENDIAN, (uint8_t)value);");
      check_str_contains(output, "static inline bool Quote_qty_set");
      check_str_contains(
          output, "tbe_wire_write_u32(view->data + 1, GeneratedSchema_WIRE_BIG_ENDIAN, value);");
      check_str_contains(output, "return (Side_t)tbe_wire_read_u8(view->data + 0,");
      check_str_contains(output, "GeneratedSchema_WIRE_BIG_ENDIAN");
      check(strstr(output, "Side_view_t") == NULL);

      free(output);
    }

    it("should render fixed bytes and fixed array writers safely") {
      const char *schema = "enum Side <uint8> { Buy = 1; Sell = 2; } "
                           "composite Point { int32 x; int32 y; } "
                           "message Payloads { bytes(16) digest; Point[2] points; uint32[4] "
                           "values; Side[2] sides; }";
      char *output = render_c_template(schema);

      check_not_null(output);
      check_str_contains(output, "enum { Payloads_digest_OFFSET = 0 };");
      check_str_contains(output, "enum { Payloads_points_OFFSET = 16 };");
      check_str_contains(output, "enum { Payloads_values_OFFSET = 32 };");
      check_str_contains(output, "enum { Payloads_sides_OFFSET = 48 };");
      check_str_contains(output, "static inline bool Payloads_digest_set(");
      check_str_contains(output, "size != 16");
      check_str_contains(output, "memcpy(view->data + 0, data, 16);");
      check_str_contains(output, "static inline bool Payloads_points_builder_at(");
      check_str_contains(output, "Point_builder_t *value");
      check_str_contains(output, "index >= 2");
      check_str_contains(output, "element_offset = 16 + ((size_t)index * 8);");
      check_str_contains(output, "return Point_builder_bind(value, view->data + element_offset, "
                                 "view->size - element_offset);");
      check_str_contains(output, "static inline bool Payloads_values_set_at(");
      check_str_contains(output, "index >= 4");
      check_str_contains(output, "tbe_wire_write_u32(");
      check_str_contains(output, "view->data + 32 + ((size_t)index * 4),");
      check_str_contains(output, "GeneratedSchema_WIRE_BIG_ENDIAN, value);");
      check_str_contains(output, "static inline bool Payloads_sides_set_at(");
      check_str_contains(output, "tbe_wire_write_u8(");
      check_str_contains(output, "view->data + 48 + ((size_t)index * 1),");
      check_str_contains(output, "(uint8_t)value);");

      free(output);
    }

    it("should render schema composites groups and messages") {
      const char *schema = "schema Market [id(7), version(2), byte_order(little)]; "
                           "composite Header { uint32 seq_num; uint64 timestamp; } "
                           "group Level { uint64 price; uint32 qty; } "
                           "[id(100), version(1)] message BookSnapshot { "
                           "Header header; "
                           "group<Level> bids; "
                           "string symbol; "
                           "bytes source; }";
      char *output = render_c_template(schema);

      check_not_null(output);
      check_str_contains(output, "typedef struct Header_s {");
      check_str_contains(output, "typedef struct Level_s {");
      check_str_contains(output, "typedef struct BookSnapshot_s {");
      check_str_contains(output, "#include \"tbe_wire.h\"");
      check_str_contains(output, "enum { Market_WIRE_BIG_ENDIAN = 0 };");
      check_str_contains(output, "Header_t header;");
      check_str_contains(output, "list<Level> bids;");
      check_str_contains(output, "enum { Header_BLOCK_LENGTH = 12 };");
      check_str_contains(output, "enum { BookSnapshot_BLOCK_LENGTH = 12 };");
      check_str_contains(output, "enum { BookSnapshot_header_OFFSET = 0 };");
      check_str_contains(output, "typedef struct Level_cursor_s {");
      check_str_contains(output, "static inline bool Level_cursor_bind");
      check_str_contains(
          output,
          "cursor->block_length = tbe_wire_read_u16(cursor->data, Market_WIRE_BIG_ENDIAN);");
      check_str_contains(output, "static inline bool Level_cursor_get");
      check_str_contains(output, "typedef struct BookSnapshot_view_s {");
      check_str_contains(output, "static inline bool BookSnapshot_view_bind");
      check_str_contains(output, "typedef struct BookSnapshot_builder_s {");
      check_str_contains(output, "static inline bool BookSnapshot_builder_bind");
      check_str_contains(output, "static inline const uint8_t *BookSnapshot_header_ptr");
      check_str_contains(output, "static inline bool BookSnapshot_bids_cursor");
      check_str_contains(output, "static inline bool BookSnapshot_symbol(");
      check_str_contains(output, "return tbe_wire_read_var_data(payload_data,");
      check_str_contains(output, "static inline bool BookSnapshot_symbol_set(");
      check_str_contains(output, "BookSnapshot_view_t read_view;");
      check_str_contains(output, "if (!BookSnapshot_bids_cursor(&read_view, &previous)) {");
      check_str_contains(output, "return tbe_wire_write_var_data(");
      check_str_contains(output, "static inline bool BookSnapshot_source(");
      check_str_contains(output, "tbe_wire_var_data_end(&previous);");
      check_str_contains(output, "static inline bool BookSnapshot_source_set(");
      check_str_contains(output, "if (!BookSnapshot_symbol(&read_view, &previous)) {");
      check_str_contains(output, "return Level_cursor_bind(cursor, view->data + group_offset, "
                                 "view->size - group_offset);");
      check_str_contains(output, "if (!BookSnapshot_symbol(view, &previous)) {");
      check_str_contains(output, "payload_data = tbe_wire_var_data_end(&previous);");
      check_str_contains(output, "static inline uint32_t Header_seq_num_get");
      check_str_contains(output,
                         "return tbe_wire_read_u32(view->data + 0, Market_WIRE_BIG_ENDIAN);");

      free(output);
    }
  }
}
