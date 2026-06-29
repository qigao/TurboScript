#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Node *find_child(Node *parent, const char *name) {
  if (!parent || !name) return NULL;

  if (parent->type == NODE_MAP) {
    for (size_t i = 0; i < parent->data.map.count; i++) {
      Node *child = parent->data.map.items[i];
      if (child->name && strcmp(child->name, name) == 0) return child;
    }
  } else if (parent->type == NODE_LIST) {
    for (size_t i = 0; i < parent->data.list.count; i++) {
      Node *child = parent->data.list.items[i];
      if (child->name && strcmp(child->name, name) == 0) return child;
    }
  }
  return NULL;
}

suite("tbe_parser") {
  describe("Schema Parsing") {
    it("should parse simple composites") {
      const char *schema = "composite Point { int32 x; int32 y; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *composites = NULL;
      for (size_t i = 0; i < root->data.map.count; i++) {
        if (strcmp(root->data.map.items[i]->name, "composites") == 0) {
          composites = root->data.map.items[i];
          break;
        }
      }
      check_not_null(composites);
      check_uint_eq(composites->data.list.count, 1);

      Node *point = composites->data.list.items[0];
      check_not_null(point);

      node_free(root);
    }

    it("should parse enums with underlying types") {
      const char *schema = "enum Color <uint8> { Red = 1; Green = 2; Blue = 3; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *enums = NULL;
      for (size_t i = 0; i < root->data.map.count; i++) {
        if (strcmp(root->data.map.items[i]->name, "enums") == 0) {
          enums = root->data.map.items[i];
          break;
        }
      }
      check_not_null(enums);
      check_uint_eq(enums->data.list.count, 1);

      Node *color = enums->data.list.items[0];
      // Check underlying type
      Node *utype = NULL;
      for (size_t i = 0; i < color->data.map.count; i++) {
        if (color->data.map.items[i]->name &&
            strcmp(color->data.map.items[i]->name, "underlying_type") == 0) {
          utype = color->data.map.items[i];
          break;
        }
      }
      check_not_null(utype);
      check_str_eq(utype->data.string_val, "uint8");

      node_free(root);
    }

    it("should parse top-level attributes [id(x)]") {
      const char *schema = "[id(100)] message Message { int32 code; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *messages = NULL;
      for (size_t i = 0; i < root->data.map.count; i++) {
        if (strcmp(root->data.map.items[i]->name, "messages") == 0) {
          messages = root->data.map.items[i];
          break;
        }
      }
      Node *msg = messages->data.list.items[0];

      Node *attrs = NULL;
      for (size_t i = 0; i < msg->data.map.count; i++) {
        if (msg->data.map.items[i]->name &&
            strcmp(msg->data.map.items[i]->name, "attributes") == 0) {
          attrs = msg->data.map.items[i];
          break;
        }
      }
      check_not_null(attrs);

      Node *id_node = find_child(attrs, "id");
      check_not_null(id_node);
      Node *id_val = find_child(id_node, "value");
      check_not_null(id_val);
      check_str_eq(id_val->data.string_val, "100");

      node_free(root);
    }

    it("should extract rich metadata") {
      const char *schema = "composite Data { uint32 u32; int64 i64; float f32; double d64; byte b; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);
      Node *composites = find_child(root, "composites");
      Node *data = composites->data.list.items[0];
      Node *fields = find_child(data, "fields");

      // Verify uint32_t (aliased from uint32)
      Node *u32 = fields->data.list.items[0];
      check_str_eq(find_child(u32, "size_bytes")->data.string_val, "4");
      check_str_eq(find_child(u32, "is_unsigned")->data.string_val, "1");
      check_str_eq(find_child(u32, "is_numeric")->data.string_val, "1");

      // Verify int64_t (aliased from int64)
      Node *i64 = fields->data.list.items[1];
      check_str_eq(find_child(i64, "size_bytes")->data.string_val, "8");
      check_null(find_child(i64, "is_unsigned"));
      check_str_eq(find_child(i64, "is_numeric")->data.string_val, "1");

      // Verify float
      Node *f32 = fields->data.list.items[2];
      check_str_eq(find_child(f32, "size_bytes")->data.string_val, "4");
      check_str_eq(find_child(f32, "is_float")->data.string_val, "1");

      node_free(root);
    }

    it("should handle fixed arrays and variable data in TBE records") {
      const char *schema =
          "composite Point { int32 x; int32 y; } "
          "message Collections { Point[10] points; bytes(16) digest; bytes payload; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);
      Node *messages = find_child(root, "messages");
      Node *coll = messages->data.list.items[0];
      Node *fields = find_child(coll, "fields");

      // Point[10]
      Node *points = fields->data.list.items[0];
      check_str_eq(find_child(points, "ctype")->data.string_val, "COLLECTION");
      check_str_eq(find_child(points, "inner_type")->data.string_val, "Point");
      check_str_eq(find_child(points, "length_field")->data.string_val, "10");
      check_str_eq(find_child(points, "field_size_bytes")->data.string_val, "80");
      check_str_eq(find_child(points, "element_size_bytes")->data.string_val, "8");
      check_str_eq(find_child(points, "collection_element_is_composite")->data.string_val, "1");

      Node *digest = fields->data.list.items[1];
      check_str_eq(find_child(digest, "is_fixed_size")->data.string_val, "1");
      check_str_eq(find_child(digest, "size_bytes")->data.string_val, "16");

      Node *payload = fields->data.list.items[2];
      check_str_eq(find_child(payload, "is_variable_size")->data.string_val, "1");
      check_str_eq(find_child(payload, "is_var_data")->data.string_val, "1");

      node_free(root);
    }

    it("should classify fixed array element types for safe writers") {
      const char *schema =
          "enum Side <uint8> { Buy = 1; Sell = 2; } "
          "composite Point { int32 x; int32 y; } "
          "message Payloads { uint32[4] values; Side[2] sides; Point[2] points; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *messages = find_child(root, "messages");
      Node *payloads = messages->data.list.items[0];
      Node *fields = find_child(payloads, "fields");
      Node *values = fields->data.list.items[0];
      Node *sides = fields->data.list.items[1];
      Node *points = fields->data.list.items[2];

      check_str_eq(find_child(values, "element_size_bytes")->data.string_val, "4");
      check_str_eq(find_child(values, "collection_element_is_primitive")->data.string_val, "1");
      check_str_eq(find_child(values, "collection_element_host_type")->data.string_val, "uint32_t");
      check_str_eq(find_child(values, "collection_element_wire_reader")->data.string_val, "u32");

      check_str_eq(find_child(sides, "element_size_bytes")->data.string_val, "1");
      check_str_eq(find_child(sides, "collection_element_is_enum")->data.string_val, "1");
      check_str_eq(find_child(sides, "collection_element_enum_c_type")->data.string_val, "Side_t");
      check_str_eq(find_child(sides, "collection_element_host_type")->data.string_val, "uint8_t");
      check_str_eq(find_child(sides, "collection_element_wire_reader")->data.string_val, "u8");

      check_str_eq(find_child(points, "element_size_bytes")->data.string_val, "8");
      check_str_eq(find_child(points, "collection_element_is_composite")->data.string_val, "1");

      node_free(root);
    }

    it("should handle complex schemas with multiple declarations") {
      const char *schema = "enum State { IDLE = 0; ACTIVE = 1; } "
                           "composite Meta { int32 version; } "
                           "message Message { Meta meta; State state; string body; }";

      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);
      Node *composites = find_child(root, "composites");
      Node *messages = find_child(root, "messages");
      Node *enums = find_child(root, "enums");

      check_uint_eq(composites->data.list.count, 1);
      check_uint_eq(messages->data.list.count, 1);
      check_uint_eq(enums->data.list.count, 1);

      Node *state_enum = enums->data.list.items[0];
      check_str_eq(find_child(state_enum, "enum_name")->data.string_val, "State");

      Node *msg_struct = messages->data.list.items[0];
      check_str_eq(find_child(msg_struct, "message_name")->data.string_val, "Message");
      Node *msg_fields = find_child(msg_struct, "fields");
      check_uint_eq(msg_fields->data.list.count, 3);

      node_free(root);
    }

    it("should treat enum fields as fixed-size in record layouts") {
      const char *schema = "enum Side <uint8> { Buy = 1; Sell = 2; } "
                           "message Quote { Side side; uint32 qty; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *messages = find_child(root, "messages");
      Node *quote = messages->data.list.items[0];
      Node *fields = find_child(quote, "fields");
      Node *side = fields->data.list.items[0];
      Node *qty = fields->data.list.items[1];

      check_str_eq(find_child(quote, "fixed_block_size")->data.string_val, "5");
      check_str_eq(find_child(side, "is_enum_ref")->data.string_val, "1");
      check_null(find_child(side, "is_composite_ref"));
      check_str_eq(find_child(side, "offset")->data.string_val, "0");
      check_str_eq(find_child(side, "field_size_bytes")->data.string_val, "1");
      check_str_eq(find_child(qty, "offset")->data.string_val, "1");
      check_str_eq(find_child(qty, "field_size_bytes")->data.string_val, "4");

      node_free(root);
    }

    it("should parse enum attributes and fixed-size bytes") {
      const char *schema = "[id(1), version(100)] enum MessageType <uint8> { LoginRequest = 1; } "
                           "message LoginMessage { bytes(16) pass_hash; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      // Verify enum attributes
      Node *enums = find_child(root, "enums");
      Node *mt = enums->data.list.items[0];
      Node *mt_attrs = find_child(mt, "attributes");
      check_not_null(mt_attrs);

      Node *mt_id = find_child(mt_attrs, "id");
      check_not_null(mt_id);
      check_str_eq(find_child(mt_id, "name")->data.string_val, "id");
      check_str_eq(find_child(mt_id, "value")->data.string_val, "1");

      // Verify fixed-size bytes
      Node *messages = find_child(root, "messages");
      Node *lm = messages->data.list.items[0];
      Node *fields = find_child(lm, "fields");
      Node *ph = fields->data.list.items[0];

      check_str_eq(find_child(ph, "is_bytes")->data.string_val, "1");
      check_str_eq(find_child(ph, "size_bytes")->data.string_val, "16");
      check_str_eq(find_child(ph, "is_fixed_size")->data.string_val, "1");

      node_free(root);
    }

    it("should handle fixed-size arrays with numeric length") {
      const char *schema =
          "composite Point { int32 x; int32 y; } composite Poly { Point[4] vertices; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);
      Node *composites = find_child(root, "composites");
      Node *poly = composites->data.list.items[1];
      Node *fields = find_child(poly, "fields");
      Node *v = fields->data.list.items[0];

      check_str_eq(find_child(v, "ctype")->data.string_val, "COLLECTION");
      check_str_eq(find_child(v, "inner_type")->data.string_val, "Point");
      check_str_eq(find_child(v, "length_field")->data.string_val, "4");

      node_free(root);
    }

    it("should retain field attributes and use declaration order") {
      const char *schema = "message LoginMessage { "
                           "[id(1)] Header header; "
                           "string username; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);
      Node *messages = find_child(root, "messages");
      Node *msg = messages->data.list.items[0];
      Node *fields = find_child(msg, "fields");
      Node *header = fields->data.list.items[0];
      Node *attrs = find_child(header, "attributes");
      Node *id = attrs->data.list.items[0];
      check_str_eq(find_child(id, "name")->data.string_val, "id");
      check_str_eq(find_child(id, "value")->data.string_val, "1");
      node_free(root);
    }

    it("should assign values to enum items without explicit values") {
      const char *schema = "enum Color { Red; Green = 5; Blue; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *enums = find_child(root, "enums");
      Node *color = enums->data.list.items[0];
      Node *items = find_child(color, "items");

      check_uint_eq(items->data.list.count, 3);
      check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "0");
      check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "5");
      check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "6");

      node_free(root);
    }

    it("should treat bare bytes fields as variable size") {
      const char *schema = "message Blob { bytes payload; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *messages = find_child(root, "messages");
      Node *blob = messages->data.list.items[0];
      Node *fields = find_child(blob, "fields");
      Node *payload = fields->data.list.items[0];

      check_str_eq(find_child(payload, "is_bytes")->data.string_val, "1");
      check_str_eq(find_child(payload, "is_variable_size")->data.string_val, "1");
      check_str_eq(find_child(payload, "var_data_accessor_accessible")->data.string_val, "1");
      check_str_eq(find_child(payload, "is_first_var_data_field")->data.string_val, "1");
      check_str_eq(find_child(payload, "var_data_from_block_length")->data.string_val, "1");
      check_null(find_child(payload, "is_fixed_size"));
      check_null(find_child(payload, "size_bytes"));

      node_free(root);
    }

    it("should parse dynamic list set and map container metadata") {
      const char *schema = "message Containers { list<uint32> values; set<string> tags; map<string,int32> attrs; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *messages = find_child(root, "messages");
      Node *containers = messages->data.list.items[0];
      Node *fields = find_child(containers, "fields");
      Node *values = fields->data.list.items[0];
      Node *tags = fields->data.list.items[1];
      Node *attrs = fields->data.list.items[2];

      check_str_eq(find_child(values, "collection_kind")->data.string_val, "list");
      check_str_eq(find_child(values, "inner_type")->data.string_val, "uint32");
      check_str_eq(find_child(values, "is_variable_size")->data.string_val, "1");

      check_str_eq(find_child(tags, "collection_kind")->data.string_val, "set");
      check_str_eq(find_child(tags, "inner_type")->data.string_val, "string");
      check_str_eq(find_child(tags, "is_set")->data.string_val, "1");
      check_str_eq(find_child(tags, "is_variable_size")->data.string_val, "1");

      check_str_eq(find_child(attrs, "collection_kind")->data.string_val, "map");
      check_str_eq(find_child(attrs, "key_type")->data.string_val, "string");
      check_str_eq(find_child(attrs, "value_type")->data.string_val, "int32");
      check_str_eq(find_child(attrs, "inner_type")->data.string_val, "int32");
      check_str_eq(find_child(attrs, "is_map")->data.string_val, "1");
      check_str_eq(find_child(attrs, "is_variable_size")->data.string_val, "1");

      node_free(root);
    }

    it("should not mutate root on parse failure") {
      const char *schema = "message Bad { int32 missing_semi }";
      Node *root = create_node_map("root");
      map_add(root, create_node_string("marker", "keep"));
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, -1);
      check_uint_eq(root->data.map.count, 1);
      check_str_eq(find_child(root, "marker")->data.string_val, "keep");
      check_null(find_child(root, "enums"));
      check_null(find_child(root, "messages"));

      node_free(root);
    }

    it("should replace generated top-level lists on repeated parses") {
      Node *root = create_node_map("root");
      map_add(root, create_node_string("marker", "keep"));

      check_int_eq(parse_schema("composite First { int32 x; }",
                                strlen("composite First { int32 x; }"), root, NULL),
                   0);
      check_int_eq(parse_schema("enum State { Idle = 1; } message Second { int32 y; }",
                                strlen("enum State { Idle = 1; } message Second { int32 y; }"),
                                root, NULL),
                   0);

      Node *messages = find_child(root, "messages");
      Node *composites = find_child(root, "composites");
      Node *enums = find_child(root, "enums");
      Node *groups = find_child(root, "groups");
      Node *unions = find_child(root, "unions");

      check_uint_eq(root->data.map.count, 6);
      check_not_null(find_child(root, "marker"));
      check_not_null(messages);
      check_not_null(composites);
      check_not_null(enums);
      check_not_null(groups);
      check_not_null(unions);
      check_uint_eq(messages->data.list.count, 1);
      check_uint_eq(composites->data.list.count, 0);
      check_uint_eq(enums->data.list.count, 1);
      check_uint_eq(groups->data.list.count, 0);
      check_uint_eq(unions->data.list.count, 0);
      check_str_eq(find_child(messages->data.list.items[0], "message_name")->data.string_val,
                   "Second");
      check_str_eq(find_child(enums->data.list.items[0], "enum_name")->data.string_val, "State");

      node_free(root);
    }

    it("should reject legacy struct declarations") {
      Node *root = create_node_map("root");
      int rc = parse_schema("struct Point { int32 x; int32 y; }",
                            strlen("struct Point { int32 x; int32 y; }"), root, NULL);

      check_int_eq(rc, -1);
      node_free(root);
    }

    it("should parse schema composite group and message declarations") {
      const char *schema = "schema Market [id(7), version(2), byte_order(little)]; "
                           "enum Side <uint8> { Buy = 1; Sell = 2; } "
                           "composite Header { uint32 seq_num; uint64 timestamp; } "
                           "group Level { uint64 price; uint32 qty; } "
                           "[id(100), version(1)] message BookSnapshot { "
                           "Header header; "
                           "uint8[16] digest; "
                           "group<Level> bids; "
                           "string symbol; "
                           "bytes source; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);

      Node *schema_node = find_child(root, "schema");
      Node *schema_attrs = find_child(schema_node, "attributes");
      Node *composites = find_child(root, "composites");
      Node *groups = find_child(root, "groups");
      Node *messages = find_child(root, "messages");

      check_not_null(schema_node);
      check_str_eq(find_child(schema_node, "schema_name")->data.string_val, "Market");

      check_not_null(schema_attrs);
      Node *byte_order_node = find_child(schema_attrs, "byte_order");
      check_not_null(byte_order_node);
      Node *byte_order_value = find_child(byte_order_node, "value");
      check_not_null(byte_order_value);
      check_str_eq(byte_order_value->data.string_val, "little");

      check_not_null(composites);
      check_not_null(groups);
      check_not_null(messages);
      check_uint_eq(composites->data.list.count, 1);
      check_uint_eq(groups->data.list.count, 1);
      check_uint_eq(messages->data.list.count, 1);

      Node *header = composites->data.list.items[0];
      Node *level = groups->data.list.items[0];
      Node *snapshot = messages->data.list.items[0];
      Node *snapshot_fields = find_child(snapshot, "fields");

      check_str_eq(find_child(header, "composite_name")->data.string_val, "Header");
      check_str_eq(find_child(level, "group_name")->data.string_val, "Level");
      check_str_eq(find_child(snapshot, "message_name")->data.string_val, "BookSnapshot");
      check_str_eq(find_child(header, "fixed_block_size")->data.string_val, "12");
      check_str_eq(find_child(level, "fixed_block_size")->data.string_val, "12");
      check_str_eq(find_child(level, "supports_group_cursor")->data.string_val, "1");
      check_str_eq(find_child(snapshot, "fixed_block_size")->data.string_val, "28");
      check_uint_eq(snapshot_fields->data.list.count, 5);

      check_str_eq(
          find_child(snapshot_fields->data.list.items[0], "is_fixed_block")->data.string_val, "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[0], "offset")->data.string_val, "0");
      check_str_eq(find_child(snapshot_fields->data.list.items[0], "field_size_bytes")->data.string_val,
                   "12");
      check_str_eq(
          find_child(snapshot_fields->data.list.items[1], "is_fixed_block")->data.string_val, "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[1], "offset")->data.string_val, "12");
      check_str_eq(find_child(snapshot_fields->data.list.items[1], "length_field")->data.string_val,
                   "16");
      check_str_eq(find_child(snapshot_fields->data.list.items[1], "field_size_bytes")->data.string_val,
                   "16");
      check_str_eq(
          find_child(snapshot_fields->data.list.items[2], "is_group_field")->data.string_val, "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[2], "supports_group_cursor")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[2], "group_cursor_accessible")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[2], "is_first_group_field")->data.string_val,
                   "1");
      check_null(find_child(snapshot_fields->data.list.items[2], "offset"));
      check_str_eq(find_child(snapshot_fields->data.list.items[2], "group_type")->data.string_val,
                   "Level");
      check_str_eq(find_child(snapshot_fields->data.list.items[3], "is_var_data")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[3], "var_data_accessor_accessible")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[3], "is_first_var_data_field")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[3], "var_data_from_previous_group")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[3], "previous_group_field_name")->data.string_val,
                   "bids");
      check_str_eq(find_child(snapshot_fields->data.list.items[3], "previous_group_type")->data.string_val,
                   "Level");
      check_null(find_child(snapshot_fields->data.list.items[3], "offset"));
      check_str_eq(find_child(snapshot_fields->data.list.items[4], "is_var_data")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[4], "var_data_accessor_accessible")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[4], "var_data_from_previous_var_data")->data.string_val,
                   "1");
      check_str_eq(find_child(snapshot_fields->data.list.items[4], "previous_var_data_field_name")->data.string_val,
                   "symbol");
      check_null(find_child(snapshot_fields->data.list.items[4], "offset"));

      node_free(root);
    }

    it("should reject variable-sized fields inside composites") {
      const char *schema = "composite Header { string symbol; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, -1);
      node_free(root);
    }

    it("should parse message fields declared outside binary layout order") {
      const char *schema = "group Level { uint64 price; } "
                           "message Broken { string symbol; group<Level> bids; }";
      Node *root = create_node_map("root");
      int rc = parse_schema(schema, strlen(schema), root, NULL);

      check_int_eq(rc, 0);
      node_free(root);
    }
  }

  describe("Flags Support") {
    it("should parse flags without underlying type") {
      const char *schema = "flags Permissions { Read; Write; Execute; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, strlen(schema), root, &err);

      check_int_eq(rc, 0);
      Node *enums = find_child(root, "enums");
      check_not_null(enums);
      check_uint_eq(enums->data.list.count, 1);

      Node *perms = enums->data.list.items[0];
      check_str_eq(find_child(perms, "enum_name")->data.string_val, "Permissions");
      check_str_eq(find_child(perms, "is_flags")->data.string_val, "1");

      Node *items = find_child(perms, "items");
      check_uint_eq(items->data.list.count, 3);
      check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
      check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
      check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "4");

      node_free(root);
    }

    it("should parse flags with underlying type") {
      const char *schema = "flags OrderFlags <uint8> { IOC = 1; FOK = 2; PostOnly = 4; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, strlen(schema), root, &err);

      check_int_eq(rc, 0);
      Node *enums = find_child(root, "enums");
      Node *flags = enums->data.list.items[0];

      check_str_eq(find_child(flags, "enum_name")->data.string_val, "OrderFlags");
      check_str_eq(find_child(flags, "underlying_type")->data.string_val, "uint8");
      check_str_eq(find_child(flags, "is_flags")->data.string_val, "1");

      Node *items = find_child(flags, "items");
      check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
      check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
      check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "4");

      node_free(root);
    }

    it("should auto-increment flags as powers of 2") {
      const char *schema = "flags Status { Active; Pending; Completed; Cancelled; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, strlen(schema), root, &err);

      check_int_eq(rc, 0);
      Node *enums = find_child(root, "enums");
      Node *status = enums->data.list.items[0];
      Node *items = find_child(status, "items");

      check_uint_eq(items->data.list.count, 4);
      check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
      check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
      check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "4");
      check_str_eq(find_child(items->data.list.items[3], "value")->data.string_val, "8");

      node_free(root);
    }

    it("should handle mixed explicit and auto values in flags") {
      const char *schema = "flags Mixed { A = 1; B; C = 16; D; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, strlen(schema), root, &err);

      check_int_eq(rc, 0);
      Node *enums = find_child(root, "enums");
      Node *mixed = enums->data.list.items[0];
      Node *items = find_child(mixed, "items");

      check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
      check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
      check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "16");
      check_str_eq(find_child(items->data.list.items[3], "value")->data.string_val, "32");

      node_free(root);
    }
  }

  describe("Error Handling") {
    it("should report detailed error for lexer errors") {
      const char *schema = "composite Point { int32 x; @invalid }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, strlen(schema), root, &err);

      check_int_eq(rc, -1);
      check_int_ne(err.code, TBE_OK);
      check_int_gt(err.line, 0);
      node_free(root);
    }

    it("should report error for invalid arguments") {
      tbe_error_t err;
      int rc = parse_schema(NULL, 0, NULL, &err);

      check_int_eq(rc, -1);
      check_int_eq(err.code, TBE_ERR_INVALID_ARGUMENT);
      node_free(NULL);  // Should handle NULL gracefully
    }

    it("should handle empty schema") {
      const char *schema = "";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, 0, root, &err);

      check_int_eq(rc, 0);
      node_free(root);
    }

    it("should handle whitespace-only schema") {
      const char *schema = "   \n\t  \n  ";
      Node *root = create_node_map("root");
      tbe_error_t err;

      int rc = parse_schema(schema, strlen(schema), root, &err);

      check_int_eq(rc, 0);
      node_free(root);
    }
  }

  describe("Memory Safety") {
    it("should handle NULL in create_node_string value") {
      Node *n = create_node_string("test", NULL);
      check_null(n);
    }

    it("should handle NULL in node_free") {
      node_free(NULL);  // Should not crash
    }

    it("should properly free nested structures") {
      Node *root = create_node_map("root");
      Node *list = create_node_list("items");

      if (root && list) {
        list_add(list, create_node_string("item1", "value1"));
        list_add(list, create_node_string("item2", "value2"));
        map_add(root, list);
      }

      node_free(root);  // Should free everything
    }
  }
}
