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

suite("enum_helpers") {
    describe("Enum Metadata Generation") {
        it("should generate enum helper metadata") {
            const char *schema = "enum UserRole { "
                                "Guest = 0; "
                                "User = 1; "
                                "Moderator = 2; "
                                "Admin = 3; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *enums = find_child(root, "enums");
            check_not_null(enums);
            check_uint_eq(enums->data.list.count, 1);

            Node *user_role = enums->data.list.items[0];
            
            // 检查项目数量
            Node *count_node = find_child(user_role, "items_count");
            check_not_null(count_node);
            check_str_eq(count_node->data.string_val, "4");
            
            // 检查最小值
            Node *min_node = find_child(user_role, "min_value");
            check_not_null(min_node);
            check_str_eq(min_node->data.string_val, "UserRole_Guest");
            
            // 检查最大值
            Node *max_node = find_child(user_role, "max_value");
            check_not_null(max_node);
            check_str_eq(max_node->data.string_val, "UserRole_Admin");

            node_free(root);
        }

        it("should handle non-sequential enum values") {
            const char *schema = "enum Status { "
                                "Inactive = 10; "
                                "Active = 5; "
                                "Error = 20; "
                                "}";
            Node *root = create_node_map("root");
            int rc = parse_schema(schema, strlen(schema), root, NULL);

            check_int_eq(rc, 0);

            Node *enums = find_child(root, "enums");
            Node *status = enums->data.list.items[0];
            
            // 检查项目数量
            check_str_eq(find_child(status, "items_count")->data.string_val, "3");
            
            // 检查最小值（应该是Active=5）
            check_str_eq(find_child(status, "min_value")->data.string_val, "Status_Active");
            
            // 检查最大值（应该是Error=20）
            check_str_eq(find_child(status, "max_value")->data.string_val, "Status_Error");

            node_free(root);
        }
    }
}