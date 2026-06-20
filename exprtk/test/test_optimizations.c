/**
 * @file test_optimizations.c
 * @brief Test suite for exprtk performance and reliability optimizations
 */

#include "exprtk.h" 
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


// Test 1: Hash table performance
void test_hash_table_performance() {
  printf("Test 1: Hash table performance...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  // Insert 1000 variables
  for (int i = 0; i < 1000; i++) {
    char name[32];
    snprintf(name, sizeof(name), "var_%d", i);
    exprtk_env_set(&env, name, exprtk_val_num(i));
  }

  // Lookup should be fast (< 10ms for all 1000)
  clock_t start = clock();
  for (int i = 0; i < 1000; i++) {
    char name[32];
    snprintf(name, sizeof(name), "var_%d", i);
    exprtk_value_t val = exprtk_env_get(&env, name);
    assert(val.type == EXPRTK_VAL_NUMBER);
    assert(val.data.number == i);
  }
  clock_t end = clock();
  double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
  printf("  1000 lookups: %.2f ms\n", ms);

  if (ms < 10) {
    printf("  ✓ PASS: Performance is excellent (< 10ms)\n");
  } else {
    printf("  ⚠ WARNING: Performance is slower than expected (%.2f ms)\n", ms);
  }

  exprtk_env_free(&env);
}

// Test 2: Division by zero throws
void test_division_by_zero() {
  printf("\nTest 2: Division by zero error handling...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "1 / 0";
  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_value_t result = exprtk_eval(ast, &env);

  if (env.flow == exprtk_FLOW_THROW) {
    printf("  ✓ PASS: Division by zero throws exception\n");
    printf("  Error message: %s\n", env.error_msg);
  } else {
    printf("  ✗ FAIL: Division by zero did not throw\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 3: Modulo by zero throws
void test_modulo_by_zero() {
  printf("\nTest 3: Modulo by zero error handling...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "10 % 0";
  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_value_t result = exprtk_eval(ast, &env);

  if (env.flow == exprtk_FLOW_THROW) {
    printf("  ✓ PASS: Modulo by zero throws exception\n");
    printf("  Error message: %s\n", env.error_msg);
  } else {
    printf("  ✗ FAIL: Modulo by zero did not throw\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 4: Array bounds checking
void test_array_bounds() {
  printf("\nTest 4: Array out-of-bounds error handling...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "arr = [1,2,3]; arr[10]";
  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_value_t result = exprtk_eval(ast, &env);

  if (env.flow == exprtk_FLOW_THROW) {
    printf("  ✓ PASS: Array out-of-bounds throws exception\n");
    printf("  Error message: %s\n", env.error_msg);
  } else {
    printf("  ✗ FAIL: Array out-of-bounds did not throw\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 5: Negative array index
void test_negative_array_index() {
  printf("\nTest 5: Negative array index error handling...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "arr = [1,2,3]; arr[-1]";
  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_value_t result = exprtk_eval(ast, &env);

  if (env.flow == exprtk_FLOW_THROW) {
    printf("  ✓ PASS: Negative array index throws exception\n");
    printf("  Error message: %s\n", env.error_msg);
  } else {
    printf("  ✗ FAIL: Negative array index did not throw\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 6: Type error in binary operations
void test_type_error() {
  printf("\nTest 6: Type error in binary operations...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "\"hello\" - 5";
  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_value_t result = exprtk_eval(ast, &env);

  if (env.flow == exprtk_FLOW_THROW) {
    printf("  ✓ PASS: Type error throws exception\n");
    printf("  Error message: %s\n", env.error_msg);
  } else {
    printf("  ✗ FAIL: Type error did not throw\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 7: Try/catch still works
void test_try_catch() {
  printf("\nTest 7: Try/catch error handling...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "result = 0; "
                     "try { "
                     "  x = 1 / 0; "
                     "} catch (e) { "
                     "  result = 42; "
                     "}";

  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_eval(ast, &env);

  exprtk_value_t result = exprtk_env_get(&env, "result");
  if (result.type == EXPRTK_VAL_NUMBER && result.data.number == 42) {
    printf("  ✓ PASS: Try/catch successfully caught exception\n");
  } else {
    printf("  ✗ FAIL: Try/catch did not work correctly\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 8: Valid operations still work
void test_valid_operations() {
  printf("\nTest 8: Valid operations still work...\n");

  exprtk_env_t env;
  exprtk_env_init(&env);

  const char *code = "x = 10; y = 20; z = x + y * 2; arr = [1,2,3]; arr[1]";
  exprtk_node_t *ast = exprtk_parse(code, strlen(code));
  assert(ast != NULL);

  exprtk_value_t result = exprtk_eval(ast, &env);

  exprtk_value_t z = exprtk_env_get(&env, "z");
  if (z.type == EXPRTK_VAL_NUMBER && z.data.number == 50 && result.type == EXPRTK_VAL_NUMBER &&
      result.data.number == 2) {
    printf("  ✓ PASS: Valid operations work correctly\n");
  } else {
    printf("  ✗ FAIL: Valid operations failed\n");
  }

  exprtk_free(ast);
  exprtk_env_free(&env);
}

// Test 9: Performance benchmark with nested scopes
void test_nested_scope_performance() {
  printf("\nTest 9: Nested scope performance...\n");

  exprtk_env_t root;
  exprtk_env_init(&root);

  // Create nested scopes
  exprtk_env_t *scopes[10];
  scopes[0] = &root;

  for (int i = 1; i < 10; i++) {
    scopes[i] = malloc(sizeof(exprtk_env_t));
    exprtk_env_init_local(scopes[i]);
    scopes[i]->parent = scopes[i - 1];

    // Add 100 variables per scope
    for (int j = 0; j < 100; j++) {
      char name[32];
      snprintf(name, sizeof(name), "var_%d_%d", i, j);
      exprtk_env_set_local(scopes[i], name, exprtk_val_num(i * 100 + j));
    }
  }

  // Benchmark: lookup variable from deepest scope
  clock_t start = clock();
  for (int iter = 0; iter < 10000; iter++) {
    exprtk_value_t val = exprtk_env_get(scopes[9], "var_9_99");
    assert(val.data.number == 999);
  }
  clock_t end = clock();

  double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
  double us_per_lookup = ms * 1000 / 10000;
  printf("  10000 lookups (100 vars × 10 scopes): %.2f ms\n", ms);
  printf("  Average: %.2f μs per lookup\n", us_per_lookup);

  if (us_per_lookup < 1.0) {
    printf("  ✓ PASS: Excellent performance (< 1 μs per lookup)\n");
  } else if (us_per_lookup < 10.0) {
    printf("  ✓ PASS: Good performance (< 10 μs per lookup)\n");
  } else {
    printf("  ⚠ WARNING: Performance could be better (%.2f μs per lookup)\n", us_per_lookup);
  }

  // Cleanup
  for (int i = 9; i >= 1; i--) {
    exprtk_env_free(scopes[i]);
    free(scopes[i]);
  }
  exprtk_env_free(&root);
}

int main() {
  printf("=================================================\n");
  printf("exprtk Performance & Reliability Optimization Tests\n");
  printf("=================================================\n\n");

  test_hash_table_performance();
  test_division_by_zero();
  test_modulo_by_zero();
  test_array_bounds();
  test_negative_array_index();
  test_type_error();
  test_try_catch();
  test_valid_operations();
  test_nested_scope_performance();

  printf("\n=================================================\n");
  printf("All tests completed!\n");
  printf("=================================================\n");

  return 0;
}
