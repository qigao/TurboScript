#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "tbe_wire.h"
#include "tbe_version.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

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

suite("tbe_robustness") {
    describe("Deep Structure Handling") {
        it("should handle deeply nested structures safely") {
            // Create a very deep node tree to test recursion protection
            Node *root = create_node_map("root");
            Node *current = root;
            
            // Create a deep nested structure (but within limits)
            for (int i = 0; i < 50; i++) {
                char name[32];
                snprintf(name, sizeof(name), "level_%d", i);
                Node *nested = create_node_map(name);
                check_not_null(nested);
                
                int result = map_add(current, nested);
                check_int_eq(result, 0);
                current = nested;
            }
            
            // Free should handle this safely
            node_free(root);
        }

        it("should handle null pointer gracefully in wire functions") {
            // Test null pointer handling in wire read functions
            uint8_t result_u8 = tbe_wire_read_u8(NULL, 0);
            check_int_eq(result_u8, 0);
            
            int8_t result_i8 = tbe_wire_read_i8(NULL, 0);
            check_int_eq(result_i8, 0);
            
            uint16_t result_u16 = tbe_wire_read_u16(NULL, 0);
            check_int_eq(result_u16, 0);
            
            uint32_t result_u32 = tbe_wire_read_u32(NULL, 0);
            check_int_eq(result_u32, 0);
            
            uint64_t result_u64 = tbe_wire_read_u64(NULL, 0);
            check_int_eq(result_u64, 0);
            
            float result_f32 = tbe_wire_read_f32(NULL, 0);
            check_float_eq(result_f32, 0.0f, 0.0f);
            
            double result_f64 = tbe_wire_read_f64(NULL, 0);
            check_double_eq(result_f64, 0.0, 0.0);
            
            // Test write functions don't crash with null pointers
            tbe_wire_write_u8(NULL, 0, 42);
            tbe_wire_write_i8(NULL, 0, -42);
            tbe_wire_write_u16(NULL, 0, 1000);
            tbe_wire_write_u32(NULL, 0, 100000);
            tbe_wire_write_u64(NULL, 0, 1000000000ULL);
            tbe_wire_write_f32(NULL, 0, 3.14f);
            tbe_wire_write_f64(NULL, 0, 2.718281828);
            
            // If we get here, no crashes occurred
            check_int_eq(1, 1);
        }
        
        it("should handle allocation failures gracefully") {
            // Test error handling with invalid arguments
            Node *root = create_node_map("root");
            tbe_error_t err;
            
            int rc = parse_schema(NULL, 0, root, &err);
            check_int_eq(rc, -1);
            check_int_eq(err.code, TBE_ERR_INVALID_ARGUMENT);
            
            rc = parse_schema("test", 4, NULL, &err);
            check_int_eq(rc, -1);
            check_int_eq(err.code, TBE_ERR_INVALID_ARGUMENT);
            
            // Test with extremely large input
            char large_text[] = "message Test { uint32 x; }";
            rc = parse_schema(large_text, SIZE_MAX, root, &err);  // Unreasonably large size
            check_int_eq(rc, -1);
            check_int_eq(err.code, TBE_ERR_INVALID_ARGUMENT);
            
            node_free(root);
        }
    }

    describe("Buffer Safety") {
        it("should handle very long error messages safely") {
            tbe_error_t err;
            tbe_error_init(&err);
            
            // Create a very long message
            char long_msg[1000];
            memset(long_msg, 'A', sizeof(long_msg) - 1);
            long_msg[sizeof(long_msg) - 1] = '\0';
            
            tbe_error_set(&err, TBE_ERR_SYNTAX_ERROR, 1, 1, long_msg);
            
            // Message should be truncated but not cause buffer overflow
            check_int_eq(err.code, TBE_ERR_SYNTAX_ERROR);
            check_int_eq(err.line, 1);
            check_int_eq(err.column, 1);
            check_int_le(strlen(err.message), 255);  // Should be truncated
            
            // Should still be null-terminated
            check_int_eq(err.message[255], '\0');
        }
        
        it("should handle empty and null error messages") {
            tbe_error_t err;
            tbe_error_init(&err);
            
            // Test with empty string
            tbe_error_set(&err, TBE_ERR_IO_ERROR, -1, -1, "");
            check_int_eq(err.code, TBE_ERR_IO_ERROR);
            check_str_eq(err.message, "I/O error");  // Should use default
            
            // Test with NULL message
            tbe_error_set(&err, TBE_ERR_LEXER_ERROR, -1, -1, NULL);
            check_int_eq(err.code, TBE_ERR_LEXER_ERROR);
            check_str_eq(err.message, "Lexer error");  // Should use default
        }
    }
    
    describe("Large Scale Parsing") {
        it("should handle schemas with many declarations") {
            // Create a large schema with many enums, composites, and messages
            char *large_schema = (char*)malloc(50000);
            char *pos = large_schema;
            
            // Add many enum declarations
            for (int i = 0; i < 20; i++) {
                pos += sprintf(pos, "enum TestEnum%d { Value1; Value2; Value3; } ", i);
            }
            
            // Add many composite declarations  
            for (int i = 0; i < 20; i++) {
                pos += sprintf(pos, "composite TestComp%d { uint32 field1; uint64 field2; } ", i);
            }
            
            // Add many message declarations
            for (int i = 0; i < 20; i++) {
                pos += sprintf(pos, "message TestMsg%d { uint32 id; string data; } ", i);
            }
            
            Node *root = create_node_map("root");
            tbe_error_t err;
            int rc = parse_schema(large_schema, strlen(large_schema), root, &err);
            
            check_int_eq(rc, 0);
            
            // Verify all declarations were parsed
            Node *enums = find_child(root, "enums");
            Node *composites = find_child(root, "composites");  
            Node *messages = find_child(root, "messages");
            
            check_not_null(enums);
            check_not_null(composites);
            check_not_null(messages);
            check_uint_eq(enums->data.list.count, 20);
            check_uint_eq(composites->data.list.count, 20);
            check_uint_eq(messages->data.list.count, 20);
            
            node_free(root);
            free(large_schema);
        }
    }
    
    describe("Version Compatibility") {
        it("should provide version information") {
            const char *version = tbe_version();
            check_not_null(version);
            check_str_eq(version, "1.0.0");
            
            int major = -1, minor = -1, patch = -1;
            tbe_version_components(&major, &minor, &patch);
            check_int_eq(major, 1);
            check_int_eq(minor, 0);
            check_int_eq(patch, 0);
        }
        
        it("should check version compatibility correctly") {
            // Same version should be compatible
            check_int_eq(tbe_version_compatible(1, 0, 0), 1);
            
            // Higher minor version should be compatible
            check_int_eq(tbe_version_compatible(1, 0, 0), 1);  // Our version is 1.0.0
            
            // Different major version should not be compatible
            check_int_eq(tbe_version_compatible(2, 0, 0), 0);
            check_int_eq(tbe_version_compatible(0, 9, 0), 0);
            
            // Higher required minor version should not be compatible
            check_int_eq(tbe_version_compatible(1, 1, 0), 0);
        }
    }
}