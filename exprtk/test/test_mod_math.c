/**
 * @file test_mod_math.c
 * @brief Unit tests for SIMD-optimized math functions in mod_math.c
 */

#include "exprtk.h"
#include "exprtk_module.h"
#include "exprtk_types.h"
#include "simd_helpers.h"
#include "tinytest.h"
#include "turbo_buffer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
  #define M_PI 3.14159265358979323846
#endif

#define TEST_TOLERANCE 1e-10

// Forward declarations

// Helper function
static void fill_random(double *arr, size_t n, double min, double max) {
  for (size_t i = 0; i < n; i++) {
    arr[i] = min + (max - min) * ((double)rand() / RAND_MAX);
  }
}

spec("SIMD Computation") {
  describe("OLS Regression") {
    it("should fit perfect linear relationship") {
      // y = 2x + 3
      double x[] = {1, 2, 3, 4, 5};
      double y[] = {5, 7, 9, 11, 13};

      ols_result_t result = ols_fit(y, x, 5);

      check_double_eq(result.slope, 2.0, TEST_TOLERANCE);
      check_double_eq(result.intercept, 3.0, TEST_TOLERANCE);
      check_double_eq(result.res_var, 0.0, TEST_TOLERANCE);
    }

    it("should handle large datasets") {
      const size_t n = 1000;
      double *x = malloc(n * sizeof(double));
      double *y = malloc(n * sizeof(double));

      // y = 1.5x + 2.0 + noise
      for (size_t i = 0; i < n; i++) {
        x[i] = (double)i;
        y[i] = 1.5 * x[i] + 2.0 + ((double)rand() / RAND_MAX - 0.5) * 0.1;
      }

      ols_result_t result = ols_fit(y, x, n);

      check_double_eq(result.slope, 1.5, 0.01);
      check_double_eq(result.intercept, 2.0, 0.1);

      free(x);
      free(y);
    }
  }

  describe("SIMD Reverse") {
    it("should reverse a small vector") {
      double input[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      double output[5];

      simd_reverse(input, output, 5);

      check_double_eq(output[0], 5.0, TEST_TOLERANCE);
      check_double_eq(output[1], 4.0, TEST_TOLERANCE);
      check_double_eq(output[2], 3.0, TEST_TOLERANCE);
      check_double_eq(output[3], 2.0, TEST_TOLERANCE);
      check_double_eq(output[4], 1.0, TEST_TOLERANCE);
    }

    it("should reverse a single element") {
      double input[] = {42.0};
      double output[1];

      simd_reverse(input, output, 1);

      check_double_eq(output[0], 42.0, TEST_TOLERANCE);
    }

    it("should reverse a large vector") {
      const size_t n = 100;
      double *input = malloc(n * sizeof(double));
      double *output = malloc(n * sizeof(double));

      for (size_t i = 0; i < n; i++) {
        input[i] = (double)i;
      }

      simd_reverse(input, output, n);

      for (size_t i = 0; i < n; i++) {
        check_double_eq(output[i], (double)(n - 1 - i), TEST_TOLERANCE);
      }

      free(input);
      free(output);
    }
  }

  describe("Skewness") {
    it("should be near zero for symmetric data") {
      double data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
      double skew = exprtk_skewness(data, 9);
      check(fabs(skew) < 0.1);
    }

    it("should handle large datasets") {
      const size_t n = 1000;
      double *data = malloc(n * sizeof(double));
      fill_random(data, n, 0.0, 100.0);

      double skew = exprtk_skewness(data, n);
      check(fabs(skew) < 2.0);

      free(data);
    }
  }

  describe("Kurtosis") {
    it("should be reasonable for normal-like data") {
      const size_t n = 1000;
      double *data = malloc(n * sizeof(double));

      // Box-Muller transform for normal distribution
      for (size_t i = 0; i < n; i += 2) {
        double u1 = (double)rand() / RAND_MAX;
        double u2 = (double)rand() / RAND_MAX;
        data[i] = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
        if (i + 1 < n) {
          data[i + 1] = sqrt(-2.0 * log(u1)) * sin(2.0 * M_PI * u2);
        }
      }

      double kurt = exprtk_kurtosis(data, n);
      check(fabs(kurt) < 1.0);

      free(data);
    }
  }

  describe("Geometric Mean") {
    it("should calculate correctly") {
      double data[] = {1, 2, 4, 8, 16};
      double gm = exprtk_geometric_mean(data, 5);
      double expected = pow(1.0 * 2.0 * 4.0 * 8.0 * 16.0, 1.0 / 5.0);
      check_double_eq(gm, expected, TEST_TOLERANCE);
    }

    it("should handle large datasets") {
      const size_t n = 500;
      double *data = malloc(n * sizeof(double));
      fill_random(data, n, 1.0, 100.0);

      double gm = exprtk_geometric_mean(data, n);
      check(gm > 0 && gm < 100.0);

      free(data);
    }
  }

  describe("Harmonic Mean") {
    it("should calculate correctly") {
      double data[] = {1, 2, 4};
      double hm = exprtk_harmonic_mean(data, 3);
      double expected = 3.0 / (1.0 / 1.0 + 1.0 / 2.0 + 1.0 / 4.0);
      check_double_eq(hm, expected, TEST_TOLERANCE);
    }

    it("should handle large datasets") {
      const size_t n = 500;
      double *data = malloc(n * sizeof(double));
      fill_random(data, n, 1.0, 100.0);

      double hm = exprtk_harmonic_mean(data, n);
      check(hm > 0 && hm < 100.0);

      free(data);
    }
  }

  describe("Matrix Multiplication") {
    it("should multiply by identity correctly") {
      double A[] = {1, 2, 3, 4}; // 2x2
      double I[] = {1, 0, 0, 1}; // 2x2 identity
      double result[4];

      exprtk_matmul(A, I, 2, 2, 2, result);

      for (int i = 0; i < 4; i++) {
        check_double_eq(result[i], A[i], TEST_TOLERANCE);
      }
    }

    it("should multiply matrices correctly") {
      // [1 2]   [5 6]   [19 22]
      // [3 4] * [7 8] = [43 50]
      double A[] = {1, 2, 3, 4};
      double B[] = {5, 6, 7, 8};
      double expected[] = {19, 22, 43, 50};
      double result[4];

      exprtk_matmul(A, B, 2, 2, 2, result);

      for (int i = 0; i < 4; i++) {
        check_double_eq(result[i], expected[i], TEST_TOLERANCE);
      }
    }

    it("should handle large matrices") {
      const size_t n = 16;
      double *A = malloc(n * n * sizeof(double));
      double *B = malloc(n * n * sizeof(double));
      double *C = malloc(n * n * sizeof(double));

      fill_random(A, n * n, -10.0, 10.0);
      fill_random(B, n * n, -10.0, 10.0);

      exprtk_matmul(A, B, n, n, n, C);

      // Verify one element manually
      double expected = 0;
      for (size_t k = 0; k < n; k++) {
        expected += A[k] * B[k * n];
      }
      check_double_eq(C[0], expected, 1e-5);

      free(A);
      free(B);
      free(C);
    }
  }

  describe("Matrix Transpose") {
    it("should transpose square matrix") {
      double A[] = {1, 2, 3, 4, 5, 6, 7, 8, 9}; // 3x3
      double expected[] = {1, 4, 7, 2, 5, 8, 3, 6, 9};
      double result[9];

      exprtk_transpose(A, 3, 3, result);

      for (int i = 0; i < 9; i++) {
        check_double_eq(result[i], expected[i], TEST_TOLERANCE);
      }
    }

    it("should transpose rectangular matrix") {
      double A[] = {1, 2, 3, 4, 5, 6};        // 2x3
      double expected[] = {1, 4, 2, 5, 3, 6}; // 3x2
      double result[6];

      exprtk_transpose(A, 2, 3, result);

      for (int i = 0; i < 6; i++) {
        check_double_eq(result[i], expected[i], TEST_TOLERANCE);
      }
    }

    it("should handle large matrices") {
      const size_t rows = 32, cols = 24;
      double *A = malloc(rows * cols * sizeof(double));
      double *B = malloc(rows * cols * sizeof(double));

      fill_random(A, rows * cols, -10.0, 10.0);
      exprtk_transpose(A, rows, cols, B);

      // Verify: B[j * rows + i] = A[i * cols + j]
      check_double_eq(B[0 * rows + 0], A[0 * cols + 0], TEST_TOLERANCE);
      check_double_eq(B[1 * rows + 0], A[0 * cols + 1], TEST_TOLERANCE);
      check_double_eq(B[(cols - 1) * rows + (rows - 1)], A[(rows - 1) * cols + (cols - 1)],
                      TEST_TOLERANCE);

      free(A);
      free(B);
    }
  }

  describe("Gauss-Jordan Inversion") {
    it("should invert identity matrix") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      double I[] = {1, 0, 0, 1}; // 2x2 identity
      double result[4];
      memcpy(result, I, sizeof(I));

      int success = gauss_jordan_invert(result, 2, &arena);
      check(success);

      for (int i = 0; i < 4; i++) {
        check_double_eq(result[i], I[i], TEST_TOLERANCE);
      }

      mem_destroy(&arena);
    }

    it("should invert 2x2 matrix") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // [4 7]  inverse is [0.6 -0.7]
      // [2 6]             [-0.2 0.4]
      double A[] = {4, 7, 2, 6};
      double expected[] = {0.6, -0.7, -0.2, 0.4};

      int success = gauss_jordan_invert(A, 2, &arena);
      check(success);

      for (int i = 0; i < 4; i++) {
        check_double_eq(A[i], expected[i], 1e-9);
      }

      mem_destroy(&arena);
    }
  }

  describe("Vector Functions via Registry") {
    it("should reverse a vector via exprtk_call_internal") {
      // Create environment with arena
      exprtk_env_t env;
      exprtk_env_init(&env);

      // Add math module to env
      extern const exprtk_module_t *exprtk_module_math(void);
      exprtk_env_add_module(&env, exprtk_module_math());

      double input[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      exprtk_value_t vec_arg = {.type = EXPRTK_VAL_VECTOR,
                                .data.vector = {.data = input, .size = 5}};

      exprtk_value_t result = exprtk_call_internal("vec.reverse", 1, &vec_arg, &env, &env.arena);

      printf("Result type: %d (expected %d for VECTOR)\n", result.type, EXPRTK_VAL_VECTOR);
      if (result.type == EXPRTK_VAL_NUMBER) {
        printf("Got number: %f\n", result.data.number);
      }

      check_int_eq(result.type, EXPRTK_VAL_VECTOR);
      if (result.type == EXPRTK_VAL_VECTOR) {
        check_int_eq(result.data.vector.size, 5);
        check_not_null(result.data.vector.data);

        if (result.data.vector.data) {
          printf("Reversed vector: [%f, %f, %f, %f, %f]\n", result.data.vector.data[0],
                 result.data.vector.data[1], result.data.vector.data[2], result.data.vector.data[3],
                 result.data.vector.data[4]);

          check_double_eq(result.data.vector.data[0], 5.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[1], 4.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[2], 3.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[3], 2.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[4], 1.0, TEST_TOLERANCE);
        }
      }

      exprtk_env_free(&env);
    }

    it("should generate vec.range via exprtk_call_internal") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      extern const exprtk_module_t *exprtk_module_math(void);
      exprtk_env_add_module(&env, exprtk_module_math());

      exprtk_value_t arg = {.type = EXPRTK_VAL_NUMBER, .data.number = 5.0};

      exprtk_value_t result = exprtk_call_internal("vec.range", 1, &arg, &env, &env.arena);

      printf("Result type: %d (expected %d for VECTOR)\n", result.type, EXPRTK_VAL_VECTOR);
      if (result.type == EXPRTK_VAL_NUMBER) {
        printf("Got number: %f\n", result.data.number);
      }

      check_int_eq(result.type, EXPRTK_VAL_VECTOR);
      if (result.type == EXPRTK_VAL_VECTOR) {
        check_int_eq(result.data.vector.size, 5);
        check_not_null(result.data.vector.data);

        if (result.data.vector.data) {
          printf("Range vector: [%f, %f, %f, %f, %f]\n", result.data.vector.data[0],
                 result.data.vector.data[1], result.data.vector.data[2], result.data.vector.data[3],
                 result.data.vector.data[4]);

          check_double_eq(result.data.vector.data[0], 0.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[1], 1.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[2], 2.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[3], 3.0, TEST_TOLERANCE);
          check_double_eq(result.data.vector.data[4], 4.0, TEST_TOLERANCE);
        }
      }

      exprtk_env_free(&env);
    }
  }
}
