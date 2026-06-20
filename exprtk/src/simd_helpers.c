/**
 * @file simd_helpers.c
 * @brief SIMD-optimized vector operations
 *
 * Uses SIMDe for portable SIMD generation (transforms to Neon/SSE/WASM automatically),
 * preventing use of standard C scalar loops on non-AVX architectures.
 */

#include "simd_helpers.h"
#include "linalg.h"
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include "exprtk_module.h"


#if defined(_MSC_VER)
    #include <intrin.h>
#endif

/* ========================================================================= */
/* CPU Feature Detection                                                     */
/* ========================================================================= */

int simd_has_avx(void) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 28)) != 0;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("avx");
#else
    return 0;
#endif
}

int simd_has_avx2(void) {
#if defined(_MSC_VER) && (defined(_M_AMD64) || defined(_M_IX86))
    int cpuInfo[4];
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] >= 7) {
        __cpuidex(cpuInfo, 7, 0);
        return (cpuInfo[1] & (1 << 5)) != 0;
    }
    return 0;
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("avx2");
#else
    return 0;
#endif
}

#define SIMD_THRESHOLD 8

/* ========================================================================= */
/* Vector Reductions                                                         */
/* ========================================================================= */

double simd_sum(const double *arr, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += arr[i];
        return sum;
    }
    __m256d acc = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = _mm256_add_pd(acc, _mm256_loadu_pd(&arr[i]));
    }
    double tmp[4];
    _mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; i < n; i++) sum += arr[i];
    return sum;
}

double simd_dot(const double *a, const double *b, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += a[i] * b[i];
        return sum;
    }
    __m256d acc = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        acc = _mm256_fmadd_pd(va, vb, acc);
    }
    double tmp[4];
    _mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; i < n; i++) sum += a[i] * b[i];
    return sum;
}

double simd_norm_sq(const double *a, size_t n) {
    if (n < SIMD_THRESHOLD) {
        double sum = 0;
        for (size_t i = 0; i < n; i++) sum += a[i] * a[i];
        return sum;
    }
    __m256d acc = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        acc = _mm256_fmadd_pd(va, va, acc);
    }
    double tmp[4];
    _mm256_storeu_pd(tmp, acc);
    double sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; i < n; i++) sum += a[i] * a[i];
    return sum;
}

double simd_max(const double *arr, size_t n) {
    if (n == 0) return 0.0;
    if (n < SIMD_THRESHOLD) {
        double mx = arr[0];
        for (size_t i = 1; i < n; i++)
            if (arr[i] > mx) mx = arr[i];
        return mx;
    }

    __m256d acc = _mm256_set1_pd(-INFINITY);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = _mm256_max_pd(acc, _mm256_loadu_pd(&arr[i]));
    }

    double tmp[4];
    _mm256_storeu_pd(tmp, acc);
    double mx = fmax(fmax(tmp[0], tmp[1]), fmax(tmp[2], tmp[3]));

    for (; i < n; i++)
        if (arr[i] > mx) mx = arr[i];
    return mx;
}

double simd_min(const double *arr, size_t n) {
    if (n == 0) return 0.0;
    if (n < SIMD_THRESHOLD) {
        double mn = arr[0];
        for (size_t i = 1; i < n; i++)
            if (arr[i] < mn) mn = arr[i];
        return mn;
    }

    __m256d acc = _mm256_set1_pd(INFINITY);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        acc = _mm256_min_pd(acc, _mm256_loadu_pd(&arr[i]));
    }

    double tmp[4];
    _mm256_storeu_pd(tmp, acc);
    double mn = fmin(fmin(tmp[0], tmp[1]), fmin(tmp[2], tmp[3]));

    for (; i < n; i++)
        if (arr[i] < mn) mn = arr[i];
    return mn;
}

/* ========================================================================= */
/* Vector/Scalar Operations                                                  */
/* ========================================================================= */

void simd_add(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] + b[i];
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        _mm256_storeu_pd(&dst[i], _mm256_add_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] + b[i];
}

void simd_sub(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] - b[i];
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        _mm256_storeu_pd(&dst[i], _mm256_sub_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] - b[i];
}

void simd_scale(const double *a, double *dst, double scale, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] * scale;
        return;
    }
    __m256d vs = _mm256_set1_pd(scale);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        _mm256_storeu_pd(&dst[i], _mm256_mul_pd(va, vs));
    }
    for (; i < n; i++) dst[i] = a[i] * scale;
}

void simd_mul(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] * b[i];
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        _mm256_storeu_pd(&dst[i], _mm256_mul_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] * b[i];
}

void simd_div(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = a[i] / b[i];
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        _mm256_storeu_pd(&dst[i], _mm256_div_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] / b[i];
}

void simd_sqr(const double *in, double *out, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) out[i] = in[i] * in[i];
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&in[i]);
        _mm256_storeu_pd(&out[i], _mm256_mul_pd(v, v));
    }
    for (; i < n; i++) out[i] = in[i] * in[i];
}

void simd_reciprocal(const double *in, double *out, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) out[i] = 1.0 / in[i];
        return;
    }
    __m256d one = _mm256_set1_pd(1.0);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&in[i]);
        _mm256_storeu_pd(&out[i], _mm256_div_pd(one, v));
    }
    for (; i < n; i++) out[i] = 1.0 / in[i];
}

void simd_element_max(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = fmax(a[i], b[i]);
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        _mm256_storeu_pd(&dst[i], _mm256_max_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = fmax(a[i], b[i]);
}

void simd_element_min(const double *a, const double *b, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = fmin(a[i], b[i]);
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        __m256d vb = _mm256_loadu_pd(&b[i]);
        _mm256_storeu_pd(&dst[i], _mm256_min_pd(va, vb));
    }
    for (; i < n; i++) dst[i] = fmin(a[i], b[i]);
}

void simd_element_max_scalar(const double *a, double scalar, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = fmax(a[i], scalar);
        return;
    }
    __m256d vs = _mm256_set1_pd(scalar);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        _mm256_storeu_pd(&dst[i], _mm256_max_pd(va, vs));
    }
    for (; i < n; i++) dst[i] = fmax(a[i], scalar);
}

void simd_element_min_scalar(const double *a, double scalar, double *dst, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = fmin(a[i], scalar);
        return;
    }
    __m256d vs = _mm256_set1_pd(scalar);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(&a[i]);
        _mm256_storeu_pd(&dst[i], _mm256_min_pd(va, vs));
    }
    for (; i < n; i++) dst[i] = fmin(a[i], scalar);
}

void simd_fill(double *dst, double val, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[i] = val;
        return;
    }
    __m256d vs = _mm256_set1_pd(val);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(&dst[i], vs);
    }
    for (; i < n; i++) dst[i] = val;
}

void simd_reverse(const double *src, double *dst, size_t n) {
    if (n == 0) return;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) dst[n - 1 - i] = src[i];
        return;
    }
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&src[i]);
        // Reverse within the 256-bit register
        // AVX doesn't have a direct "reverse 4 doubles" but we can permute.
        // Permute indices 3, 2, 1, 0
        v = _mm256_permute4x64_pd(v, _MM_SHUFFLE(0, 1, 2, 3));
        _mm256_storeu_pd(&dst[n - i - 4], v);
    }
    for (; i < n; i++) dst[n - 1 - i] = src[i];
}

void simd_cumsum(const double *src, double *dst, size_t n) {
    if (n == 0) return;
    double sum = 0;
    // Cumulative sum is hard to parallelize fully with SIMD across lanes,
    // but we can process blocks and add the prefix.
    // For now, a vectorized inner-loop for small blocks or just scalar is common.
    // However, let's use a SIMD-optimized version if possible.
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) {
            sum += src[i];
            dst[i] = sum;
        }
    } else {
        // Blocked version: 1. Sum blocks using SIMD. 2. prefix sum of sums. 3. distribute.
        // Or just use the simple scalar for simplicity as it's inherently serial.
        // Actually, for n large, we can use _mm256_adds_pd but it's not a single instruction.
        for (size_t i = 0; i < n; i++) {
            sum += src[i];
            dst[i] = sum;
        }
    }
}

void simd_diff(const double *src, double *dst, size_t n) {
    if (n <= 1) return;
    size_t out_n = n - 1;
    if (out_n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < out_n; i++) dst[i] = src[i + 1] - src[i];
    } else {
        size_t i = 0;
        for (; i + 4 <= out_n; i += 4) {
            __m256d v1 = _mm256_loadu_pd(&src[i + 1]);
            __m256d v0 = _mm256_loadu_pd(&src[i]);
            _mm256_storeu_pd(&dst[i], _mm256_sub_pd(v1, v0));
        }
        for (; i < out_n; i++) dst[i] = src[i + 1] - src[i];
    }
}

void simd_mean_variance(const double *data, size_t n, double *out_mean, double *out_variance) {
    if (n == 0) { *out_mean = 0; *out_variance = 0; return; }
    if (n == 1) { *out_mean = data[0]; *out_variance = 0; return; }

    double sum = 0, sum_sq = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) {
            sum += data[i];
            sum_sq += data[i] * data[i];
        }
    } else {
        __m256d acc_sum = _mm256_setzero_pd();
        __m256d acc_sq = _mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(&data[i]);
            acc_sum = _mm256_add_pd(acc_sum, v);
            acc_sq = _mm256_fmadd_pd(v, v, acc_sq);
        }
        double tmp_s[4], tmp_sq[4];
        _mm256_storeu_pd(tmp_s, acc_sum);
        _mm256_storeu_pd(tmp_sq, acc_sq);
        sum = tmp_s[0] + tmp_s[1] + tmp_s[2] + tmp_s[3];
        sum_sq = tmp_sq[0] + tmp_sq[1] + tmp_sq[2] + tmp_sq[3];
        for (; i < n; i++) {
            sum += data[i];
            sum_sq += data[i] * data[i];
        }
    }
    double mean = sum / (double)n;
    double var = (sum_sq - (sum * sum) / (double)n) / (double)(n - 1);
    if (var < 0) var = 0;
    *out_mean = mean;
    *out_variance = var;
}

void simd_zscore(const double *in, double *out, size_t n, double mean, double sd) {
    if (sd < 1e-15) {
        memset(out, 0, n * sizeof(double));
        return;
    }
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) out[i] = (in[i] - mean) / sd;
    } else {
        __m256d vm = _mm256_set1_pd(mean);
        __m256d vs = _mm256_set1_pd(1.0 / sd);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d vx = _mm256_loadu_pd(&in[i]);
            _mm256_storeu_pd(&out[i], _mm256_mul_pd(_mm256_sub_pd(vx, vm), vs));
        }
        for (; i < n; i++) out[i] = (in[i] - mean) / sd;
    }
}

void simd_roll_max(const double *src, double *dst, size_t n, size_t win) {
    if (win <= 1 || n == 0) {
        if (src != dst) memcpy(dst, src, n * sizeof(double));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        size_t start = (i >= win - 1) ? i - win + 1 : 0;
        size_t len = i - start + 1;
        dst[i] = simd_max(&src[start], len);
    }
}

void simd_roll_min(const double *src, double *dst, size_t n, size_t win) {
    if (win <= 1 || n == 0) {
        if (src != dst) memcpy(dst, src, n * sizeof(double));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        size_t start = (i >= win - 1) ? i - win + 1 : 0;
        size_t len = i - start + 1;
        dst[i] = simd_min(&src[start], len);
    }
}

/* ========================================================================= */
/* Statistics and Math Helpers                                               */
/* ========================================================================= */

double inv_normal_cdf(double p) {
    if (p <= 0 || p >= 1) return 0;
    const double pp = (p < 0.5) ? p : (1.0 - p);
    const double t = sqrt(-2.0 * log(pp));
    const double c0 = 2.515517;
    const double c1 = 0.802853;
    const double c2 = 0.010328;
    const double d1 = 1.432788;
    const double d2 = 1.89269;
    const double d3 = 0.001308;
    double x = t - (c0 + c1 * t + c2 * t * t) / (1.0 + d1 * t + d2 * t * t + d3 * t * t * t);
    return (p < 0.5) ? -x : x;
}

int compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double*)a;
    double arg2 = *(const double*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

int compare_rank_items(const void *a, const void *b) {
    double v1 = ((const rank_item_t*)a)->val;
    double v2 = ((const rank_item_t*)b)->val;
    if (v1 < v2) return -1;
    if (v1 > v2) return 1;
    return 0;
}

double exprtk_median(const double *data, size_t n, mem_pool_t *arena) {
    if (n == 0) return 0;
    double *sorted = (double*)mem_alloc(arena, n * sizeof(double));
    memcpy(sorted, data, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);
    double res;
    if (n % 2 == 1) res = sorted[n / 2];
    else res = (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    return res;
}

double exprtk_percentile(const double *data, size_t n, double p, mem_pool_t *arena) {
    if (n == 0) return 0;
    p /= 100.0;
    double *sorted = (double*)mem_alloc(arena, n * sizeof(double));
    memcpy(sorted, data, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);
    double res;
    if (n == 1) res = sorted[0];
    else {
        double idx = p * (double)(n - 1);
        size_t lo = (size_t)idx;
        size_t hi = (lo + 1 < n) ? lo + 1 : lo;
        double frac = idx - (double)lo;
        res = sorted[lo] + frac * (sorted[hi] - sorted[lo]);
    }
    return res;
}

double exprtk_geometric_mean(const double *data, size_t n) {
    if (n == 0) return 0;
    double sum_log = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            if (data[i] <= 0) return 0;
            sum_log += log(data[i]);
        }
    } else {
        // Note: log() isn't standard SIMD but many compilers vectorize it or we use sleef/simde.
        // Here we just loop to ensure safety, but compilers with -ffast-math -O3 might vectorize.
        for (size_t i = 0; i < n; ++i) {
            if (data[i] <= 0) return 0;
            sum_log += log(data[i]);
        }
    }
    return exp(sum_log / (double)n);
}

double exprtk_harmonic_mean(const double *data, size_t n) {
    if (n == 0) return 0;
    double sum_inv = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            if (fabs(data[i]) < 1e-15) return 0;
            sum_inv += 1.0 / data[i];
        }
    } else {
        for (size_t i = 0; i < n; ++i) if (fabs(data[i]) < 1e-15) return 0;
        __m256d acc = _mm256_setzero_pd();
        __m256d one = _mm256_set1_pd(1.0);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d vd = _mm256_loadu_pd(&data[i]);
            acc = _mm256_add_pd(acc, _mm256_div_pd(one, vd));
        }
        double tmp[4];
        _mm256_storeu_pd(tmp, acc);
        sum_inv = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        for (; i < n; ++i) sum_inv += 1.0 / data[i];
    }
    return (double)n / sum_inv;
}

double exprtk_skewness(const double *data, size_t n) {
    if (n < 3) return 0;
    double mean, var;
    simd_mean_variance(data, n, &mean, &var);
    double std_dev = sqrt(var);
    if (std_dev < 1e-15) return 0;
    double m3 = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            double d = data[i] - mean;
            m3 += d * d * d;
        }
    } else {
        __m256d v_mean = _mm256_set1_pd(mean);
        __m256d acc3 = _mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d vd = _mm256_sub_pd(_mm256_loadu_pd(&data[i]), v_mean);
            __m256d vd2 = _mm256_mul_pd(vd, vd);
            acc3 = _mm256_fmadd_pd(vd2, vd, acc3);
        }
        double tmp3[4];
        _mm256_storeu_pd(tmp3, acc3);
        m3 = tmp3[0] + tmp3[1] + tmp3[2] + tmp3[3];
        for (; i < n; ++i) {
            double d = data[i] - mean;
            m3 += d * d * d;
        }
    }
    return ((double)n / ((double)(n - 1) * (double)(n - 2))) * (m3 / (std_dev * std_dev * std_dev));
}

double exprtk_kurtosis(const double *data, size_t n) {
    if (n < 4) return 0;
    double mean, var;
    simd_mean_variance(data, n, &mean, &var);
    if (var < 1e-15) return 0;
    double m4 = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            double d = data[i] - mean;
            double d2 = d * d;
            m4 += d2 * d2;
        }
    } else {
        __m256d v_mean = _mm256_set1_pd(mean);
        __m256d acc4 = _mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d vd = _mm256_sub_pd(_mm256_loadu_pd(&data[i]), v_mean);
            __m256d vd2 = _mm256_mul_pd(vd, vd);
            acc4 = _mm256_fmadd_pd(vd2, vd2, acc4);
        }
        double tmp4[4];
        _mm256_storeu_pd(tmp4, acc4);
        m4 = tmp4[0] + tmp4[1] + tmp4[2] + tmp4[3];
        for (; i < n; ++i) {
            double d = data[i] - mean;
            double d2 = d * d;
            m4 += d2 * d2;
        }
    }
    double nn = (double)n;
    return (nn * (nn + 1.0) / ((nn - 1.0) * (nn - 2.0) * (nn - 3.0))) * (m4 / (var * var))
            - 3.0 * (nn - 1.0) * (nn - 1.0) / ((nn - 2.0) * (nn - 3.0));
}

/* ========================================================================= */
/* Matrix and Other Utilities                                                */
/* ========================================================================= */

static void simd_row_major_double_to_col_float(const double *src, size_t rows, size_t cols,
                                               float *dst) {
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c)
            dst[r + c * rows] = (float)src[r * cols + c];
}

static void simd_col_float_to_row_major_double(const float *src, size_t rows, size_t cols,
                                               double *dst) {
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c)
            dst[r * cols + c] = (double)src[r + c * rows];
}

double exprtk_fibonacci(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    double a = 0, b = 1;
    for (int i = 2; i <= n; ++i) { double c = a + b; a = b; b = c; }
    return b;
}

long long exprtk_gcd(long long u, long long v) {
    u = llabs(u); v = llabs(v);
    while (v != 0) { long long r = u % v; u = v; v = r; }
    return u;
}

double exprtk_normal_rand(double mu, double sigma) {
    static int have_next = 0;
    static double next_val = 0;
    if (have_next) {
        have_next = 0;
        return mu + sigma * next_val;
    } else {
        double u1 = (double)rand() / (double)RAND_MAX;
        double u2 = (double)rand() / (double)RAND_MAX;
        if (u1 < 1e-30) u1 = 1e-30;
        double r = sqrt(-2.0 * log(u1));
        double theta = 2.0 * 3.14159265358979323846 * u2;
        next_val = r * sin(theta);
        have_next = 1;
        return mu + sigma * r * cos(theta);
    }
}

double exprtk_det2(const double *A) { return A[0]*A[3] - A[1]*A[2]; }
double exprtk_det3(const double *A) {
    return A[0] * (A[4]*A[8] - A[5]*A[7]) - 
           A[1] * (A[3]*A[8] - A[5]*A[6]) + 
           A[2] * (A[3]*A[7] - A[4]*A[6]);
}

int exprtk_inv2(const double *A, double *out) {
    double det = exprtk_det2(A);
    if (fabs(det) < 1e-15) return 0;
    double idet = 1.0 / det;
    out[0] = A[3] * idet; out[1] = -A[1] * idet;
    out[2] = -A[2] * idet; out[3] = A[0] * idet;
    return 1;
}

int exprtk_inv3(const double *A, double *out) {
    double det = exprtk_det3(A);
    if (fabs(det) < 1e-15) return 0;
    double idet = 1.0 / det;
    out[0] = (A[4]*A[8] - A[5]*A[7]) * idet;
    out[1] = (A[2]*A[7] - A[1]*A[8]) * idet;
    out[2] = (A[1]*A[5] - A[2]*A[4]) * idet;
    out[3] = (A[5]*A[6] - A[3]*A[8]) * idet;
    out[4] = (A[0]*A[8] - A[2]*A[6]) * idet;
    out[5] = (A[2]*A[3] - A[0]*A[5]) * idet;
    out[6] = (A[3]*A[7] - A[4]*A[6]) * idet;
    out[7] = (A[1]*A[6] - A[0]*A[7]) * idet;
    out[8] = (A[0]*A[4] - A[1]*A[3]) * idet;
    return 1;
}

void exprtk_matmul(const double *A, const double *B, size_t m, size_t k, size_t n, double *out) {
    if (!A || !B || !out || m == 0 || k == 0 || n == 0 ||
        m > (size_t)INT_MAX || k > (size_t)INT_MAX || n > (size_t)INT_MAX)
        return;

    float *a = (float *)malloc(m * k * sizeof(float));
    float *b = (float *)malloc(k * n * sizeof(float));
    float *c = (float *)malloc(m * n * sizeof(float));
    if (!a || !b || !c) {
        free(a);
        free(b);
        free(c);
        return;
    }

    simd_row_major_double_to_col_float(A, m, k, a);
    simd_row_major_double_to_col_float(B, k, n, b);
    memset(c, 0, m * n * sizeof(float));
    matmul("N", "N", (int)m, (int)n, (int)k, 1.0f, a, b, 0.0f, c);
    simd_col_float_to_row_major_double(c, m, n, out);

    free(a);
    free(b);
    free(c);
}

void exprtk_transpose(const double *A, size_t rows, size_t cols, double *out) {
    const size_t BLOCK = 8;
    for (size_t i = 0; i < rows; i += BLOCK) {
        for (size_t j = 0; j < cols; j += BLOCK) {
            size_t i_max = (i + BLOCK < rows) ? i + BLOCK : rows;
            size_t j_max = (j + BLOCK < cols) ? j + BLOCK : cols;
            for (size_t ii = i; ii < i_max; ++ii)
                for (size_t jj = j; jj < j_max; ++jj)
                    out[jj * rows + ii] = A[ii * cols + jj];
        }
    }
}

int exprtk_eig2(const double *A, double *ev) {
    double tr = A[0] + A[3];
    double det = A[0] * A[3] - A[1] * A[2];
    double disc = tr * tr - 4.0 * det;
    if (disc < 0) { ev[0] = tr / 2.0; ev[1] = tr / 2.0; return 0; }
    double sq = sqrt(disc);
    ev[0] = (tr + sq) / 2.0;
    ev[1] = (tr - sq) / 2.0;
    return 1;
}

int exprtk_eig3(const double *A, double *ev) {
    double c2 = A[0] + A[4] + A[8];
    double c1 = A[0] * A[4] - A[1] * A[3] + A[0] * A[8] - A[2] * A[6] + A[4] * A[8] - A[5] * A[7];
    double c0 = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
    double c2_3 = c2 / 3.0;
    double p = (3.0 * c1 - c2 * c2) / 3.0;
    double qq = (9.0 * c2 * c1 - 2.0 * c2 * c2 * c2 - 27.0 * c0) / 27.0;
    double disc = qq * qq / 4.0 + p * p * p / 27.0;
    if (disc <= 0) {
        double m = 2.0 * sqrt(-p / 3.0);
        double theta = acos(fmin(1.0, fmax(-1.0, 3.0 * qq / (p * m)))) / 3.0;
        ev[0] = c2_3 + m * cos(theta);
        ev[1] = c2_3 + m * cos(theta - 2.0 * 3.14159265358979323846 / 3.0);
        ev[2] = c2_3 + m * cos(theta - 4.0 * 3.14159265358979323846 / 3.0);
    } else {
        double sq = sqrt(disc);
        double u = pow(-qq / 2.0 + sq, 1.0 / 3.0);
        double v = pow(-qq / 2.0 - sq, 1.0 / 3.0);
        ev[0] = c2_3 + u + v;
        ev[1] = c2_3 - (u + v) / 2.0;
        ev[2] = ev[1];
    }
    return (disc <= 0) ? 3 : 1;
}

void simd_abs(const double *in, double *out, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) out[i] = fabs(in[i]);
    } else {
        __m256d mask = _mm256_set1_pd(-0.0); // Bitmask for sign bit
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(&in[i]);
            _mm256_storeu_pd(&out[i], _mm256_andnot_pd(mask, v));
        }
        for (; i < n; i++) out[i] = fabs(in[i]);
    }
}

void simd_sign(const double *in, double *out, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) out[i] = (in[i] > 0) - (in[i] < 0);
    } else {
        __m256d zero = _mm256_setzero_pd();
        __m256d one = _mm256_set1_pd(1.0);
        __m256d neg_one = _mm256_set1_pd(-1.0);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(&in[i]);
            __m256d pos_mask = _mm256_cmp_pd(v, zero, _CMP_GT_OQ);
            __m256d neg_mask = _mm256_cmp_pd(v, zero, _CMP_LT_OQ);
            __m256d res = _mm256_blendv_pd(zero, one, pos_mask);
            res = _mm256_blendv_pd(res, neg_one, neg_mask);
            _mm256_storeu_pd(&out[i], res);
        }
        for (; i < n; i++) out[i] = (double)((in[i] > 0) - (in[i] < 0));
    }
}

void simd_clip(const double *in, double *out, size_t n, double lo, double hi) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) out[i] = fmin(fmax(in[i], lo), hi);
    } else {
        __m256d v_lo = _mm256_set1_pd(lo);
        __m256d v_hi = _mm256_set1_pd(hi);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d v = _mm256_loadu_pd(&in[i]);
            _mm256_storeu_pd(&out[i], _mm256_min_pd(_mm256_max_pd(v, v_lo), v_hi));
        }
        for (; i < n; i++) out[i] = fmin(fmax(in[i], lo), hi);
    }
}

double exprtk_trace2(const double *A) { return A[0] + A[3]; }

ols_result_t ols_fit(const double *y, const double *x, size_t n) {
    ols_result_t r = {0};
    if (n < 3) return r;
    double sx = simd_sum(x, n), sy = simd_sum(y, n);
    double sxx = simd_dot(x, x, n), sxy = simd_dot(x, y, n);
    double denom = (double)n * sxx - sx * sx;
    if (fabs(denom) < 1e-15) return r;
    r.slope = ((double)n * sxy - sx * sy) / denom;
    r.intercept = (sy - r.slope * sx) / (double)n;
    double sse = 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; ++i) {
            double e = y[i] - (r.intercept + r.slope * x[i]);
            sse += e * e;
        }
    } else {
        __m256d v_int = _mm256_set1_pd(r.intercept), v_slp = _mm256_set1_pd(r.slope), acc = _mm256_setzero_pd();
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m256d vy = _mm256_loadu_pd(&y[i]), vx = _mm256_loadu_pd(&x[i]);
            __m256d e = _mm256_sub_pd(vy, _mm256_fmadd_pd(v_slp, vx, v_int));
            acc = _mm256_fmadd_pd(e, e, acc);
        }
        double tmp[4]; _mm256_storeu_pd(tmp, acc);
        sse = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        for (; i < n; i++) { double e = y[i] - (r.intercept + r.slope * x[i]); sse += e * e; }
    }
    r.res_var = sse / (double)(n - 2);
    double se_slp_sq = r.res_var * (double)n / denom;
    r.t_slope = (se_slp_sq > 0) ? r.slope / sqrt(se_slp_sq) : 0;
    return r;
}

int gauss_jordan_invert(double *mat, size_t n, mem_pool_t *arena) {
    if (n == 0) return 0;
    double *aug = (double*)mem_alloc(arena, n * 2 * n * sizeof(double));
    if (!aug) return 0;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) aug[i * 2 * n + j] = mat[i * n + j];
        for (size_t j = 0; j < n; j++) aug[i * 2 * n + n + j] = (i == j) ? 1.0 : 0.0;
    }
    size_t cols = 2 * n;
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        double max_val = fabs(aug[col * cols + col]);
        for (size_t row = col + 1; row < n; ++row) {
            double v = fabs(aug[row * cols + col]);
            if (v > max_val) { max_val = v; pivot = row; }
        }
        if (max_val < 1e-15) return 0;
        if (pivot != col) {
            size_t j = 0;
            for (; j + 4 <= cols; j += 4) {
                __m256d v1 = _mm256_loadu_pd(&aug[col * cols + j]);
                __m256d v2 = _mm256_loadu_pd(&aug[pivot * cols + j]);
                _mm256_storeu_pd(&aug[col * cols + j], v2);
                _mm256_storeu_pd(&aug[pivot * cols + j], v1);
            }
            for (; j < cols; ++j) {
                double tmp = aug[col * cols + j];
                aug[col * cols + j] = aug[pivot * cols + j];
                aug[pivot * cols + j] = tmp;
            }
        }
        double diag = aug[col * cols + col];
        __m256d v_inv_diag = _mm256_set1_pd(1.0 / diag);
        size_t j = 0;
        for (; j + 4 <= cols; j += 4) {
            __m256d v = _mm256_loadu_pd(&aug[col * cols + j]);
            _mm256_storeu_pd(&aug[col * cols + j], _mm256_mul_pd(v, v_inv_diag));
        }
        for (; j < cols; ++j) aug[col * cols + j] /= diag;

        for (size_t row = 0; row < n; ++row) {
            if (row == col) continue;
            double factor = aug[row * cols + col];
            __m256d v_fac = _mm256_set1_pd(factor);
            size_t k = 0;
            for (; k + 4 <= cols; k += 4) {
                __m256d vr = _mm256_loadu_pd(&aug[row * cols + k]);
                __m256d vc = _mm256_loadu_pd(&aug[col * cols + k]);
                _mm256_storeu_pd(&aug[row * cols + k], _mm256_fnmadd_pd(v_fac, vc, vr));
            }
            for (; k < cols; ++k) aug[row * cols + k] -= factor * aug[col * cols + k];
        }
    }
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) mat[i * n + j] = aug[i * cols + n + j];
    return 1;
}

int simd_any(const double *data, size_t n) {
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) if (fabs(data[i]) > 1e-15) return 1;
        return 0;
    }
    __m256d zero = _mm256_setzero_pd();
    for (size_t i = 0; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&data[i]);
        // Compare with zero. _mm256_cmp_pd returns all 1s if true.
        __m256d cmp = _mm256_cmp_pd(v, zero, _CMP_NEQ_OQ);
        if (_mm256_movemask_pd(cmp) != 0) return 1;
    }
    for (size_t i = (n & ~3); i < n; i++) if (fabs(data[i]) > 1e-15) return 1;
    return 0;
}

int simd_all(const double *data, size_t n) {
    if (n == 0) return 0;
    if (n < SIMD_THRESHOLD) {
        for (size_t i = 0; i < n; i++) if (fabs(data[i]) <= 1e-15) return 0;
        return 1;
    }
    __m256d zero = _mm256_setzero_pd();
    for (size_t i = 0; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(&data[i]);
        __m256d cmp = _mm256_cmp_pd(v, zero, _CMP_NEQ_OQ);
        if (_mm256_movemask_pd(cmp) != 0xF) return 0;
    }
    for (size_t i = (n & ~3); i < n; i++) if (fabs(data[i]) <= 1e-15) return 0;
    return 1;
}

/* --- Legacy compatibility aliases --- */
double simd_sum_avx(const double *data, size_t n) { return simd_sum(data, n); }
double simd_dot_avx(const double *a, const double *b, size_t n) { return simd_dot(a, b, n); }
void simd_scale_avx(const double *in, double *out, double s, size_t n) { simd_scale(in, out, s, n); }
void simd_add_avx(const double *a, const double *b, double *o, size_t n) { simd_add(a, b, o, n); }
void simd_sub_avx(const double *a, const double *b, double *o, size_t n) { simd_sub(a, b, o, n); }
void simd_mul_avx(const double *a, const double *b, double *o, size_t n) { simd_mul(a, b, o, n); }
void simd_zscore_avx(const double *in, double *out, size_t n, double m, double s) { simd_zscore(in, out, n, m, s); }
void simd_mean_variance_avx(const double *data, size_t n, double *m, double *v) { simd_mean_variance(data, n, m, v); }

/* ========================================================================= */
/* Float (f32) SIMD Primitives                                               */
/* ========================================================================= */

float simd_sum_f32(const float *arr, size_t n) {
    if (n < SIMD_THRESHOLD) {
        float sum = 0;
        for (size_t i = 0; i < n; i++) sum += arr[i];
        return sum;
    }
    __m256 acc = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        acc = _mm256_add_ps(acc, _mm256_loadu_ps(&arr[i]));
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, acc);
    float sum = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7];
    for (; i < n; i++) sum += arr[i];
    return sum;
}

float simd_dot_f32(const float *a, const float *b, size_t n) {
    if (n < SIMD_THRESHOLD) {
        float sum = 0;
        for (size_t i = 0; i < n; i++) sum += a[i] * b[i];
        return sum;
    }
    __m256 acc = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        __m256 vb = _mm256_loadu_ps(&b[i]);
        acc = _mm256_fmadd_ps(va, vb, acc);
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, acc);
    float sum = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7];
    for (; i < n; i++) sum += a[i] * b[i];
    return sum;
}

float simd_norm_sq_f32(const float *a, size_t n) {
    return simd_dot_f32(a, a, n);
}

void simd_add_f32(const float *a, const float *b, float *dst, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        __m256 vb = _mm256_loadu_ps(&b[i]);
        _mm256_storeu_ps(&dst[i], _mm256_add_ps(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] + b[i];
}

void simd_sub_f32(const float *a, const float *b, float *dst, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        __m256 vb = _mm256_loadu_ps(&b[i]);
        _mm256_storeu_ps(&dst[i], _mm256_sub_ps(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] - b[i];
}

void simd_scale_f32(const float *a, float *dst, float scale, size_t n) {
    __m256 vs = _mm256_set1_ps(scale);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        _mm256_storeu_ps(&dst[i], _mm256_mul_ps(va, vs));
    }
    for (; i < n; i++) dst[i] = a[i] * scale;
}

void simd_mul_f32(const float *a, const float *b, float *dst, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        __m256 vb = _mm256_loadu_ps(&b[i]);
        _mm256_storeu_ps(&dst[i], _mm256_mul_ps(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] * b[i];
}

void simd_div_f32(const float *a, const float *b, float *dst, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(&a[i]);
        __m256 vb = _mm256_loadu_ps(&b[i]);
        _mm256_storeu_ps(&dst[i], _mm256_div_ps(va, vb));
    }
    for (; i < n; i++) dst[i] = a[i] / b[i];
}

float simd_max_f32(const float *arr, size_t n) {
    if (n == 0) return 0;
    if (n < SIMD_THRESHOLD) {
        float res = arr[0];
        for (size_t i = 1; i < n; i++) if (arr[i] > res) res = arr[i];
        return res;
    }
    __m256 res_v = _mm256_loadu_ps(arr);
    size_t i = 8;
    for (; i + 8 <= n; i += 8) {
        res_v = _mm256_max_ps(res_v, _mm256_loadu_ps(&arr[i]));
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, res_v);
    float res = tmp[0];
    for (int k = 1; k < 8; k++) if (tmp[k] > res) res = tmp[k];
    for (; i < n; i++) if (arr[i] > res) res = arr[i];
    return res;
}

float simd_min_f32(const float *arr, size_t n) {
    if (n == 0) return 0;
    if (n < SIMD_THRESHOLD) {
        float res = arr[0];
        for (size_t i = 1; i < n; i++) if (arr[i] < res) res = arr[i];
        return res;
    }
    __m256 res_v = _mm256_loadu_ps(arr);
    size_t i = 8;
    for (; i + 8 <= n; i += 8) {
        res_v = _mm256_min_ps(res_v, _mm256_loadu_ps(&arr[i]));
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, res_v);
    float res = tmp[0];
    for (int k = 1; k < 8; k++) if (tmp[k] < res) res = tmp[k];
    for (; i < n; i++) if (arr[i] < res) res = arr[i];
    return res;
}

/* ========================================================================= */
/* 2D Graphics / Flex Engine Helpers                                         */
/* ========================================================================= */

void simd_transform_points_f32(const float m[6], const float* in, float* out, size_t n) {
    if (n == 0) return;
    
    __m256 v_m0 = _mm256_set1_ps(m[0]);
    __m256 v_m1 = _mm256_set1_ps(m[1]);
    __m256 v_m2 = _mm256_set1_ps(m[2]); // tx
    __m256 v_m3 = _mm256_set1_ps(m[3]);
    __m256 v_m4 = _mm256_set1_ps(m[4]);
    __m256 v_m5 = _mm256_set1_ps(m[5]); // ty

    size_t i = 0;
    // Process 8 points at a time (16 floats, two AVX registers)
    for (; i + 8 <= n; i += 8) {
        __m256 v0 = _mm256_loadu_ps(&in[i * 2]);
        __m256 v1 = _mm256_loadu_ps(&in[i * 2 + 8]);
        
        // Deinterleave: X=[x0..x7], Y=[y0..y7]
        __m256 x_unord = _mm256_shuffle_ps(v0, v1, 0x88); // [x0 x1 x4 x5 | x2 x3 x6 x7]
        __m256 y_unord = _mm256_shuffle_ps(v0, v1, 0xDD); // [y0 y1 y4 y5 | y2 y3 y6 y7]
        
        // Fix order using 64-bit swaps (permute4x64)
        __m256 x = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(x_unord), 0xD8));
        __m256 y = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(y_unord), 0xD8));
        
        // Transform: x' = m0*x + m1*y + m2, y' = m3*x + m4*y + m5
        __m256 rx = _mm256_fmadd_ps(v_m0, x, _mm256_fmadd_ps(v_m1, y, v_m2));
        __m256 ry = _mm256_fmadd_ps(v_m3, x, _mm256_fmadd_ps(v_m4, y, v_m5));
        
        // Interleave back
        __m256 w0 = _mm256_unpacklo_ps(rx, ry); // [x0 y0 x1 y1 | x4 y4 x5 y5]
        __m256 w1 = _mm256_unpackhi_ps(rx, ry); // [x2 y2 x3 y3 | x6 y6 x7 y7]
        
        __m256 v0_out = _mm256_permute2f128_ps(w0, w1, 0x20); // Lane 0: w0.lo, Lane 1: w1.lo -> [x0 y0 x1 y1 x2 y2 x3 y3]
        __m256 v1_out = _mm256_permute2f128_ps(w0, w1, 0x31); // Lane 0: w0.hi, Lane 1: w1.hi -> [x4 y4 x5 y5 x6 y6 x7 y7]
        
        _mm256_storeu_ps(&out[i * 2], v0_out);
        _mm256_storeu_ps(&out[i * 2 + 8], v1_out);
    }
    // Remainder
    for (; i < n; i++) {
        float px = in[i * 2], py = in[i * 2 + 1];
        out[i * 2]     = m[0] * px + m[1] * py + m[2];
        out[i * 2 + 1] = m[3] * px + m[4] * py + m[5];
    }
}

void simd_compose_transforms_f32(const float m1[6], const float m2[6], float out[6]) {
    out[0] = m1[0] * m2[0] + m1[1] * m2[3];
    out[1] = m1[0] * m2[1] + m1[1] * m2[4];
    out[2] = m1[0] * m2[2] + m1[1] * m2[5] + m1[2];
    out[3] = m1[3] * m2[0] + m1[4] * m2[3];
    out[4] = m1[3] * m2[1] + m1[4] * m2[4];
    out[5] = m1[3] * m2[2] + m1[4] * m2[5] + m1[5];
}

void simd_aabb_points_f32(const float* points, size_t n, float out_min[2], float out_max[2]) {
    if (n == 0) { out_min[0] = out_min[1] = out_max[0] = out_max[1] = 0; return; }
    
    float min_x = points[0], min_y = points[1], max_x = points[0], max_y = points[1];
    size_t i = 0;

    if (n >= 8) {
        __m256 v_min_x = _mm256_set1_ps(1e30f), v_min_y = _mm256_set1_ps(1e30f);
        __m256 v_max_x = _mm256_set1_ps(-1e30f), v_max_y = _mm256_set1_ps(-1e30f);

        for (; i + 8 <= n; i += 8) {
            __m256 v0 = _mm256_loadu_ps(&points[i * 2]);
            __m256 v1 = _mm256_loadu_ps(&points[i * 2 + 8]);
            
            __m256 x_unord = _mm256_shuffle_ps(v0, v1, 0x88);
            __m256 y_unord = _mm256_shuffle_ps(v0, v1, 0xDD);
            __m256 x = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(x_unord), 0xD8));
            __m256 y = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(y_unord), 0xD8));

            v_min_x = _mm256_min_ps(v_min_x, x); v_min_y = _mm256_min_ps(v_min_y, y);
            v_max_x = _mm256_max_ps(v_max_x, x); v_max_y = _mm256_max_ps(v_max_y, y);
        }
        
        float tx[8], ty[8], tX[8], tY[8];
        _mm256_storeu_ps(tx, v_min_x); _mm256_storeu_ps(ty, v_min_y);
        _mm256_storeu_ps(tX, v_max_x); _mm256_storeu_ps(tY, v_max_y);
        
        for (int k = 0; k < 8; k++) {
            if (tx[k] < min_x) min_x = tx[k]; if (ty[k] < min_y) min_y = ty[k];
            if (tX[k] > max_x) max_x = tX[k]; if (tY[k] > max_y) max_y = tY[k];
        }
    }
    for (; i < n; i++) {
        float px = points[i*2], py = points[i*2+1];
        if (px < min_x) min_x = px; if (px > max_x) max_x = px;
        if (py < min_y) min_y = py; if (py > max_y) max_y = py;
    }
    out_min[0] = min_x; out_min[1] = min_y; out_max[0] = max_x; out_max[1] = max_y;
}
