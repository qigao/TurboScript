/**
 * @file simd_helpers.h
 * @brief SIMD-optimized vector operations using AVX/AVX2
 */

#ifndef SIMD_HELPERS_H
#define SIMD_HELPERS_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Represents an item with its original index for ranking/sorting.
 */
typedef struct { size_t idx; double val; } rank_item_t;

/**
 * @brief Result of an Ordinary Least Squares (OLS) linear regression.
 */
typedef struct { double intercept, slope, res_var, t_slope; } ols_result_t;

/**
 * @brief Comparator for sorting doubles.
 */
int compare_doubles(const void *a, const void *b);

/**
 * @brief Comparator for sorting rank_item_t by value.
 */
int compare_rank_items(const void *a, const void *b);

/**
 * @brief Inverse Normal Cumulative Distribution Function.
 * @param p Probability (0, 1)
 * @return Value corresponding to probability p in standard normal distribution.
 */
double inv_normal_cdf(double p);

/**
 * @brief Vectorized sum reduction.
 * @param arr Input array
 * @param n Number of elements
 * @return Sum of all elements
 */
double simd_sum(const double *arr, size_t n);

/**
 * @brief Vectorized dot product of two arrays.
 * @param a First array
 * @param b Second array
 * @param n Number of elements
 * @return Scalar dot product
 */
double simd_dot(const double *a, const double *b, size_t n);

/**
 * @brief Vectorized squared norm (L2 norm squared) of an array.
 * @param a Input array
 * @param n Number of elements
 * @return Sum of squares of elements
 */
double simd_norm_sq(const double *a, size_t n);

/**
 * @brief Vectorized element-wise addition: dst = a + b.
 */
void simd_add(const double *a, const double *b, double *dst, size_t n);

/**
 * @brief Vectorized element-wise subtraction: dst = a - b.
 */
void simd_sub(const double *a, const double *b, double *dst, size_t n);

/**
 * @brief Vectorized scalar scaling: dst = a * scale.
 */
void simd_scale(const double *a, double *dst, double scale, size_t n);

/**
 * @brief Vectorized element-wise multiplication: dst = a * b.
 */
void simd_mul(const double *a, const double *b, double *dst, size_t n);

/**
 * @brief Vectorized element-wise division: dst = a / b.
 */
void simd_div(const double *a, const double *b, double *dst, size_t n);

/**
 * @brief Vectorized element-wise square: out = in * in.
 */
void simd_sqr(const double *in, double *out, size_t n);

/**
 * @brief Vectorized element-wise reciprocal: out = 1.0 / in.
 */
void simd_reciprocal(const double *in, double *out, size_t n);

/**
 * @brief Fill array with a scalar value.
 */
void simd_fill(double *dst, double val, size_t n);

/**
 * @brief Reverse elements of an array.
 */
void simd_reverse(const double *src, double *dst, size_t n);

/**
 * @brief Compute cumulative sum of an array.
 */
void simd_cumsum(const double *src, double *dst, size_t n);

/**
 * @brief Compute first-order difference of an array: dst[i] = src[i+1] - src[i].
 * @param src Input array (size n)
 * @param dst Output array (size n-1)
 * @param n size of input array
 */
void simd_diff(const double *src, double *dst, size_t n);

/**
 * @brief Compute rolling maximum over a window.
 */
void simd_roll_max(const double *src, double *dst, size_t n, size_t win);

/**
 * @brief Compute rolling minimum over a window.
 */
void simd_roll_min(const double *src, double *dst, size_t n, size_t win);

/**
 * @brief Z-score normalization of an array.
 */
void simd_zscore(const double *in, double *out, size_t n, double mean, double sd);

/**
 * @brief Single-pass calculation of mean and variance.
 */
void simd_mean_variance(const double *data, size_t n, double *out_mean, double *out_variance);

/**
 * @brief Logical OR reduction of non-zero elements.
 * @return 1 if any element is non-zero, 0 otherwise.
 */
int simd_any(const double *data, size_t n);

/**
 * @brief Logical AND reduction of non-zero elements.
 * @return 1 if all elements are non-zero, 0 otherwise.
 */
int simd_all(const double *data, size_t n);

/**
 * @brief Vectorized absolute value.
 */
void simd_abs(const double *in, double *out, size_t n);

/**
 * @brief Vectorized sign function (-1, 0, 1).
 */
void simd_sign(const double *in, double *out, size_t n);

/**
 * @brief Vectorized clipping (clamp) to range [lo, hi].
 */
void simd_clip(const double *in, double *out, size_t n, double lo, double hi);

/**
 * @brief Maximum value in an array.
 */
double simd_max(const double *arr, size_t n);

/**
 * @brief Minimum value in an array.
 */
double simd_min(const double *arr, size_t n);

/**
 * @brief Vectorized element-wise maximum: dst = max(a, b).
 */
void simd_element_max(const double *a, const double *b, double *dst, size_t n);

/**
 * @brief Vectorized element-wise minimum: dst = min(a, b).
 */
void simd_element_min(const double *a, const double *b, double *dst, size_t n);

/**
 * @brief Vectorized scalar-wise maximum: dst = max(a, scalar).
 */
void simd_element_max_scalar(const double *a, double scalar, double *dst, size_t n);

/**
 * @brief Vectorized scalar-wise minimum: dst = min(a, scalar).
 */
void simd_element_min_scalar(const double *a, double scalar, double *dst, size_t n);

/* ========================================================================= */
/* Float (f32) SIMD Primitives                                               */
/* ========================================================================= */

/**
 * @brief Vectorized sum reduction (f32).
 */
float simd_sum_f32(const float *arr, size_t n);

/**
 * @brief Vectorized dot product (f32).
 */
float simd_dot_f32(const float *a, const float *b, size_t n);

/**
 * @brief Vectorized squared norm (f32).
 */
float simd_norm_sq_f32(const float *a, size_t n);

/**
 * @brief Vectorized element-wise addition: dst = a + b (f32).
 */
void simd_add_f32(const float *a, const float *b, float *dst, size_t n);

/**
 * @brief Vectorized element-wise subtraction: dst = a - b (f32).
 */
void simd_sub_f32(const float *a, const float *b, float *dst, size_t n);

/**
 * @brief Vectorized scalar scaling: dst = a * scale (f32).
 */
void simd_scale_f32(const float *a, float *dst, float scale, size_t n);

/**
 * @brief Vectorized element-wise multiplication: dst = a * b (f32).
 */
void simd_mul_f32(const float *a, const float *b, float *dst, size_t n);

/**
 * @brief Vectorized element-wise division: dst = a / b (f32).
 */
void simd_div_f32(const float *a, const float *b, float *dst, size_t n);

/**
 * @brief Maximum value in a float array.
 */
float simd_max_f32(const float *arr, size_t n);

/**
 * @brief Minimum value in a float array.
 */
float simd_min_f32(const float *arr, size_t n);

/* ========================================================================= */
/* 2D Graphics / Flex Engine Helpers                                         */
/* ========================================================================= */

/**
 * @brief Apply 2D affine transform to an array of points.
 * @param m Transform matrix [m0 m1 m2 m3 m4 m5] (affine 3x3 with implicit bottom row)
 * @param in_xy Input points (interleaved [x0 y0 x1 y1 ...])
 * @param out_xy Output points (interleaved)
 * @param n Number of points
 */
void simd_transform_points_f32(const float m[6], const float* in_xy, float* out_xy, size_t n);

/**
 * @brief Compose two 2D affine transforms: out = m1 * m2.
 * @param m1 First transform (3x3 with implicit bottom row [0 0 1])
 * @param m2 Second transform
 * @param out Resulting transform [m0 m1 m2 m3 m4 m5]
 */
void simd_compose_transforms_f32(const float m1[6], const float m2[6], float out[6]);

/**
 * @brief Find Axis-Aligned Bounding Box (AABB) of a set of points.
 * @param points Input points (interleaved [x0 y0 x1 y1 ...])
 * @param n Number of points
 * @param out_min Output [min_x, min_y]
 * @param out_max Output [max_x, max_y]
 */
void simd_aabb_points_f32(const float* points, size_t n, float out_min[2], float out_max[2]);

/**
 * @brief Compute median of array elements using quickselect.
 */
double exprtk_median(const double *data, size_t n, mem_pool_t *arena);

/**
 * @brief Compute p-th percentile of array elements.
 */
double exprtk_percentile(const double *data, size_t n, double p, mem_pool_t *arena);

/**
 * @brief Compute geometric mean of an array.
 */
double exprtk_geometric_mean(const double *data, size_t n);

/**
 * @brief Compute harmonic mean of an array.
 */
double exprtk_harmonic_mean(const double *data, size_t n);

/**
 * @brief Compute sample skewness.
 */
double exprtk_skewness(const double *data, size_t n);

/**
 * @brief Compute sample excess kurtosis.
 */
double exprtk_kurtosis(const double *data, size_t n);

/**
 * @brief Compute n-th Fibonacci number.
 */
double exprtk_fibonacci(int n);

/**
 * @brief Compute greatest common divisor.
 */
long long exprtk_gcd(long long u, long long v);

/**
 * @brief Generate a normal random number.
 */
double exprtk_normal_rand(double mu, double sigma);

/**
 * @brief Determinant of a 2x2 matrix.
 */
double exprtk_det2(const double *A);

/**
 * @brief Determinant of a 3x3 matrix.
 */
double exprtk_det3(const double *A);

/**
 * @brief Inverse of a 2x2 matrix.
 */
int exprtk_inv2(const double *A, double *out);

/**
 * @brief Inverse of a 3x3 matrix.
 */
int exprtk_inv3(const double *A, double *out);

/**
 * @brief Matrix multiplication: out = A * B.
 */
void exprtk_matmul(const double *A, const double *B, size_t m, size_t k, size_t n, double *out);

/**
 * @brief Matrix transpose: out = A^T.
 */
void exprtk_transpose(const double *A, size_t rows, size_t cols, double *out);

/**
 * @brief Eigenvalues of a 2x2 symmetric matrix.
 */
int exprtk_eig2(const double *A, double *ev);

/**
 * @brief Eigenvalues of a 3x3 symmetric matrix.
 */
int exprtk_eig3(const double *A, double *ev);

/**
 * @brief Trace of a 2x2 matrix.
 */
double exprtk_trace2(const double *A);

/**
 * @brief Fits an OLS model y = alpha + beta*x + e.
 * @return Struct containing slope, intercept, and statistical metrics.
 */
ols_result_t ols_fit(const double *y, const double *x, size_t n);

/**
 * @brief In-place matrix inversion using Gauss-Jordan elimination.
 * @return 1 on success, 0 if matrix is singular.
 */
int gauss_jordan_invert(double *mat, size_t n, mem_pool_t *arena);



/**
 * @brief Vectorized sum using AVX (processes 4 doubles at a time)
 * @param data Input array
 * @param n Array size
 * @return Sum of all elements
 */
double simd_sum_avx(const double *data, size_t n);

/**
 * @brief Vectorized dot product using AVX
 * @param a First vector
 * @param b Second vector
 * @param n Vector size
 * @return Dot product a·b
 */
double simd_dot_avx(const double *a, const double *b, size_t n);

/**
 * @brief Vectorized scalar multiplication using AVX
 * @param in Input array
 * @param out Output array
 * @param scalar Scalar multiplier
 * @param n Array size
 */
void simd_scale_avx(const double *in, double *out, double scalar, size_t n);

/**
 * @brief Vectorized element-wise addition using AVX
 * @param a First vector
 * @param b Second vector
 * @param out Output vector (a + b)
 * @param n Vector size
 */
void simd_add_avx(const double *a, const double *b, double *out, size_t n);

/**
 * @brief Vectorized element-wise subtraction using AVX
 * @param a First vector
 * @param b Second vector
 * @param out Output vector (a - b)
 * @param n Vector size
 */
void simd_sub_avx(const double *a, const double *b, double *out, size_t n);

/**
 * @brief Vectorized element-wise multiplication using AVX
 * @param a First vector
 * @param b Second vector
 * @param out Output vector (a * b)
 * @param n Vector size
 */
void simd_mul_avx(const double *a, const double *b, double *out, size_t n);

/**
 * @brief Vectorized z-score normalization using AVX
 * @param in Input array
 * @param out Output array (z-scores)
 * @param n Array size
 * @param mean Mean value
 * @param sd Standard deviation
 */
void simd_zscore_avx(const double *in, double *out, size_t n, double mean, double sd);

/**
 * @brief Vectorized mean and variance calculation (single pass)
 * @param data Input array
 * @param n Array size
 * @param out_mean Output: mean value
 * @param out_variance Output: variance value
 */
void simd_mean_variance_avx(const double *data, size_t n, double *out_mean, double *out_variance);

/**
 * @brief Check if AVX is available at runtime
 * @return 1 if AVX is supported, 0 otherwise
 */
int simd_has_avx(void);

/**
 * @brief Check if AVX2 is available at runtime
 * @return 1 if AVX2 is supported, 0 otherwise
 */
int simd_has_avx2(void);

#ifdef __cplusplus
}
#endif

#endif /* SIMD_HELPERS_H */
