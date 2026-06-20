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

suite("optional_fields_and_defaults") {
    describe("Optional Fields") {
        it("should parse required fields (default behavior)") {
            const char *schema = "message User { uint32 id; string name; }";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *user = messages->data.list.items[0];
            Node *fields = find_child(user, "fields");

            // 默认情况下字段是required的（没有is_optional标记）
            Node *id_field = fields->data.list.items[0];
            Node *name_field = fields->data.list.items[1];

            check_null(find_child(id_field, "is_optional"));
            check_null(find_child(name_field, "is_optional"));

            node_free(root);
        }

        it("should parse optional keyword") {
            const char *schema = "message User { "
                                "required uint32 id; "
                                "optional string email; "
                                "}";
            Node *root = create_node_map("root");
            tbe_error_t err;
            int rc = parse_schema(schema, strlen(schema), root, &err);

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *user = messages->data.list.items[0];
            Node *fields = find_child(user, "fields");

            Node *id_field = fields->data.list.items[0];
            Node *email_field = fields->data.list.items[1];

            // id是required，不应该有is_optional标记
            check_null(find_child(id_field, "is_optional"));

            // email是optional，应该有is_optional标记
            check_not_null(find_child(email_field, "is_optional"));
            check_str_eq(find_child(email_field, "is_optional")->data.string_val, "1");

            node_free(root);
        }

        it("should support multiple optional fields") {
            const char *schema = "message User { "
                                "required uint32 id; "
                                "optional uint32 age; "
                                "required string username; "
                                "optional string email; "
                                "optional string phone; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *user = messages->data.list.items[0];
            Node *fields = find_child(user, "fields");

            check_uint_eq(fields->data.list.count, 5);

            // 检查固定字段（id - required, age - optional）
            check_null(find_child(fields->data.list.items[0], "is_optional"));      // id
            check_not_null(find_child(fields->data.list.items[1], "is_optional"));  // age

            // 检查变长字段（username - required, email - optional, phone - optional）
            check_null(find_child(fields->data.list.items[2], "is_optional"));      // username
            check_not_null(find_child(fields->data.list.items[3], "is_optional"));  // email
            check_not_null(find_child(fields->data.list.items[4], "is_optional"));  // phone

            node_free(root);
        }
    }

    describe("Default Values") {
        it("should parse numeric default values") {
            const char *schema = "message Config { "
                                "uint32 timeout default 3000; "
                                "uint32 retries default 3; "
                                "}";
            Node *root = create_node_map("root");
            tbe_error_t err;
            int rc = parse_schema(schema, strlen(schema), root, &err);

            if (rc != 0) {
                printf("Parse error: %s\n", err.message);
            }

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *config = messages->data.list.items[0];
            Node *fields = find_child(config, "fields");

            Node *timeout_field = fields->data.list.items[0];
            Node *retries_field = fields->data.list.items[1];

            // 检查has_default标记
            check_not_null(find_child(timeout_field, "has_default"));
            check_not_null(find_child(retries_field, "has_default"));

            // 检查default_value
            check_str_eq(find_child(timeout_field, "default_value")->data.string_val, "3000");
            check_str_eq(find_child(retries_field, "default_value")->data.string_val, "3");

            node_free(root);
        }

        it("should parse string default values") {
            const char *schema = "message Config { "
                                "string endpoint default \"localhost\"; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *config = messages->data.list.items[0];
            Node *fields = find_child(config, "fields");
            Node *endpoint = fields->data.list.items[0];

            check_not_null(find_child(endpoint, "has_default"));
            check_str_eq(find_child(endpoint, "default_value")->data.string_val, "localhost");

            node_free(root);
        }

        it("should parse boolean default values") {
            const char *schema = "message Config { "
                                "uint8 enabled default true; "
                                "uint8 debug default false; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *config = messages->data.list.items[0];
            Node *fields = find_child(config, "fields");

            check_str_eq(find_child(fields->data.list.items[0], "default_value")->data.string_val, "true");
            check_str_eq(find_child(fields->data.list.items[1], "default_value")->data.string_val, "false");

            node_free(root);
        }

        it("should support combined optional and default") {
            const char *schema = "message User { "
                                "required uint32 id; "
                                "optional uint32 age default 18; "
                                "optional string role default \"user\"; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *messages = find_child(root, "messages");
            Node *user = messages->data.list.items[0];
            Node *fields = find_child(user, "fields");

            Node *age = fields->data.list.items[1];
            Node *role = fields->data.list.items[2];

            // age是optional且有default
            check_not_null(find_child(age, "is_optional"));
            check_not_null(find_child(age, "has_default"));
            check_str_eq(find_child(age, "default_value")->data.string_val, "18");

            // role是optional且有default
            check_not_null(find_child(role, "is_optional"));
            check_not_null(find_child(role, "has_default"));
            check_str_eq(find_child(role, "default_value")->data.string_val, "user");

            node_free(root);
        }
    }

    describe("Enum String Conversion") {
        it("should generate enum to string mapping info") {
            const char *schema = "enum Status { "
                                "Idle = 0; "
                                "Active = 1; "
                                "Error = 2; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *enums = find_child(root, "enums");
            check_not_null(enums);
            check_uint_eq(enums->data.list.count, 1);

            Node *status_enum = enums->data.list.items[0];
            Node *items = find_child(status_enum, "items");

            check_uint_eq(items->data.list.count, 3);

            // 验证枚举项
            Node *idle = items->data.list.items[0];
            check_str_eq(find_child(idle, "name")->data.string_val, "Idle");
            check_str_eq(find_child(idle, "value")->data.string_val, "0");

            Node *active = items->data.list.items[1];
            check_str_eq(find_child(active, "name")->data.string_val, "Active");
            check_str_eq(find_child(active, "value")->data.string_val, "1");

            node_free(root);
        }
    }

    describe("Schema Versioning") {
        it("should parse schema version attributes") {
            const char *schema = "schema MySchema [id(1), version(100), byte_order(little)];";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *schema_node = find_child(root, "schema");
            check_not_null(schema_node);
            check_str_eq(find_child(schema_node, "schema_name")->data.string_val, "MySchema");

            Node *attrs = find_child(schema_node, "attributes");
            check_not_null(attrs);

            // 检查version属性
            Node *version_attr = find_child(attrs, "version");
            check_not_null(version_attr);
            check_str_eq(find_child(version_attr, "value")->data.string_val, "100");

            node_free(root);
        }
    }
}