#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_node(Node *node, int indent) {
    if (!node) return;
    
    for (int i = 0; i < indent; i++) printf("  ");
    
    if (node->name) {
        printf("%s: ", node->name);
    }
    
    switch (node->type) {
        case NODE_STRING:
            printf("\"%s\"\n", node->data.string_val ? node->data.string_val : "(null)");
            break;
        case NODE_LIST:
            printf("[\n");
            for (size_t i = 0; i < node->data.list.count; i++) {
                print_node(node->data.list.items[i], indent + 1);
            }
            for (int i = 0; i < indent; i++) printf("  ");
            printf("]\n");
            break;
        case NODE_MAP:
            printf("{\n");
            for (size_t i = 0; i < node->data.map.count; i++) {
                print_node(node->data.map.items[i], indent + 1);
            }
            for (int i = 0; i < indent; i++) printf("  ");
            printf("}\n");
            break;
    }
}

int main() {
    const char *schema = "schema TestOptional [id(1), version(1), byte_order(little)];\n"
                        "message User {\n"
                        "    required uint32 id;\n" 
                        "    optional uint32 age default 18;\n"
                        "    required string username;\n"
                        "    optional string email;\n"
                        "}";
    
    Node *root = create_node_map("root");
    tbe_error_t err;
    int rc = parse_schema(schema, strlen(schema), root, &err);
    
    if (rc != 0) {
        printf("Parse error: %s\n", err.message);
        return 1;
    }
    
    printf("Parsed schema:\n");
    print_node(root, 0);
    
    node_free(root);
    return 0;
}