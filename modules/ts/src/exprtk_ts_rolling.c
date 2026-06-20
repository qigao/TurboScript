/**
 * @file exprtk_ts_rolling.c
 * @brief Rolling, expanding, EWM window functions and return calculations.
 */
#include "ts.h"
#include "ts_internal.h"
#include "simd_helpers.h"
#include <math.h>
#include <string.h>

/* ========================================================================= */
/* Rolling Window Functions                                                  */
/* ========================================================================= */

size_t exprtk_ts_rolling_mean(const double *in, size_t n, size_t period, double *out) {
    if (period == 0 || n < period) return 0;
    double sum = simd_sum(in, period);
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    out[period - 1] = sum / (double)period;
    for (size_t i = period; i < n; i++) {
        sum += in[i] - in[i - period];
        out[i] = sum / (double)period;
    }
    return n;
}

size_t exprtk_ts_rolling_std(const double *in, size_t n, size_t period, double *out) {
    if (period < 2 || n < period) return 0;
    double sum = simd_sum(in, period);
    double sum2 = simd_dot(in, in, period);
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    double mean = sum / (double)period;
    out[period - 1] = sqrt((sum2 / (double)period - mean * mean) * (double)period / (double)(period - 1));
    for (size_t i = period; i < n; i++) {
        sum += in[i] - in[i - period];
        sum2 += in[i] * in[i] - in[i - period] * in[i - period];
        mean = sum / (double)period;
        double var = sum2 / (double)period - mean * mean;
        out[i] = sqrt(fabs(var) * (double)period / (double)(period - 1));
    }
    return n;
}

size_t exprtk_ts_rolling_skew(const double *in, size_t n, size_t period, double *out) {
    if (period < 3 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        const double *sub = &in[i - period + 1];
        out[i] = exprtk_skewness(sub, period);
    }
    return n;
}

size_t exprtk_ts_rolling_kurt(const double *in, size_t n, size_t period, double *out) {
    if (period < 4 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        const double *sub = &in[i - period + 1];
        out[i] = exprtk_kurtosis(sub, period);
    }
    return n;
}

size_t exprtk_ts_rolling_corr(const double *a, const double *b, size_t n, size_t period, double *out) {
    if (period < 2 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        size_t offset = i - period + 1;
        const double *sub_a = &a[offset];
        const double *sub_b = &b[offset];
        
        double ma, mb, va, vb;
        simd_mean_variance(sub_a, period, &ma, &va);
        simd_mean_variance(sub_b, period, &mb, &vb);
        
        if (va < 1e-15 || vb < 1e-15) {
            out[i] = 0;
            continue;
        }
        
        double covariance = 0;
        for (size_t j = 0; j < period; j++) {
            covariance += (sub_a[j] - ma) * (sub_b[j] - mb);
        }
        covariance /= (double)(period - 1);
        out[i] = covariance / sqrt(va * vb);
    }
    return n;
}

size_t exprtk_ts_rolling_beta(const double *y, const double *x, size_t n, size_t period, double *out) {
    if (period < 2 || n < period) return 0;
    for (size_t i = 0; i < period - 1; i++) out[i] = 0;
    for (size_t i = period - 1; i < n; i++) {
        size_t offset = i - period + 1;
        ols_result_t res = ols_fit(&y[offset], &x[offset], period);
        out[i] = res.slope;
    }
    return n;
}

/* ========================================================================= */
/* Expanding Window Functions                                                */
/* ========================================================================= */

size_t exprtk_ts_expanding_mean(const double *in, size_t n, double *out) {
    if (n == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += in[i];
        out[i] = sum / (double)(i + 1);
    }
    return n;
}

size_t exprtk_ts_expanding_std(const double *in, size_t n, double *out) {
    if (n < 2) { if (n == 1) out[0] = 0; return n; }
    double sum = 0, sum2 = 0;
    out[0] = 0;
    for (size_t i = 0; i < n; i++) {
        sum += in[i];
        sum2 += in[i] * in[i];
        if (i == 0) { out[i] = 0; continue; }
        double mean = sum / (double)(i + 1);
        double var = sum2 / (double)(i + 1) - mean * mean;
        out[i] = sqrt(fabs(var) * (double)(i + 1) / (double)i);
    }
    return n;
}

/* ========================================================================= */
/* Exponentially Weighted Moving Functions                                   */
/* ========================================================================= */

size_t exprtk_ts_ewm_mean(const double *in, size_t n, size_t span, double *out) {
    if (n == 0 || span == 0) return 0;
    double alpha = 2.0 / ((double)span + 1.0);
    out[0] = in[0];
    for (size_t i = 1; i < n; i++)
        out[i] = alpha * in[i] + (1.0 - alpha) * out[i - 1];
    return n;
}

size_t exprtk_ts_ewm_std(const double *in, size_t n, size_t span, double *out) {
    if (n < 2 || span == 0) { if (n >= 1) out[0] = 0; return n; }
    double alpha = 2.0 / ((double)span + 1.0);
    double ewm = in[0], ewm2 = in[0] * in[0];
    out[0] = 0;
    for (size_t i = 1; i < n; i++) {
        ewm = alpha * in[i] + (1.0 - alpha) * ewm;
        ewm2 = alpha * in[i] * in[i] + (1.0 - alpha) * ewm2;
        double var = ewm2 - ewm * ewm;
        out[i] = sqrt(fabs(var));
    }
    return n;
}

/* ========================================================================= */
/* Return Calculations                                                       */
/* ========================================================================= */

size_t exprtk_ts_pct_change(const double *in, size_t n, size_t period, double *out) {
    if (n == 0 || period == 0) return 0;
    for (size_t i = 0; i < period && i < n; i++) out[i] = 0;
    for (size_t i = period; i < n; i++)
        out[i] = (fabs(in[i - period]) > 1e-15) ? in[i] / in[i - period] - 1.0 : 0;
    return n;
}

size_t exprtk_ts_log_return(const double *in, size_t n, double *out) {
    if (n < 2) { if (n == 1) out[0] = 0; return n; }
    out[0] = 0;
    for (size_t i = 1; i < n; i++)
        out[i] = (in[i - 1] > 1e-15 && in[i] > 1e-15) ? log(in[i] / in[i - 1]) : 0;
    return n;
}

size_t exprtk_ts_cum_return(const double *in, size_t n, double *out) {
    if (n == 0) return 0;
    out[0] = 1.0 + in[0];
    for (size_t i = 1; i < n; i++)
        out[i] = out[i - 1] * (1.0 + in[i]);
    return n;
}
