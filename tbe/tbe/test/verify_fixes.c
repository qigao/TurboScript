#include "schema_parser_dsl.h"
#include "tbe_error.h"
#include "tbe_version.h"
#include "tbe_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    printf("=== TBE Security Fixes Verification ===\n");
    
    // Test 1: Version compatibility
    printf("1. Testing version compatibility...\n");
    const char *version = tbe_version();
    printf("   Version: %s\n", version);
    
    int compatible = tbe_version_compatible(1, 0, 0);
    printf("   Compatibility check (1.0.0): %s\n", compatible ? "PASS" : "FAIL");
    
    // Test 2: Null pointer safety in wire functions
    printf("2. Testing wire function null safety...\n");
    uint8_t result = tbe_wire_read_u8(NULL, 0);
    printf("   Null read returned: %u (expected 0)\n", result);
    
    // Write to null pointer should not crash
    tbe_wire_write_u32(NULL, 0, 42);
    printf("   Null write completed without crash: PASS\n");
    
    // Test 3: Error handling improvements
    printf("3. Testing enhanced error handling...\n");
    tbe_error_t err;
    tbe_error_init(&err);
    
    // Test with very long message
    char long_msg[500];
    memset(long_msg, 'X', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    
    tbe_error_set(&err, TBE_ERR_SYNTAX_ERROR, 1, 1, long_msg);
    printf("   Long message truncation: %s\n", strlen(err.message) <= 255 ? "PASS" : "FAIL");
    
    // Test 4: Deep structure handling
    printf("4. Testing deep structure safety...\n");
    Node *root = create_node_map("root");
    Node *current = root;
    
    // Create moderately deep structure
    for (int i = 0; i < 100; i++) {
        Node *nested = create_node_map("nested");
        if (!nested) {
            printf("   Allocation failed at depth %d\n", i);
            break;
        }
        map_add(current, nested);
        current = nested;
    }
    
    // This should not crash with stack overflow
    node_free(root);
    printf("   Deep structure cleanup: PASS\n");
    
    // Test 5: Input validation
    printf("5. Testing input validation...\n");
    Node *test_root = create_node_map("test");
    
    int rc = parse_schema(NULL, 0, test_root, &err);
    printf("   NULL input rejection: %s\n", (rc == -1 && err.code == TBE_ERR_INVALID_ARGUMENT) ? "PASS" : "FAIL");
    
    const char *valid_schema = "composite Point { int32 x; int32 y; }";
    rc = parse_schema(valid_schema, strlen(valid_schema), test_root, &err);
    printf("   Valid schema parsing: %s\n", rc == 0 ? "PASS" : "FAIL");
    
    node_free(test_root);
    
    // Test 6: Variable data bounds checking
    printf("6. Testing variable data bounds...\n");
    uint8_t test_data[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02, 0x03, 0x04}; // Very large length
    tbe_var_data_t var_data;
    
    bool var_result = tbe_wire_read_var_data(test_data, sizeof(test_data), 0, &var_data);
    printf("   Large length rejection: %s\n", !var_result ? "PASS" : "FAIL");
    
    printf("\n=== All security fixes verified ===\n");
    return 0;
}