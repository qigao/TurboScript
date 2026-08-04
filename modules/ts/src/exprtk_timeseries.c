/**
 * @file exprtk_timeseries.c
 * @brief Timeseries analysis implementations
 */
#include "ts.h"
#include "ts_internal.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_buffer.h>
#include "simd_helpers.h"

/* Standard memory management via MEM_ALLOC_ARRAY from turbo_buffer.h is used. */

// zn_normalize is a wrapper around simd_zscore
static void zn_normalize(const double *in, size_t n, double *out) {
    double mean, var;
    simd_mean_variance(in, n, &mean, &var);
    double std = (var > 0) ? sqrt(var) : 1e-9;
    simd_zscore(in, out, n, mean, std);
}

// Helper for candle features
static void get_candle_features(const double *O, const double *H, const double *L, const double *C, size_t n, double *out) {
    for (size_t i = 0; i < n; ++i) {
        double range = H[i] - L[i];
        if (range < 1e-9) {
            out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = 0;
            continue;
        }
        out[i * 3] = (C[i] - O[i]) / range;
        out[i * 3 + 1] = (H[i] - fmax(O[i], C[i])) / range;
        out[i * 3 + 2] = (fmin(O[i], C[i]) - L[i]) / range;
    }
}

// Helper for DTW
static double dtw_dist(const double *a, const double *b, size_t m, double *cost_mat) {
#define COST(r, c) cost_mat[(r) * (m + 1) + (c)]
    for (size_t i = 0; i <= m; ++i) {
        COST(i, 0) = 1e30;
        COST(0, i) = 1e30;
    }
    COST(0, 0) = 0;
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            double d = (a[i - 1] - b[j - 1]);
            double diff = d * d;
            double min_prev = fmin(COST(i - 1, j), fmin(COST(i, j - 1), COST(i - 1, j - 1)));
            COST(i, j) = diff + min_prev;
        }
    }
    return COST(m, m);
#undef COST
}

size_t exprtk_ts_diff(const double *data, size_t n, size_t order, double *out, mem_pool_t *arena) {
    if (n <= order) return 0;
    double *tmp = MEM_ALLOC_ARRAY(arena, double, n); memcpy(tmp, data, n * sizeof(double));
    for (size_t o = 0; o < order; ++o) {
        for (size_t i = n - 1; i > o; --i) tmp[i] = tmp[i] - tmp[i - 1];
    }
    for (size_t i = 0; i < n; ++i) out[i] = (i < order) ? 0 : tmp[i];
    return n - order;
}

size_t exprtk_ts_autocorr(const double *data, size_t n, size_t max_lag, double *out) {
    if (n < 2 || max_lag >= n) return 0;
    double mean, var;
    simd_mean_variance(data, n, &mean, &var);
    if (var < 1e-15) { for (size_t l = 0; l <= max_lag; ++l) out[l] = 1.0; return max_lag + 1; }
    
    double denom_sum_sq = var * (n - 1);
    for (size_t l = 0; l <= max_lag; ++l) {
        double cv = 0;
        for (size_t j = l; j < n; ++j) cv += (data[j] - mean) * (data[j - l] - mean);
        out[l] = cv / denom_sum_sq;
    }
    return max_lag + 1;
}

size_t exprtk_ts_pacf(const double *data, size_t n, size_t max_lag, double *out, mem_pool_t *arena) {
    if (n < 2 || max_lag >= n) return 0;
    double *rho = MEM_ALLOC_ARRAY(arena, double, max_lag + 1);

    exprtk_ts_autocorr(data, n, max_lag, rho);
    out[0] = 1.0; if (max_lag == 0) { return 1; }
    out[1] = rho[1];
    double *phi = MEM_ALLOC_ARRAY(arena, double, (max_lag + 1) * (max_lag + 1));
    if (phi) memset(phi, 0, (max_lag + 1) * (max_lag + 1) * sizeof(double));

    phi[1 * (max_lag + 1) + 1] = rho[1];
    for (size_t k = 2; k <= max_lag; ++k) {
        double num = rho[k];
        for (size_t j = 1; j < k; ++j) { num -= phi[(k - 1) * (max_lag + 1) + j] * rho[k - j]; }
        double den_dl = 1.0;
        for (size_t j = 1; j < k; ++j) den_dl -= phi[(k - 1) * (max_lag + 1) + j] * rho[j];
        phi[k * (max_lag + 1) + k] = num / den_dl;
        for (size_t j = 1; j < k; ++j) phi[k * (max_lag + 1) + j] = phi[(k - 1) * (max_lag + 1) + j] - phi[k * (max_lag + 1) + k] * phi[(k - 1) * (max_lag + 1) + (k - j)];
        out[k] = phi[k * (max_lag + 1) + k];
    }
    return max_lag + 1;
}

int exprtk_ts_adf(const double *data, size_t n, size_t p, double *out, mem_pool_t *arena) {
    if (n <= p + 2) return 0;
    size_t m = n - 1 - p;
    double *dy = MEM_ALLOC_ARRAY(arena, double, m);
    double *y_lag = MEM_ALLOC_ARRAY(arena, double, m);
    double *X = MEM_ALLOC_ARRAY(arena, double, m * (p + 1));

    for (size_t i = 0; i < m; ++i) {
        dy[i] = data[i + p + 1] - data[i + p]; y_lag[i] = data[i + p];
        X[i * (p + 1)] = y_lag[i];
        for (size_t j = 1; j <= p; ++j) X[i * (p + 1) + j] = data[i + p + 1 - j] - data[i + p - j];
    }
    ols_result_t r = ols_fit(dy, y_lag, m);
    out[0] = r.t_slope; out[1] = r.slope;
}

size_t exprtk_ts_garch(const double *returns, size_t n, double alpha, double beta, double *out) {
    if (n == 0) return 0;
    double var = 0; for (size_t i = 0; i < n; ++i) var += returns[i] * returns[i];
    var /= (double)n; double omega = var * (1.0 - alpha - beta);
    if (omega < 0) omega = 1e-6;
    out[0] = var;
    for (size_t i = 1; i < n; ++i) out[i] = omega + alpha * returns[i - 1] * returns[i - 1] + beta * out[i - 1];
    return n;
}

double exprtk_ts_hurst(const double *data, size_t n, double *out, mem_pool_t *arena) {
    if (n < 8) return 0;
    size_t m = 0; for (size_t k = 4; k <= n / 2; k *= 2) m++;
    double *x = MEM_ALLOC_ARRAY(arena, double, m);
    double *y = MEM_ALLOC_ARRAY(arena, double, m);
    size_t idx = 0;
    for (size_t k = 4; k <= n / 2; k *= 2) {
        size_t num_blocks = n / k; double sum_rs = 0;
        for (size_t b = 0; b < num_blocks; ++b) {
            double b_sum = 0; for (size_t i = 0; i < k; ++i) b_sum += data[b * k + i];
            double b_mean = b_sum / (double)k, b_var = 0;
            double *cum = MEM_ALLOC_ARRAY(arena, double, k);
            double c_sum = 0;
            for (size_t i = 0; i < k; ++i) {
                double d = data[b * k + i] - b_mean; b_var += d * d;
                c_sum += d; cum[i] = c_sum;
            }
            double min_c = cum[0], max_c = cum[0];
            for (size_t i = 1; i < k; ++i) { if (cum[i] < min_c) min_c = cum[i]; if (cum[i] > max_c) max_c = cum[i]; }
            double s = sqrt(b_var / (double)k);
            if (s > 1e-15) sum_rs += (max_c - min_c) / s;
        }
        x[idx] = log((double)k); y[idx] = log(sum_rs / (double)num_blocks); idx++;
    }
    ols_result_t r = ols_fit(y, x, m);
    return r.slope;
}
static double inline_simd_l2sq(const double *a, const double *b, size_t m) {
    if (m == 0) return 0;
    double dist = 0;
    size_t j = 0;
    simde__m256d v_dist = simde_mm256_setzero_pd();
    for (; j + 4 <= m; j += 4) {
        simde__m256d v_a = simde_mm256_loadu_pd(&a[j]);
        simde__m256d v_b = simde_mm256_loadu_pd(&b[j]);
        simde__m256d v_diff = simde_mm256_sub_pd(v_a, v_b);
        v_dist = simde_mm256_add_pd(v_dist, simde_mm256_mul_pd(v_diff, v_diff));
    }
    double tmp[4]; simde_mm256_storeu_pd(tmp, v_dist);
    dist = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    for (; j < m; ++j) {
        double diff = a[j] - b[j];
        dist += diff * diff;
    }
    return dist;
}

static double inline_simd_cos(const double *a, const double *b, size_t m) {
    if (m == 0) return 1.0;
    double dot = 0, norm_a = 0, norm_b = 0;
    size_t j = 0;
    simde__m256d v_dot = simde_mm256_setzero_pd();
    simde__m256d v_na = simde_mm256_setzero_pd();
    simde__m256d v_nb = simde_mm256_setzero_pd();
    for (; j + 4 <= m; j += 4) {
        simde__m256d v_a = simde_mm256_loadu_pd(&a[j]);
        simde__m256d v_b = simde_mm256_loadu_pd(&b[j]);
        v_dot = simde_mm256_add_pd(v_dot, simde_mm256_mul_pd(v_a, v_b));
        v_na = simde_mm256_add_pd(v_na, simde_mm256_mul_pd(v_a, v_a));
        v_nb = simde_mm256_add_pd(v_nb, simde_mm256_mul_pd(v_b, v_b));
    }
    double tmp_dot[4], tmp_na[4], tmp_nb[4];
    simde_mm256_storeu_pd(tmp_dot, v_dot);
    simde_mm256_storeu_pd(tmp_na, v_na);
    simde_mm256_storeu_pd(tmp_nb, v_nb);
    dot = tmp_dot[0] + tmp_dot[1] + tmp_dot[2] + tmp_dot[3];
    norm_a = tmp_na[0] + tmp_na[1] + tmp_na[2] + tmp_na[3];
    norm_b = tmp_nb[0] + tmp_nb[1] + tmp_nb[2] + tmp_nb[3];
    for (; j < m; ++j) {
        dot += a[j] * b[j];
        norm_a += a[j] * a[j];
        norm_b += b[j] * b[j];
    }
    return (norm_a > 0 && norm_b > 0) ? (1.0 - dot / sqrt(norm_a * norm_b)) : 1.0;
}

size_t exprtk_ts_match(const double *data, const double *pattern, size_t n, size_t m, double *out) {
    if (n < m || m == 0) return 0;
    
    // We compute the squared Euclidean distance between the pattern and each window in data
    for (size_t i = 0; i <= n - m; ++i) {
        out[i] = inline_simd_l2sq(&data[i], pattern, m);
    }
    
    // Zero out the rest of the output vector
    for (size_t i = n - m + 1; i < n; ++i) {
        out[i] = 0;
    }
    
    return n - m + 1;
}
size_t exprtk_ts_match_cosine(const double *data, const double *pattern, size_t n, size_t m, double *out) {
    if (n < m || m == 0) return 0;
    for (size_t i = 0; i <= n - m; ++i) {
        out[i] = inline_simd_cos(&data[i], pattern, m);
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_normalized(const double *data, const double *pattern, size_t n, size_t m, double *out, mem_pool_t *arena) {
    if (n < m || m == 0) return 0;
    double *p_norm = (double *)MEM_ALLOC_ARRAY(arena, double, m);
    double *w_norm = (double *)MEM_ALLOC_ARRAY(arena, double, m);
    if (!p_norm || !w_norm) return 0;
    
    zn_normalize(pattern, m, p_norm);
    
    for (size_t i = 0; i <= n - m; ++i) {
        zn_normalize(&data[i], m, w_norm);
        out[i] = inline_simd_l2sq(w_norm, p_norm, m);
    }
    
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_candle(const double *O, const double *H, const double *L, const double *C,
                            const double *pO, const double *pH, const double *pL, const double *pC,
                            size_t n, size_t m, double *out, mem_pool_t *arena) {
    if (n < m || m == 0) return 0;
    double *p_feat = (double *)MEM_ALLOC_ARRAY(arena, double, m * 3);
    double *w_feat = (double *)MEM_ALLOC_ARRAY(arena, double, m * 3);
    if (!p_feat || !w_feat) return 0;

    get_candle_features(pO, pH, pL, pC, m, p_feat);
    
    for (size_t i = 0; i <= n - m; ++i) {
        get_candle_features(&O[i], &H[i], &L[i], &C[i], m, w_feat);
        out[i] = inline_simd_l2sq(w_feat, p_feat, m * 3);
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_dtw(const double *data, const double *pattern, size_t n, size_t m, double *out, mem_pool_t *arena) {
    if (n < m || m == 0) return 0;
    double *cost_mat = (double *)MEM_ALLOC_ARRAY(arena, double, (m + 1) * (m + 1));
    if (!cost_mat) return 0;
    
    for (size_t i = 0; i <= n - m; ++i) {
        out[i] = dtw_dist(&data[i], pattern, m, cost_mat);
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_correl(const double *data, const double *pattern, size_t n, size_t m, double *out, mem_pool_t *arena) {
    if (n < m || m == 0) return 0;
    double *p_norm = (double *)MEM_ALLOC_ARRAY(arena, double, m);
    double *w_norm = (double *)MEM_ALLOC_ARRAY(arena, double, m);
    if (!p_norm || !w_norm) return 0;
    
    zn_normalize(pattern, m, p_norm);
    
    for (size_t i = 0; i <= n - m; ++i) {
        zn_normalize(&data[i], m, w_norm);
        double dist_sq = inline_simd_l2sq(w_norm, p_norm, m);
        out[i] = 1.0 - (dist_sq / (2.0 * m));
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

size_t exprtk_ts_match_returns(const double *data, const double *pattern, size_t n, size_t m, double *out, mem_pool_t *arena) {
    if (n < m || m < 2) return 0;
    double *p_ret = (double *)MEM_ALLOC_ARRAY(arena, double, m - 1);
    double *w_ret = (double *)MEM_ALLOC_ARRAY(arena, double, m - 1);
    if (!p_ret || !w_ret) return 0;
    
    for (size_t j = 0; j < m - 1; ++j) {
        double r = (pattern[j+1] > 1e-9 && pattern[j] > 1e-9) ? log(pattern[j+1] / pattern[j]) : 0;
        p_ret[j] = r;
    }
    
    for (size_t i = 0; i <= n - m; ++i) {
        for (size_t j = 0; j < m - 1; ++j) {
            double r = (data[i+j+1] > 1e-9 && data[i+j] > 1e-9) ? log(data[i+j+1] / data[i+j]) : 0;
            w_ret[j] = r;
        }
        out[i] = inline_simd_l2sq(w_ret, p_ret, m - 1);
    }
    for (size_t i = n - m + 1; i < n; ++i) out[i] = 0;
    return n - m + 1;
}

static double ts_normal_cdf(double x) {
    return 0.5 * erfc(-x / sqrt(2.0));
}

/* Engle-Granger cointegration test.
   out[0] = ADF test statistic on residuals
   out[1] = approximate lower-tail p-value
   out[2] = hedge ratio (beta) */
size_t exprtk_ts_coint(const double *x, const double *y, size_t n, double *out, mem_pool_t *arena) {
    if (n < 10) return 0;

    /* Step 1: OLS regression y = beta * x + alpha + epsilon */
    double sum_x = simd_sum(x, n);
    double sum_y = simd_sum(y, n);
    double sum_xx = simd_dot(x, x, n);
    double sum_xy = simd_dot(x, y, n);

    double mean_x = sum_x / (double)n;
    double mean_y = sum_y / (double)n;
    double sxx = sum_xx - (double)n * mean_x * mean_x;
    if (fabs(sxx) < 1e-15) return 0;

    double beta = (sum_xy - (double)n * mean_x * mean_y) / sxx;
    double alpha = mean_y - beta * mean_x;

    /* Step 2: Compute residuals */
    double *resid = MEM_ALLOC_ARRAY(arena, double, n);
    if (!resid) return 0;
    simd_fill(resid, alpha, n);
    for (size_t i = 0; i < n; i++) resid[i] = y[i] - (alpha + beta * x[i]);

    /* Step 3: ADF test on residuals */
    double adf_out[2] = {0, 0};
    exprtk_ts_adf(resid, n, 1, adf_out, arena);

    out[0] = adf_out[0];  /* test statistic */
    out[1] = ts_normal_cdf(adf_out[0]);  /* approximate one-sided significance */
    out[2] = beta;         /* hedge ratio */
    return 3;
}

/* Spread: spread[i] = y[i] - beta * x[i], where beta = OLS slope */
size_t exprtk_ts_spread(const double *x, const double *y, size_t n, double *out, mem_pool_t *arena) {
    (void)arena;
    if (n < 2) return 0;

    double sum_x = simd_sum(x, n);
    double sum_y = simd_sum(y, n);
    double sum_xx = simd_dot(x, x, n);
    double sum_xy = simd_dot(x, y, n);

    double mean_x = sum_x / (double)n;
    double mean_y = sum_y / (double)n;
    double sxx = sum_xx - (double)n * mean_x * mean_x;
    if (fabs(sxx) < 1e-15) return 0;

    double beta = (sum_xy - (double)n * mean_x * mean_y) / sxx;

    for (size_t i = 0; i < n; i++)
        out[i] = y[i] - beta * x[i];
    return n;
}
