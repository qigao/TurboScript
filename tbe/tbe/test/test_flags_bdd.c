#include "tbe_error.h"
#include "schema_parser_dsl.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Node *find_child(Node *parent, const char *name) {
  if (!parent || parent->type != NODE_MAP) return NULL;
  for (size_t i = 0; i < parent->data.map.count; i++) {
    Node *child = parent->data.map.items[i];
    if (child->name && strcmp(child->name, name) == 0) return child;
  }
  return NULL;
}

suite("Flags Feature") {
  section("Flags Declaration") {
    given("a flags declaration without underlying type") {
      const char *schema = "flags Permissions { Read; Write; Execute; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, &err);

        then("should parse successfully") { check_int_eq(rc, 0); }

        then("should create flags in enums list") {
          Node *enums = find_child(root, "enums");
          check_not_null(enums);
          check_uint_eq(enums->data.list.count, 1);
        }

        then("should mark as flags") {
          Node *enums = find_child(root, "enums");
          Node *perms = enums->data.list.items[0];
          check_str_eq(find_child(perms, "is_flags")->data.string_val, "1");
        }

        then("should have correct name") {
          Node *enums = find_child(root, "enums");
          Node *perms = enums->data.list.items[0];
          check_str_eq(find_child(perms, "enum_name")->data.string_val, "Permissions");
        }
      }

      node_free(root);
    }

    given("a flags declaration with underlying type") {
      const char *schema = "flags OrderFlags <uint8> { IOC = 1; FOK = 2; PostOnly = 4; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, &err);

        then("should parse successfully") { check_int_eq(rc, 0); }

        then("should have underlying type") {
          Node *enums = find_child(root, "enums");
          Node *flags = enums->data.list.items[0];
          check_str_eq(find_child(flags, "underlying_type")->data.string_val, "uint8");
        }

        then("should preserve explicit values") {
          Node *enums = find_child(root, "enums");
          Node *flags = enums->data.list.items[0];
          Node *items = find_child(flags, "items");

          check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
          check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
          check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "4");
        }
      }

      node_free(root);
    }
  }

  section("Auto-increment Behavior") {
    given("flags without explicit values") {
      const char *schema = "flags Status { Active; Pending; Completed; Cancelled; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, &err);
        Node *enums = find_child(root, "enums");
        Node *status = enums->data.list.items[0];
        Node *items = find_child(status, "items");

        then("should auto-increment as powers of 2") {
          check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
          check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
          check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "4");
          check_str_eq(find_child(items->data.list.items[3], "value")->data.string_val, "8");
        }

        then("should have 4 items") { check_uint_eq(items->data.list.count, 4); }
      }

      node_free(root);
    }

    given("flags with mixed explicit and auto values") {
      const char *schema = "flags Mixed { A = 1; B; C = 16; D; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, &err);
        Node *enums = find_child(root, "enums");
        Node *mixed = enums->data.list.items[0];
        Node *items = find_child(mixed, "items");

        then("should respect explicit values") {
          check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
          check_str_eq(find_child(items->data.list.items[2], "value")->data.string_val, "16");
        }

        then("should auto-increment from previous value") {
          check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2");
          check_str_eq(find_child(items->data.list.items[3], "value")->data.string_val, "32");
        }
      }

      node_free(root);
    }
  }

  section("Flags vs Enum Distinction") {
    given("both enum and flags in same schema") {
      const char *schema = "enum Side <uint8> { Buy = 1; Sell = 2; } "
                           "flags Permissions { Read; Write; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, &err);

        then("should parse both successfully") { check_int_eq(rc, 0); }

        then("should have 2 items in enums list") {
          Node *enums = find_child(root, "enums");
          check_uint_eq(enums->data.list.count, 2);
        }

        then("enum should not have is_flags marker") {
          Node *enums = find_child(root, "enums");
          Node *side = enums->data.list.items[0];
          check_null(find_child(side, "is_flags"));
        }

        then("flags should have is_flags marker") {
          Node *enums = find_child(root, "enums");
          Node *perms = enums->data.list.items[1];
          check_str_eq(find_child(perms, "is_flags")->data.string_val, "1");
        }
      }

      node_free(root);
    }
  }

  section("Edge Cases") {
    given("flags with single item") {
      const char *schema = "flags Single { Only; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        int rc = parse_schema(schema, strlen(schema), root, &err);

        then("should parse successfully") { check_int_eq(rc, 0); }

        then("should have value 1") {
          Node *enums = find_child(root, "enums");
          Node *single = enums->data.list.items[0];
          Node *items = find_child(single, "items");
          check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1");
        }
      }

      node_free(root);
    }

    given("flags with large power of 2") {
      const char *schema = "flags Large { A = 1024; B; }";
      Node *root = create_node_map("root");
      tbe_error_t err;

      when("parsing the schema") {
        parse_schema(schema, strlen(schema), root, &err);
        Node *enums = find_child(root, "enums");
        Node *large = enums->data.list.items[0];
        Node *items = find_child(large, "items");

        then("should handle large values") {
          check_str_eq(find_child(items->data.list.items[0], "value")->data.string_val, "1024");
        }

        then("should double for next value") {
          check_str_eq(find_child(items->data.list.items[1], "value")->data.string_val, "2048");
        }
      }

      node_free(root);
    }
  }
}
