/**
 * @file test_simd_helpers.c
 * @brief Unit tests for SIMD optimized functions.
 */

#include "tinytest.h"
#include "simd_helpers.h"
#include <stdlib.h>
#include <math.h>

#define TEST_TOLERANCE 1e-6f
#define DBL_TOLERANCE 1e-10

spec("SIMD Helpers") {
    describe("F32 Arithmetic") {
        it("should sum float array correctly") {
            float arr[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
            float expected = 55.0f;
            check_double_eq(simd_sum_f32(arr, 10), (double)expected, TEST_TOLERANCE);
        }

        it("should compute dot product correctly") {
            float a[] = {1.0f, 2.0f, 3.0f, 4.0f};
            float b[] = {0.5f, 0.5f, 0.5f, 0.5f};
            float expected = (1.0f + 2.0f + 3.0f + 4.0f) * 0.5f;
            check_double_eq(simd_dot_f32(a, b, 4), (double)expected, TEST_TOLERANCE);
        }

        it("should add two float arrays element-wise") {
            float a[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
            float b[] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
            float dst[9];
            simd_add_f32(a, b, dst, 9);
            for(int i=0; i<9; i++) check_double_eq(dst[i], 10.0, TEST_TOLERANCE);
        }
    }

    describe("Double Arithmetic") {
        it("should divide element-wise") {
            double a[] = {10.0, 20.0, 30.0};
            double b[] = {2.0, 4.0, 5.0};
            double dst[3];
            simd_div(a, b, dst, 3);
            check_double_eq(dst[0], 5.0, DBL_TOLERANCE);
            check_double_eq(dst[1], 5.0, DBL_TOLERANCE);
            check_double_eq(dst[2], 6.0, DBL_TOLERANCE);
        }

        it("should square element-wise") {
            double a[] = {1.0, 2.0, 3.0, 4.0};
            double dst[4];
            simd_sqr(a, dst, 4);
            check_double_eq(dst[0], 1.0, DBL_TOLERANCE);
            check_double_eq(dst[1], 4.0, DBL_TOLERANCE);
            check_double_eq(dst[2], 9.0, DBL_TOLERANCE);
            check_double_eq(dst[3], 16.0, DBL_TOLERANCE);
        }
    }

    describe("Flex Graphics Helpers") {
        it("should transform 2D points with affine matrix") {
            // Matrix: [ 2.0  0.0  10.0 ] (Scale = 2, Translate = 10, 20)
            //         [ 0.0  2.0  20.0 ]
            float m[6] = {2.0f, 0.0f, 10.0f, 0.0f, 2.0f, 20.0f};
            float pts[] = { 1.0f, 2.0f,  // Point 0 (1, 2)
                           -1.0f, 5.0f}; // Point 1 (-1, 5)
            float out[4];
            
            simd_transform_points_f32(m, pts, out, 2);
            
            // Expected Point 0: 2*1 + 0*2 + 10 = 12, 0*1 + 2*2 + 20 = 24
            check_double_eq(out[0], 12.0, TEST_TOLERANCE);
            check_double_eq(out[1], 24.0, TEST_TOLERANCE);
            // Expected Point 1: 2*-1 + 10 = 8, 2*5 + 20 = 30
            check_double_eq(out[2], 8.0, TEST_TOLERANCE);
            check_double_eq(out[3], 30.0, TEST_TOLERANCE);
        }

        it("should compute AABB for points") {
            float pts[] = { 10.0f, 20.0f, 
                           -5.0f, 30.0f, 
                            25.0f, -10.0f,
                            0.0f, 0.0f };
            float min_xy[2], max_xy[2];
            
            simd_aabb_points_f32(pts, 4, min_xy, max_xy);
            
            check_double_eq(min_xy[0], -5.0, TEST_TOLERANCE);
            check_double_eq(min_xy[1], -10.0, TEST_TOLERANCE);
            check_double_eq(max_xy[0], 25.0, TEST_TOLERANCE);
            check_double_eq(max_xy[1], 30.0, TEST_TOLERANCE);
        }

        it("should compose two transforms correctly") {
            // T1: Scale by 2
            float t1[6] = {2.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f};
            // T2: Translate by (10, 20)
            float t2[6] = {1.0f, 0.0f, 10.0f, 0.0f, 1.0f, 20.0f};
            
            float out[6];
            simd_compose_transforms_f32(t2, t1, out); // Translate * Scale
            
            // Expected: [ 2.0 0.0 10.0 ]
            //           [ 0.0 2.0 20.0 ]
            check_double_eq(out[0], 2.0, TEST_TOLERANCE);
            check_double_eq(out[2], 10.0, TEST_TOLERANCE);
            check_double_eq(out[4], 2.0, TEST_TOLERANCE);
            check_double_eq(out[5], 20.0, TEST_TOLERANCE);
        }
    }
}
