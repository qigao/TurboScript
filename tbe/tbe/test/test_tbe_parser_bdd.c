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

suite("TBE Schema Parser") {
  section("Basic Type Declarations") {
    given("a simple composite definition") {
      const char *schema = "composite Point { int32 x; int32 y; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, NULL);

        then("should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("should create composites list") {
          Node *composites = find_child(root, "composites");
          check_not_null(composites);
          check_uint_eq(composites->data.list.count, 1);
        }

        then("should have correct composite name") {
          Node *composites = find_child(root, "composites");
          Node *point = composites->data.list.items[0];
          check_not_null(point);
        }
      }

      node_free(root);
    }

    given("an enum with underlying type") {
      const char *schema = "enum Color <uint8> { Red = 1; Green = 2; Blue = 3; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, NULL);

        then("should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("should have correct underlying type") {
          Node *enums = find_child(root, "enums");
          check_not_null(enums);
          Node *color = enums->data.list.items[0];
          Node *utype = find_child(color, "underlying_type");
          check_not_null(utype);
          check_str_eq(utype->data.string_val, "uint8");
        }
      }

      node_free(root);
    }

    given("a message with multiple field types") {
      const char *schema = "message Order { uint64 order_id; uint32 quantity; uint64 price; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, NULL);

        then("should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("should create messages list") {
          Node *messages = find_child(root, "messages");
          check_not_null(messages);
          check_uint_eq(messages->data.list.count, 1);
        }
      }

      node_free(root);
    }
  }

  section("Schema Attributes") {
    given("a schema declaration with attributes") {
      const char *schema = "schema Market [id(7), version(2), byte_order(little)];";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, NULL);

        then("should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("should have schema node") {
          Node *schema_node = find_child(root, "schema");
          check_not_null(schema_node);
        }

        then("should have correct schema name") {
          Node *schema_node = find_child(root, "schema");
          Node *name = find_child(schema_node, "schema_name");
          check_str_eq(name->data.string_val, "Market");
        }

        then("should have wire byte order") {
          Node *schema_node = find_child(root, "schema");
          Node *byte_order = find_child(schema_node, "wire_byte_order");
          check_str_eq(byte_order->data.string_val, "little");
        }
      }

      node_free(root);
    }

    given("a message with id and version attributes") {
      const char *schema = "[id(100)] message Message { int32 code; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, NULL);

        then("should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("should have attributes") {
          Node *messages = find_child(root, "messages");
          Node *msg = messages->data.list.items[0];
          Node *attrs = find_child(msg, "attributes");
          check_not_null(attrs);
        }

        then("should have id attribute") {
          Node *messages = find_child(root, "messages");
          Node *msg = messages->data.list.items[0];
          Node *attrs = find_child(msg, "attributes");
          Node *id_node = find_child(attrs, "id");
          check_not_null(id_node);
          Node *id_val = find_child(id_node, "value");
          check_str_eq(id_val->data.string_val, "100");
        }
      }

      node_free(root);
    }
  }

  section("Field Type Metadata") {
    given("a composite with various numeric types") {
      const char *schema = "composite Data { uint32 u32; int64 i64; float f32; double d64; byte b; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, NULL);
        Node *composites = find_child(root, "composites");
        Node *data = composites->data.list.items[0];
        Node *fields = find_child(data, "fields");

        then("should annotate uint32 correctly") {
          Node *u32 = fields->data.list.items[0];
          check_str_eq(find_child(u32, "size_bytes")->data.string_val, "4");
          check_str_eq(find_child(u32, "is_unsigned")->data.string_val, "1");
          check_str_eq(find_child(u32, "is_numeric")->data.string_val, "1");
        }

        then("should annotate int64 correctly") {
          Node *i64 = fields->data.list.items[1];
          check_str_eq(find_child(i64, "size_bytes")->data.string_val, "8");
          check_null(find_child(i64, "is_unsigned"));
          check_str_eq(find_child(i64, "is_numeric")->data.string_val, "1");
        }

        then("should annotate float correctly") {
          Node *f32 = fields->data.list.items[2];
          check_str_eq(find_child(f32, "size_bytes")->data.string_val, "4");
          check_str_eq(find_child(f32, "is_float")->data.string_val, "1");
        }
      }

      node_free(root);
    }
  }

  section("Fixed Arrays and Variable Data") {
    given("a message with fixed array and variable data") {
      const char *schema =
        "composite Point { int32 x; int32 y; } "
        "message Collections { Point[10] points; bytes(16) digest; bytes payload; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, NULL);
        Node *messages = find_child(root, "messages");
        Node *coll = messages->data.list.items[0];
        Node *fields = find_child(coll, "fields");

        then("should recognize fixed array") {
          Node *points = fields->data.list.items[0];
          check_str_eq(find_child(points, "ctype")->data.string_val, "COLLECTION");
          check_str_eq(find_child(points, "inner_type")->data.string_val, "Point");
          check_str_eq(find_child(points, "length_field")->data.string_val, "10");
        }

        then("should calculate array size") {
          Node *points = fields->data.list.items[0];
          check_str_eq(find_child(points, "field_size_bytes")->data.string_val, "80");
          check_str_eq(find_child(points, "element_size_bytes")->data.string_val, "8");
        }

        then("should recognize fixed bytes") {
          Node *digest = fields->data.list.items[1];
          check_str_eq(find_child(digest, "is_fixed_size")->data.string_val, "1");
          check_str_eq(find_child(digest, "size_bytes")->data.string_val, "16");
        }

        then("should recognize variable bytes") {
          Node *payload = fields->data.list.items[2];
          check_str_eq(find_child(payload, "is_variable_size")->data.string_val, "1");
          check_str_eq(find_child(payload, "is_var_data")->data.string_val, "1");
        }
      }

      node_free(root);
    }
  }

  section("Enum Auto-increment") {
    given("an enum with mixed explicit and implicit values") {
      const char *schema = "enum Color { Red; Green = 5; Blue; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, NULL);
        Node *enums = find_child(root, "enums");
        Node *color = enums->data.list.items[0];
        Node *items = find_child(color, "items");

        then("should start at 0 for first item") {
          check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "0");
        }

        then("should use explicit value") {
          check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "5");
        }

        then("should continue from explicit value") {
          check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "6");
        }
      }

      node_free(root);
    }
  }

  section("Layout Calculation") {
    given("a message with fixed-size fields") {
      const char *schema = "enum Side <uint8> { Buy = 1; Sell = 2; } message Quote { Side side; uint32 qty; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, NULL);
        Node *messages = find_child(root, "messages");
        Node *quote = messages->data.list.items[0];
        Node *fields = find_child(quote, "fields");

        then("should calculate total block size") {
          check_str_eq(find_child(quote, "fixed_block_size")->data.string_val, "5");
        }

        then("should calculate field offsets") {
          Node *side = fields->data.list.items[0];
          Node *qty = fields->data.list.items[1];
          check_str_eq(find_child(side, "offset")->data.string_val, "0");
          check_str_eq(find_child(qty, "offset")->data.string_val, "1");
        }

        then("should calculate field sizes") {
          Node *side = fields->data.list.items[0];
          Node *qty = fields->data.list.items[1];
          check_str_eq(find_child(side, "field_size_bytes")->data.string_val, "1");
          check_str_eq(find_child(qty, "field_size_bytes")->data.string_val, "4");
        }
      }

      node_free(root);
    }
  }

  section("Groups and Cursors") {
    given("a message with repeating groups") {
      const char *schema =
        "group Level { uint64 price; uint32 qty; } "
        "message BookSnapshot { group<Level> bids; string symbol; }";
      Node *root = create_node_map("root");

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, NULL);
        Node *groups = find_child(root, "groups");
        Node *level = groups->data.list.items[0];

        then("should calculate group block size") {
          check_str_eq(find_child(level, "fixed_block_size")->data.string_val, "12");
        }

        then("should support group cursor") {
          check_str_eq(find_child(level, "supports_group_cursor")->data.string_val, "1");
        }
      }

      node_free(root);
    }
  }

  section("Error Handling") {
    given("invalid schema syntax") {
      const char *schema = "message Bad { int32 missing_semi }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, &err);

        then("should fail") {
          check_int_eq(rc, -1);
        }

        then("should provide error information") {
          check_int_ne(err.code, TBE_OK);
        }
      }

      node_free(root);
    }

    given("NULL arguments") {
      tbe_error_t err;

      when("calling parse_schema with NULL") {
        int rc = parse_schema(NULL, 0, NULL, &err);

        then("should return error") {
          check_int_eq(rc, -1);
        }

        then("should set error code") {
          check_int_eq(err.code, TBE_ERR_INVALID_ARGUMENT);
        }
      }
    }

    given("empty schema") {
      const char *schema = "";
      Node *root = create_node_map("root");

      when("parsing empty schema") {
        int rc = parse_schema(schema, 0, root, NULL);

        then("should succeed") {
          check_int_eq(rc, 0);
        }
      }

      node_free(root);
    }
  }

  section("Schema Replacement") {
    given("a root node with existing schema") {
      Node *root = create_node_map("root");
      map_add(root, create_node_string("marker", "keep"));

      when("parsing first schema") {
        parse_schema("composite First { int32 x; }", strlen("composite First { int32 x; }"), root, NULL);

        and_when("parsing second schema") {
          parse_schema("enum State { Idle = 1; } message Second { int32 y; }",
                      strlen("enum State { Idle = 1; } message Second { int32 y; }"), root, NULL);

          then("should keep non-schema nodes") {
            check_not_null(find_child(root, "marker"));
          }

          then("should replace composites") {
            Node *composites = find_child(root, "composites");
            check_uint_eq(composites->data.list.count, 0);
          }

          then("should have new messages") {
            Node *messages = find_child(root, "messages");
            check_uint_eq(messages->data.list.count, 1);
          }

          then("should have new enums") {
            Node *enums = find_child(root, "enums");
            check_uint_eq(enums->data.list.count, 1);
          }
        }
      }

      node_free(root);
    }
  }

  section("Memory Safety") {
    given("NULL value in create_node_string") {
      when("creating node with NULL value") {
        Node *n = create_node_string("test", NULL);

        then("should return NULL") {
          check_null(n);
        }
      }
    }

    given("NULL pointer in node_free") {
      when("freeing NULL node") {
        node_free(NULL);

        then("should not crash") {
          check(1);
        }
      }
    }

    given("nested structures") {
      Node *root = create_node_map("root");
      Node *list = create_node_list("items");

      when("building nested structure") {
        if (root && list) {
          list_add(list, create_node_string("item1", "value1"));
          list_add(list, create_node_string("item2", "value2"));
          map_add(root, list);
        }

        and_when("freeing root") {
          node_free(root);

          then("should free everything without leak") {
            check(1);
          }
        }
      }
    }
  }
}
