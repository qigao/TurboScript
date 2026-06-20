/**
 * @file exprtk_factor.c
 * @brief Factor processing functions for quantitative research.
 *
 * Provides core factor operations:
 * - Ranking and normalization
 * - Distribution statistics
 * - Outlier handling
 * - Cross-sectional operations
 */

#include "fin.h"
#include "simd_helpers.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Ranking
 * ========================================================================= */

/**
 * @brief Rank values in ascending order.
 *
 * Assigns rank 0 to smallest value, rank n-1 to largest.
 * Ties are handled by average rank.
 *
 * Example: [3.0, 1.0, 4.0, 1.5, 2.0] → [2, 0, 4, 1, 3]
 */
size_t exprtk_vec_rank(const double *in, size_t n, double *out, mem_pool_t *arena) {
    if (!in || !out || n == 0) return 0;

    /* Create index-value pairs */
    rank_item_t *items = MEM_ALLOC_ARRAY(arena, rank_item_t, n);
    if (!items) return 0;

    for (size_t i = 0; i < n; i++) {
        items[i].idx = i;
        items[i].val = in[i];
    }

    /* Sort by value */
    qsort(items, n, sizeof(rank_item_t), compare_rank_items);

    /* Assign ranks, handling ties with average rank */
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        /* Find end of tie group */
        while (j < n && fabs(items[j].val - items[i].val) < 1e-10) {
            j++;
        }

        /* Average rank for tie group */
        double avg_rank = (i + j - 1) / 2.0;
        for (size_t k = i; k < j; k++) {
            out[items[k].idx] = avg_rank;
        }

        i = j;
    }

    return n;
}

/* =========================================================================
 * Z-Score Normalization
 * ========================================================================= */

/**
 * @brief Standardize values to zero mean and unit variance.
 *
 * Formula: z = (x - mean) / std
 *
 * Example: [1, 2, 3, 4, 5] → [-1.414, -0.707, 0, 0.707, 1.414]
 */
size_t exprtk_vec_zscore(const double *in, size_t n, double *out, mem_pool_t *arena) {
    (void)arena;
    if (!in || !out || n == 0) return 0;
    if (n == 1) {
        out[0] = 0.0;
        return 1;
    }

    /* Calculate mean */
    double sum = simd_sum(in, n);
    double mean = sum / n;

    /* Calculate population variance (divide by n, not n-1) */
    double sum_sq = simd_norm_sq(in, n);
    double variance = sum_sq / n - mean * mean;

    double std = sqrt(variance);
    if (std < 1e-10) {
        /* Zero variance - all values are the same */
        memset(out, 0, n * sizeof(double));
        return n;
    }

    /* Standardize: (x - mean) / std */
    for (size_t i = 0; i < n; i++) {
        out[i] = (in[i] - mean) / std;
    }

    return n;
}

/* =========================================================================
 * Winsorization (Outlier Handling)
 * ========================================================================= */

/**
 * @brief Limit extreme values to specified percentiles.
 *
 * Caps values at lower and upper percentiles to reduce outlier impact.
 *
 * @param limit_pct Percentile to cap at (e.g., 0.05 for 5th/95th percentiles)
 *
 * Example: [1, 2, 3, 100], limit=0.25 → [1.75, 2, 3, 3.25]
 */
size_t exprtk_vec_winsorize(const double *in, size_t n, double limit_pct,
                             double *out, mem_pool_t *arena) {
    if (!in || !out || n == 0) return 0;
    if (limit_pct <= 0.0 || limit_pct >= 0.5) {
        /* Invalid percentile, just copy */
        memcpy(out, in, n * sizeof(double));
        return n;
    }

    /* Sort to find percentiles */
    double *sorted = MEM_ALLOC_ARRAY(arena, double, n);
    if (!sorted) return 0;
    memcpy(sorted, in, n * sizeof(double));
    qsort(sorted, n, sizeof(double), compare_doubles);

    /* Find lower and upper bounds */
    size_t lower_idx = (size_t)(limit_pct * (n - 1));
    size_t upper_idx = (size_t)((1.0 - limit_pct) * (n - 1));
    double lower_bound = sorted[lower_idx];
    double upper_bound = sorted[upper_idx];

    /* Winsorize */
    for (size_t i = 0; i < n; i++) {
        if (in[i] < lower_bound) {
            out[i] = lower_bound;
        } else if (in[i] > upper_bound) {
            out[i] = upper_bound;
        } else {
            out[i] = in[i];
        }
    }

    return n;
}

/* =========================================================================
 * Standardization (Min-Max Scaling)
 * ========================================================================= */

/**
 * @brief Scale values to [0, 1] range.
 *
 * Formula: x_scaled = (x - min) / (max - min)
 *
 * Example: [1, 2, 3, 4, 5] → [0, 0.25, 0.5, 0.75, 1.0]
 */
size_t exprtk_vec_standardize(const double *in, size_t n, double *out, mem_pool_t *arena) {
    (void)arena;
    if (!in || !out || n == 0) return 0;
    if (n == 1) {
        out[0] = 0.0;
        return 1;
    }

    /* Find min and max using SIMD */
    double min_val = simd_min(in, n);
    double max_val = simd_max(in, n);

    double range = max_val - min_val;
    if (range < 1e-10) {
        /* All values are the same */
        memset(out, 0, n * sizeof(double));
        return n;
    }

    /* Scale to [0, 1] */
    for (size_t i = 0; i < n; i++) {
        out[i] = (in[i] - min_val) / range;
    }

    return n;
}

/* =========================================================================
 * Distribution Statistics
 * ========================================================================= */

/**
 * @brief Calculate skewness (asymmetry of distribution).
 *
 * Positive skew: right tail is longer
 * Negative skew: left tail is longer
 * Zero skew: symmetric distribution
 *
 * Formula: skew = E[(X - μ)³] / σ³
 */
double exprtk_vec_skewness(const double *in, size_t n) {
    if (!in || n < 3) return 0.0;

    /* Calculate mean and std */
    double mean, variance;
    simd_mean_variance(in, n, &mean, &variance);
    double std = sqrt(variance);

    if (std < 1e-10) return 0.0;

    /* Calculate third moment */
    double m3 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double z = (in[i] - mean) / std;
        m3 += z * z * z;
    }

    return m3 / n;
}

/**
 * @brief Calculate kurtosis (tailedness of distribution).
 *
 * Excess kurtosis = kurtosis - 3
 * Positive: heavy tails (leptokurtic)
 * Negative: light tails (platykurtic)
 * Zero: normal distribution (mesokurtic)
 *
 * Formula: kurt = E[(X - μ)⁴] / σ⁴ - 3
 */
double exprtk_vec_kurtosis(const double *in, size_t n) {
    if (!in || n < 4) return 0.0;

    /* Calculate mean and std */
    double mean, variance;
    simd_mean_variance(in, n, &mean, &variance);
    double std = sqrt(variance);

    if (std < 1e-10) return 0.0;

    /* Calculate fourth moment */
    double m4 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double z = (in[i] - mean) / std;
        double z2 = z * z;
        m4 += z2 * z2;
    }

    /* Return excess kurtosis */
    return (m4 / n) - 3.0;
}

/**
 * @brief Calculate Shannon entropy (information content).
 *
 * Higher entropy = more uniform distribution
 * Lower entropy = more concentrated distribution
 *
 * Formula: H = -Σ p(x) * log(p(x))
 *
 * @param bins Number of bins for histogram
 */
double exprtk_vec_entropy(const double *in, size_t n, size_t bins) {
    if (!in || n == 0 || bins == 0) return 0.0;
    if (bins > n) bins = n;

    /* Find min and max */
    double min_val = simd_min(in, n);
    double max_val = simd_max(in, n);
    double range = max_val - min_val;

    if (range < 1e-10) return 0.0;

    /* Build histogram */
    size_t *hist = (size_t *)calloc(bins, sizeof(size_t));
    if (!hist) return 0.0;

    for (size_t i = 0; i < n; i++) {
        size_t bin = (size_t)((in[i] - min_val) / range * bins);
        if (bin >= bins) bin = bins - 1;
        hist[bin]++;
    }

    /* Calculate entropy */
    double entropy = 0.0;
    for (size_t i = 0; i < bins; i++) {
        if (hist[i] > 0) {
            double p = (double)hist[i] / n;
            entropy -= p * log(p);
        }
    }

    free(hist);
    return entropy;
}

/* =========================================================================
 * Realized Volatility Measures
 * ========================================================================= */

/**
 * @brief Calculate realized variance from returns.
 *
 * RV = Σ r²
 *
 * Used for volatility forecasting and risk management.
 */
double exprtk_vec_rvar(const double *in, size_t n) {
    if (!in || n == 0) return 0.0;
    return simd_norm_sq(in, n) / n;
}

/**
 * @brief Calculate realized skewness from returns.
 *
 * Measures asymmetry in realized distribution.
 */
double exprtk_vec_rskew(const double *in, size_t n) {
    if (!in || n < 3) return 0.0;

    double rv = exprtk_vec_rvar(in, n);
    if (rv < 1e-10) return 0.0;

    double m3 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double r = in[i];
        m3 += r * r * r;
    }

    return (m3 / n) / pow(rv, 1.5);
}

/**
 * @brief Calculate realized kurtosis from returns.
 *
 * Measures tail risk in realized distribution.
 */
double exprtk_vec_rkurt(const double *in, size_t n) {
    if (!in || n < 4) return 0.0;

    double rv = exprtk_vec_rvar(in, n);
    if (rv < 1e-10) return 0.0;

    double m4 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double r2 = in[i] * in[i];
        m4 += r2 * r2;
    }

    return (m4 / n) / (rv * rv) - 3.0;
}

/* =========================================================================
 * Market Microstructure Factors
 * ========================================================================= */

/**
 * @brief Calculate Amihud illiquidity measure.
 *
 * ILLIQ = |return| / dollar_volume
 *
 * Higher values indicate lower liquidity.
 *
 * @param ret Returns array
 * @param amount Dollar volume array (price * volume)
 * @param n Number of periods
 */
double exprtk_vec_illiq(const double *ret, const double *amount, size_t n) {
    if (!ret || !amount || n == 0) return 0.0;

    double illiq = 0.0;
    size_t count = 0;

    for (size_t i = 0; i < n; i++) {
        if (amount[i] > 1e-10) {
            illiq += fabs(ret[i]) / amount[i];
            count++;
        }
    }

    return count > 0 ? illiq / count : 0.0;
}

/**
 * @brief Calculate volatility ratio across segments.
 *
 * Compares volatility of a segment to overall volatility.
 * Used for detecting regime changes.
 *
 * @param vol Volatility array
 * @param n Total length
 * @param segment_len Length of each segment
 * @param segment_idx Which segment to analyze (0-based)
 */
double exprtk_vec_vol_ratio(const double *vol, size_t n, size_t segment_len, size_t segment_idx) {
    if (!vol || n == 0 || segment_len == 0) return 1.0;

    size_t start = segment_idx * segment_len;
    if (start >= n) return 1.0;

    size_t end = start + segment_len;
    if (end > n) end = n;

    /* Calculate segment average */
    double seg_sum = 0.0;
    for (size_t i = start; i < end; i++) {
        seg_sum += vol[i];
    }
    double seg_avg = seg_sum / (end - start);

    /* Calculate overall average */
    double total_avg = simd_sum(vol, n) / n;

    return total_avg > 1e-10 ? seg_avg / total_avg : 1.0;
}

/**
 * @brief Calculate trend strength using linear regression R².
 *
 * Higher values indicate stronger trend.
 * Range: [0, 1]
 */
double exprtk_vec_trend_strength(const double *price, size_t n) {
    if (!price || n < 3) return 0.0;

    /* Calculate linear regression */
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        double x = (double)i;
        double y = price[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double mean_x = sum_x / n;
    double mean_y = sum_y / n;

    double cov = sum_xy / n - mean_x * mean_y;
    double var_x = sum_x2 / n - mean_x * mean_x;

    if (var_x < 1e-10) return 0.0;

    double slope = cov / var_x;
    double intercept = mean_y - slope * mean_x;

    /* Calculate R² */
    double ss_res = 0.0, ss_tot = 0.0;
    for (size_t i = 0; i < n; i++) {
        double fitted = slope * i + intercept;
        double residual = price[i] - fitted;
        ss_res += residual * residual;
        ss_tot += (price[i] - mean_y) * (price[i] - mean_y);
    }

    return ss_tot > 1e-10 ? 1.0 - (ss_res / ss_tot) : 0.0;
}

/**
 * @brief Calculate price efficiency ratio.
 *
 * Efficiency = net_movement / total_movement
 *
 * Range: [0, 1]
 * 1 = perfectly efficient (straight line)
 * 0 = completely random walk
 */
double exprtk_vec_efficiency(const double *price, size_t n) {
    if (!price || n < 2) return 0.0;

    /* Net movement */
    double net = fabs(price[n-1] - price[0]);

    /* Total movement */
    double total = 0.0;
    for (size_t i = 1; i < n; i++) {
        total += fabs(price[i] - price[i-1]);
    }

    return total > 1e-10 ? net / total : 0.0;
}

/* =========================================================================
 * Cross-Sectional Factors
 * ========================================================================= */

/**
 * @brief Calculate Cross-Sectional Absolute Deviation (CSAD).
 *
 * CSAD measures dispersion of returns across assets.
 * Used for detecting herding behavior.
 *
 * @param rets Returns matrix (na assets × np periods)
 * @param na Number of assets
 * @param np Number of periods
 * @param out Output array (length np)
 */
size_t exprtk_vec_csad(const double *rets, size_t na, size_t np, double *out, mem_pool_t *arena) {
    if (!rets || !out || na == 0 || np == 0) return 0;

    double *cross_mean = MEM_ALLOC_ARRAY(arena, double, np);
    if (!cross_mean) return 0;

    /* Calculate cross-sectional mean for each period */
    for (size_t p = 0; p < np; p++) {
        double sum = 0.0;
        for (size_t a = 0; a < na; a++) {
            sum += rets[a * np + p];
        }
        cross_mean[p] = sum / na;
    }

    /* Calculate CSAD for each period */
    for (size_t p = 0; p < np; p++) {
        double csad = 0.0;
        for (size_t a = 0; a < na; a++) {
            csad += fabs(rets[a * np + p] - cross_mean[p]);
        }
        out[p] = csad / na;
    }

    return np;
}

/**
 * @brief Calculate rolling quantile rank.
 *
 * For each period, ranks the value within a rolling window.
 *
 * @param in Input array
 * @param n Length
 * @param period Rolling window size
 * @param out Output array (length n)
 */
size_t exprtk_vec_quantile(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
    if (!in || !out || n == 0 || period == 0) return 0;
    if (period > n) period = n;

    double *window = MEM_ALLOC_ARRAY(arena, double, period);
    if (!window) return 0;

    for (size_t i = 0; i < n; i++) {
        size_t start = (i >= period - 1) ? (i - period + 1) : 0;
        size_t len = i - start + 1;

        /* Copy window */
        memcpy(window, in + start, len * sizeof(double));
        qsort(window, len, sizeof(double), compare_doubles);

        /* Find rank of current value */
        double val = in[i];
        size_t rank = 0;
        for (size_t j = 0; j < len; j++) {
            if (window[j] < val) rank++;
        }

        out[i] = (double)rank / len;
    }

    return n;
}

/**
 * @brief Calculate Fractal Volatility Dimension (FVD).
 *
 * Measures complexity of volatility pattern.
 * Higher values indicate more complex/fractal behavior.
 *
 * @param vol Volatility array
 * @param n Length
 * @param window_size Window for calculation
 */
double exprtk_vec_fvd(const double *vol, size_t n, size_t window_size) {
    if (!vol || n < window_size || window_size < 2) return 0.0;

    /* Calculate path length at different scales */
    double length_1 = 0.0;
    for (size_t i = 1; i < window_size; i++) {
        length_1 += fabs(vol[i] - vol[i-1]);
    }

    double length_2 = 0.0;
    for (size_t i = 2; i < window_size; i += 2) {
        length_2 += fabs(vol[i] - vol[i-2]);
    }

    if (length_2 < 1e-10) return 0.0;

    /* Fractal dimension */
    double ratio = length_1 / length_2;
    return ratio > 0 ? log(ratio) / log(2.0) : 0.0;
}

/**
 * @brief Calculate Return-Signed Jump (RSJ).
 *
 * Measures asymmetry in jump behavior.
 * Positive: more positive jumps
 * Negative: more negative jumps
 */
double exprtk_vec_rsj(const double *ret, size_t n) {
    if (!ret || n < 2) return 0.0;

    double pos_jump = 0.0, neg_jump = 0.0;
    size_t pos_count = 0, neg_count = 0;

    /* Identify jumps (returns > 2 std) */
    double mean, variance;
    simd_mean_variance(ret, n, &mean, &variance);
    double std = sqrt(variance);
    double threshold = 2.0 * std;

    for (size_t i = 0; i < n; i++) {
        if (ret[i] > threshold) {
            pos_jump += ret[i];
            pos_count++;
        } else if (ret[i] < -threshold) {
            neg_jump += fabs(ret[i]);
            neg_count++;
        }
    }

    double total = pos_jump + neg_jump;
    return total > 1e-10 ? (pos_jump - neg_jump) / total : 0.0;
}

/**
 * @brief Calculate AM-PM return spread.
 *
 * Measures difference between morning and afternoon returns.
 * Used for intraday pattern analysis.
 *
 * @param ret_am Morning returns
 * @param ret_pm Afternoon returns
 * @param n Number of days
 */
double exprtk_vec_apm(const double *ret_am, const double *ret_pm, size_t n) {
    if (!ret_am || !ret_pm || n == 0) return 0.0;

    double am_sum = simd_sum(ret_am, n);
    double pm_sum = simd_sum(ret_pm, n);

    return (am_sum - pm_sum) / n;
}

/**
 * @brief Calculate Cumulative Garch Overreaction (CGO).
 *
 * Measures overreaction in volatility clustering.
 * Used for volatility timing strategies.
 *
 * @param p Price array
 * @param v Volume array
 * @param n Length
 * @param period Rolling window
 * @param out Output array
 */
size_t exprtk_vec_cgo(const double *p, const double *v, size_t n, size_t period, double *out, mem_pool_t *arena) {
    if (!p || !v || !out || n == 0 || period == 0) return 0;
    if (period > n) period = n;

    double *returns = MEM_ALLOC_ARRAY(arena, double, n);
    if (!returns) return 0;

    /* Calculate returns */
    for (size_t i = 1; i < n; i++) {
        returns[i] = (p[i] - p[i-1]) / p[i-1];
    }
    returns[0] = 0.0;

    /* Calculate CGO for each period */
    for (size_t i = 0; i < n; i++) {
        if (i < period - 1) {
            out[i] = 0.0;
            continue;
        }

        size_t start = i - period + 1;

        /* Calculate volatility-weighted return */
        double vol_sum = 0.0, weighted_ret = 0.0;
        for (size_t j = start; j <= i; j++) {
            double vol = v[j];
            vol_sum += vol;
            weighted_ret += returns[j] * vol;
        }

        out[i] = vol_sum > 1e-10 ? weighted_ret / vol_sum : 0.0;
    }

    return n;
}

/**
 * @brief Calculate Salience factor.
 *
 * Measures attention-grabbing price movements.
 * Based on behavioral finance theory.
 *
 * Formula: salience = (ret - mkt_ret) / |ret - mkt_ret|^delta
 *
 * @param ret Asset returns
 * @param mkt_ret Market returns
 * @param n Number of periods
 * @param delta Decay parameter (typically 0.5-1.0)
 */
double exprtk_vec_salience(const double *ret, const double *mkt_ret, size_t n, double delta) {
    if (!ret || !mkt_ret || n == 0) return 0.0;
    if (delta <= 0.0) delta = 0.7; // Default

    double salience = 0.0;
    size_t count = 0;

    for (size_t i = 0; i < n; i++) {
        double diff = ret[i] - mkt_ret[i];
        double abs_diff = fabs(diff);

        if (abs_diff > 1e-10) {
            /* Salience increases with extreme deviations */
            double weight = pow(abs_diff, -delta);
            salience += diff * weight;
            count++;
        }
    }

    return count > 0 ? salience / count : 0.0;
}

/**
 * @brief Calculate Short-Term Reversal (STR) factor.
 *
 * Measures mean reversion tendency.
 * Negative correlation between current and past returns.
 *
 * Formula: STR = -Corr(ret_t, ret_{t-lag})
 *
 * @param ret Asset returns
 * @param mkt_ret Market returns (for market-adjusted STR)
 * @param n Number of periods
 * @param delta Lag parameter (typically 1-5 days)
 */
double exprtk_vec_str(const double *ret, const double *mkt_ret, size_t n, double delta) {
    if (!ret || !mkt_ret || n < 2) return 0.0;

    size_t lag = (size_t)delta;
    if (lag == 0) lag = 1;
    if (lag >= n) lag = n - 1;

    /* Calculate excess returns */
    double *excess = (double *)malloc(n * sizeof(double));
    if (!excess) return 0.0;

    for (size_t i = 0; i < n; i++) {
        excess[i] = ret[i] - mkt_ret[i];
    }

    /* Calculate autocorrelation at lag */
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0;
    double sum_x2 = 0.0, sum_y2 = 0.0;
    size_t count = 0;

    for (size_t i = lag; i < n; i++) {
        double x = excess[i - lag];
        double y = excess[i];

        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
        sum_y2 += y * y;
        count++;
    }

    free(excess);

    if (count == 0) return 0.0;

    double mean_x = sum_x / count;
    double mean_y = sum_y / count;

    double cov = sum_xy / count - mean_x * mean_y;
    double std_x = sqrt(sum_x2 / count - mean_x * mean_x);
    double std_y = sqrt(sum_y2 / count - mean_y * mean_y);

    if (std_x < 1e-10 || std_y < 1e-10) return 0.0;

    /* Return negative correlation (reversal) */
    return -cov / (std_x * std_y);
}


