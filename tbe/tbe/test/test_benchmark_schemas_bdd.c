#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * This file implements BDD-style tests for the schemas used in benchmark_sbe_simple.c.
 * It ensures that the parser correctly interprets the structure of these baseline schemas.
 */

// --- Benchmark Schemas ---

static const char *SCHEMA_SMALL =
    "message SmallMsg { "
    "uint64 timestamp; "
    "uint32 value1; "
    "uint32 value2; "
    "}";

static const char *SCHEMA_MEDIUM =
    "composite Header { uint32 seq; uint64 ts; } "
    "message MediumMsg { "
    "Header header; "
    "uint64 field1; "
    "uint64 field2; "
    "uint64 field3; "
    "uint64 field4; "
    "}";

static const char *SCHEMA_LARGE =
    "group Level { uint64 price; uint32 qty; } "
    "message LargeMsg { "
    "uint64 timestamp; "
    "group<Level> bids; "
    "group<Level> asks; "
    "string symbol; "
    "}";

static const char *SCHEMA_ENUM =
    "enum Status <uint8> { "
    "Idle = 0; Active = 1; Pending = 2; "
    "Processing = 3; Completed = 4; Failed = 5; "
    "Cancelled = 6; Timeout = 7; Retry = 8; Unknown = 9; "
    "}";

static const char *SCHEMA_FLAGS =
    "flags Permissions { "
    "Read; Write; Execute; Delete; "
    "Create; Modify; Share; Admin; "
    "}";

static const char *SCHEMA_COMPLEX =
    "schema Market [id(1), version(1), byte_order(little)]; "
    "enum Side <uint8> { Buy = 1; Sell = 2; } "
    "flags OrderFlags { IOC; FOK; PostOnly; } "
    "composite Header { uint32 seq; uint64 ts; } "
    "group Level { uint64 price; uint32 qty; } "
    "message Order { "
    "Header header; "
    "uint64 order_id; "
    "Side side; "
    "OrderFlags order_flags; "
    "uint32 price; "
    "uint32 quantity; "
    "group<Level> levels; "
    "string symbol; "
    "}";

// --- Helpers ---

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

static Node *get_node_at(Node *list, size_t index) {
  if (!list || list->type != NODE_LIST || index >= list->data.list.count) return NULL;
  return list->data.list.items[index];
}

// --- Tests ---

suite("Benchmark Schema Verification") {
  
  section("Small Schema") {
    given("the SmallMsg schema") {
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(SCHEMA_SMALL, strlen(SCHEMA_SMALL), root, NULL);

        then("it should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("it should have one message") {
          Node *messages = find_child(root, "messages");
          check_not_null(messages);
          if (messages) check_uint_eq(messages->data.list.count, 1);
        }

        then("SmallMsg should have 3 fields") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *fields = find_child(msg, "fields");
            check_not_null(fields);
            if (fields) check_uint_eq(fields->data.list.count, 3);
          }
        }
        
        then("SmallMsg total size should be 16 bytes") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *size = find_child(msg, "fixed_block_size");
            check_not_null(size);
            if (size) check_str_eq(size->data.string_val, "16");
          }
        }
      }
      node_free(root);
    }
  }

  section("Medium Schema") {
    given("the MediumMsg schema with composite Header") {
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(SCHEMA_MEDIUM, strlen(SCHEMA_MEDIUM), root, NULL);
        check_int_eq(rc, 0);

        then("it should have one composite and one message") {
          Node *c = find_child(root, "composites");
          Node *m = find_child(root, "messages");
          check_not_null(c);
          check_not_null(m);
          if (c) check_uint_eq(c->data.list.count, 1);
          if (m) check_uint_eq(m->data.list.count, 1);
        }

        then("MediumMsg should include the header") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *fields = find_child(msg, "fields");
            Node *header_field = get_node_at(fields, 0);
            check_not_null(header_field);
            if (header_field) check_str_eq(find_child(header_field, "type")->data.string_val, "Header");
          }
        }
        
        then("Total message block size should be 12 (header) + 32 (4x8) = 44") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *size = find_child(msg, "fixed_block_size");
            check_not_null(size);
            if (size) check_str_eq(size->data.string_val, "44");
          }
        }
      }
      node_free(root);
    }
  }

  section("Large Schema") {
    given("the LargeMsg schema with groups and strings") {
      Node *root = create_node_map("root");

      when("parsing the schema") {
        int rc = parse_schema(SCHEMA_LARGE, strlen(SCHEMA_LARGE), root, NULL);
        check_int_eq(rc, 0);

        then("it should have one group defined") {
          Node *g = find_child(root, "groups");
          check_not_null(g);
          if (g) check_uint_eq(g->data.list.count, 1);
        }

        then("LargeMsg should have 4 fields") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *fields = find_child(msg, "fields");
            check_not_null(fields);
            if (fields) check_uint_eq(fields->data.list.count, 4);
          }
        }

        then("symbol field should be variable length") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *fields = find_child(msg, "fields");
            Node *symbol = get_node_at(fields, 3);
            check_not_null(symbol);
            if (symbol) check_str_eq(find_child(symbol, "is_variable_size")->data.string_val, "1");
          }
        }
      }
      node_free(root);
    }
  }

  section("Enum/Flags Metadata") {
    given("the Status enum and Permissions flags") {
      Node *root = create_node_map("root");

      when("parsing Status enum") {
        int rc = parse_schema(SCHEMA_ENUM, strlen(SCHEMA_ENUM), root, NULL);
        check_int_eq(rc, 0);
        Node *enums = find_child(root, "enums");
        if (enums) {
          Node *status = get_node_at(enums, 0);

          then("it should have 10 items") {
            Node *items = find_child(status, "items");
            check_not_null(items);
            if (items) check_uint_eq(items->data.list.count, 10);
          }

          then("Retry should have value 8") {
            Node *items = find_child(status, "items");
            if (items) {
              Node *retry = get_node_at(items, 8);
              check_not_null(retry);
              if (retry) check_str_eq(find_child(retry, "value")->data.string_val, "8");
            }
          }
        }
      }

      when("parsing Permissions flags") {
        node_free(root);
        root = create_node_map("root");
        int rc = parse_schema(SCHEMA_FLAGS, strlen(SCHEMA_FLAGS), root, NULL);
        check_int_eq(rc, 0);
        Node *enums = find_child(root, "enums");
        if (enums) {
          Node *perms = get_node_at(enums, 0);

          then("it should be marked as flags") {
            check_str_eq(find_child(perms, "is_flags")->data.string_val, "1");
          }

          then("Write should have value 2 (power of 2)") {
            Node *items = find_child(perms, "items");
            if (items) {
              Node *write = get_node_at(items, 1);
              check_not_null(write);
              if (write) check_str_eq(find_child(write, "value")->data.string_val, "2");
            }
          }
        }
      }
      node_free(root);
    }
  }

  section("Complex Market Schema") {
    given("the complete Market schema") {
      Node *root = create_node_map("root");

      when("parsing the complex schema") {
        int rc = parse_schema(SCHEMA_COMPLEX, strlen(SCHEMA_COMPLEX), root, NULL);

        then("it should parse successfully") {
          check_int_eq(rc, 0);
        }

        then("it should have correct schema name and byte order") {
          Node *schema = find_child(root, "schema");
          check_not_null(schema);
          if (schema) {
            check_str_eq(find_child(schema, "schema_name")->data.string_val, "Market");
            check_str_eq(find_child(schema, "wire_byte_order")->data.string_val, "little");
          }
        }

        then("Order message should have correct structural markers") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            check_str_eq(find_child(msg, "is_message_decl")->data.string_val, "1");
          }
        }

        then("Order message should have 8 fields") {
          Node *messages = find_child(root, "messages");
          if (messages) {
            Node *msg = get_node_at(messages, 0);
            Node *fields = find_child(msg, "fields");
            check_not_null(fields);
            if (fields) check_uint_eq(fields->data.list.count, 8);
          }
        }
      }
      node_free(root);
    }
  }
}
