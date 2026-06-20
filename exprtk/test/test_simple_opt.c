/**
 * @file test_simple_opt.c
 * @brief Simple test for hash table and error handling optimizations
 */

#include "exprtk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main() {
    printf("=== Simple Optimization Tests ===\n\n");

    // Test 1: Basic variable operations
    printf("Test 1: Basic variable operations...\n");
    {
        exprtk_env_t env;
        exprtk_env_init(&env);

        exprtk_env_set(&env, "x", exprtk_val_num(10));
        exprtk_env_set(&env, "y", exprtk_val_num(20));

        exprtk_value_t x = exprtk_env_get(&env, "x");
        exprtk_value_t y = exprtk_env_get(&env, "y");

        if (x.data.number == 10 && y.data.number == 20) {
            printf("  ✓ PASS\n");
        } else {
            printf("  ✗ FAIL\n");
        }

        exprtk_env_free(&env);
    }

    // Test 2: Division by zero
    printf("\nTest 2: Division by zero...\n");
    {
        exprtk_env_t env;
        exprtk_env_init(&env);

        const char *code = "1 / 0";
        exprtk_node_t *ast = exprtk_parse(code, strlen(code));
        if (ast) {
            exprtk_eval(ast, &env);
            if (env.flow == exprtk_FLOW_THROW) {
                printf("  ✓ PASS: Exception thrown\n");
                printf("  Message: %s\n", env.error_msg);
            } else {
                printf("  ✗ FAIL: No exception\n");
            }
            exprtk_free(ast);
        }

        exprtk_env_free(&env);
    }

    // Test 3: Array bounds
    printf("\nTest 3: Array out of bounds...\n");
    {
        exprtk_env_t env;
        exprtk_env_init(&env);

        const char *code = "arr = [1,2,3]; arr[10]";
        exprtk_node_t *ast = exprtk_parse(code, strlen(code));
        if (ast) {
            exprtk_eval(ast, &env);
            if (env.flow == exprtk_FLOW_THROW) {
                printf("  ✓ PASS: Exception thrown\n");
                printf("  Message: %s\n", env.error_msg);
            } else {
                printf("  ✗ FAIL: No exception\n");
            }
            exprtk_free(ast);
        }

        exprtk_env_free(&env);
    }

    // Test 4: Try/catch works
    printf("\nTest 4: Try/catch...\n");
    {
        exprtk_env_t env;
        exprtk_env_init(&env);

        const char *code = "result = 0; try { x = 1/0; } catch(e) { result = 42; }";
        exprtk_node_t *ast = exprtk_parse(code, strlen(code));
        if (ast) {
            exprtk_eval(ast, &env);
            exprtk_value_t result = exprtk_env_get(&env, "result");
            if (result.data.number == 42) {
                printf("  ✓ PASS: Caught exception\n");
            } else {
                printf("  ✗ FAIL: result = %g\n", result.data.number);
            }
            exprtk_free(ast);
        }

        exprtk_env_free(&env);
    }

    // Test 5: Performance test
    printf("\nTest 5: Hash table performance...\n");
    {
        exprtk_env_t env;
        exprtk_env_init(&env);

        // Insert 100 variables
        for (int i = 0; i < 100; i++) {
            char name[32];
            snprintf(name, sizeof(name), "var_%d", i);
            exprtk_env_set(&env, name, exprtk_val_num(i));
        }

        // Benchmark lookups
        clock_t start = clock();
        for (int iter = 0; iter < 1000; iter++) {
            for (int i = 0; i < 100; i++) {
                char name[32];
                snprintf(name, sizeof(name), "var_%d", i);
                exprtk_value_t val = exprtk_env_get(&env, name);
                (void)val;
            }
        }
        clock_t end = clock();

        double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
        printf("  100,000 lookups: %.2f ms\n", ms);
        printf("  Average: %.3f μs per lookup\n", ms * 1000 / 100000);

        if (ms < 100) {
            printf("  ✓ PASS: Good performance\n");
        } else {
            printf("  ⚠ WARNING: Slower than expected\n");
        }

        exprtk_env_free(&env);
    }

    printf("\n=== All tests completed ===\n");
    return 0;
}
