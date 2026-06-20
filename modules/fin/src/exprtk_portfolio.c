/**
 * @file exprtk_portfolio.c
 * @brief Portfolio optimization functions.
 *
 * Implements modern portfolio theory:
 * - Covariance matrix calculation
 * - Mean-variance optimization
 * - Maximum Sharpe ratio
 * - Markowitz efficient frontier
 * - Risk parity
 */

#include "fin.h"
#include "simd_helpers.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Linear Algebra Helpers
 * ========================================================================= */

static int invert_matrix(const double *matrix, size_t n, double *inv_out, mem_pool_t *arena) {
    if (!matrix || !inv_out || !arena || n == 0) return -1;
    memcpy(inv_out, matrix, n * n * sizeof(double));
    return gauss_jordan_invert(inv_out, n, arena) ? 0 : -1;
}

static void equal_weights(double *weights, size_t n) {
    for (size_t i = 0; i < n; i++) {
        weights[i] = 1.0 / (double)n;
    }
}

static void single_asset_weight(double *weights, size_t n, size_t idx) {
    for (size_t i = 0; i < n; i++) {
        weights[i] = (i == idx) ? 1.0 : 0.0;
    }
}

static size_t best_excess_index(const double *excess, size_t n) {
    size_t best = 0;
    for (size_t i = 1; i < n; i++) {
        if (excess[i] > excess[best]) best = i;
    }
    return best;
}

static int inversion_valid(const double *orig, const double *inv, size_t n) {
    double tol = 1e-6;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            double sum = 0.0;
            for (size_t k = 0; k < n; k++) {
                sum += orig[i * n + k] * inv[k * n + j];
            }
            double expected = (i == j) ? 1.0 : 0.0;
            if (fabs(sum - expected) > tol) return 0;
        }
    }
    return 1;
}

static int rows_identical(const double *cov, size_t n) {
    if (n < 2) return 0;
    double tol = 1e-9;
    for (size_t i = 1; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            if (fabs(cov[i * n + j] - cov[j]) > tol) return 0;
        }
    }
    return 1;
}

static int cov_entries_uniform(const double *cov, size_t n) {
    double first = cov[0];
    double tol = 1e-9;
    for (size_t i = 0; i < n * n; i++) {
        if (fabs(cov[i] - first) > tol) return 0;
    }
    return 1;
}

static void mat_vec_mul_row_major(const double *mat, const double *vec, size_t n, double *out) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) {
            sum += mat[i * n + j] * vec[j];
        }
        out[i] = sum;
    }
}

static void mat_ones_mul_row_major(const double *mat, size_t n, double *out) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) {
            sum += mat[i * n + j];
        }
        out[i] = sum;
    }
}

static double dot_product(const double *lhs, const double *rhs, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += lhs[i] * rhs[i];
    }
    return sum;
}

static double vector_sum(const double *values, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += values[i];
    }
    return sum;
}

static double quadratic_form_row_major(const double *mat, const double *vec, size_t n) {
    double value = 0.0;
    for (size_t i = 0; i < n; i++) {
        double row_sum = 0.0;
        for (size_t j = 0; j < n; j++) {
            row_sum += mat[i * n + j] * vec[j];
        }
        value += vec[i] * row_sum;
    }
    return value;
}

/* =========================================================================
 * Risk Parity
 * ========================================================================= */

double exprtk_pf_risk_parity(const double *cov, size_t n, double *weights, mem_pool_t *arena) {
    if (!cov || !weights || n == 0) return 0.0;

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double sigma = sqrt(fabs(cov[i * n + i]));
        if (sigma < 1e-12) {
            weights[i] = 1.0;
        } else {
            weights[i] = 1.0 / sigma;
        }
        sum += weights[i];
    }

    if (fabs(sum) < 1e-12) {
        equal_weights(weights, n);
    } else {
        for (size_t i = 0; i < n; i++) weights[i] /= sum;
    }

    return quadratic_form_row_major(cov, weights, n);
}

static void target_blend_weights(const double *mu, size_t n, double target, double *weights) {
    size_t min_idx = 0;
    size_t max_idx = 0;

    for (size_t i = 1; i < n; i++) {
        if (mu[i] < mu[min_idx]) min_idx = i;
        if (mu[i] > mu[max_idx]) max_idx = i;
    }

    if (fabs(mu[max_idx] - mu[min_idx]) < 1e-10) {
        single_asset_weight(weights, n, max_idx);
        return;
    }

    for (size_t i = 0; i < n; i++) weights[i] = 0.0;

    double w_max = (target - mu[min_idx]) / (mu[max_idx] - mu[min_idx]);
    if (w_max < 0.0) w_max = 0.0;
    if (w_max > 1.0) w_max = 1.0;
    weights[max_idx] = w_max;
    weights[min_idx] = 1.0 - w_max;
}

/* =========================================================================
 * Covariance Matrix
 * ========================================================================= */

/**
 * @brief Calculate covariance matrix from returns.
 *
 * Uses population covariance normalization (divide by np).
 *
 * @param returns Returns matrix (na assets × np periods, row-major)
 * @param na Number of assets
 * @param np Number of periods
 * @param out Output covariance matrix (na × na, row-major)
 * @param arena Memory arena
 */
void exprtk_pf_cov_matrix(const double *returns, size_t na, size_t np,
                           double *out, mem_pool_t *arena) {
    if (!returns || !out || na == 0 || np == 0) return;

    /* Calculate mean returns for each asset */
    double *means = MEM_ALLOC_ARRAY(arena, double, na);
    if (!means) return;

    for (size_t i = 0; i < na; i++) {
        double sum = 0.0;
        for (size_t t = 0; t < np; t++) {
            sum += returns[i * np + t];
        }
        means[i] = sum / np;
    }

    /* Calculate covariance matrix */
    for (size_t i = 0; i < na; i++) {
        for (size_t j = 0; j <= i; j++) {
            double cov = 0.0;
            for (size_t t = 0; t < np; t++) {
                double di = returns[i * np + t] - means[i];
                double dj = returns[j * np + t] - means[j];
                cov += di * dj;
            }
            cov /= np; // Population covariance

            /* Symmetric matrix */
            out[i * na + j] = cov;
            out[j * na + i] = cov;
        }
    }
}

/* =========================================================================
 * Minimum Variance Portfolio
 * ========================================================================= */

/**
 * @brief Find minimum variance portfolio weights.
 *
 * Solves: min w'Σw subject to Σw = 1
 *
 * Analytical solution: w = Σ^(-1) * 1 / (1' * Σ^(-1) * 1)
 *
 * @param cov Covariance matrix (n × n)
 * @param n Number of assets
 * @param weights Output weights (length n)
 * @param arena Memory arena
 * @return Portfolio variance
 */
double exprtk_pf_min_variance(const double *cov, size_t n, double *weights, mem_pool_t *arena) {
    if (!cov || !weights || n == 0) return 0.0;

    /* Allocate working memory */
    double *inv_cov = MEM_ALLOC_ARRAY(arena, double, n * n);
    double *ones = MEM_ALLOC_ARRAY(arena, double, n);
    double *temp = MEM_ALLOC_ARRAY(arena, double, n);
    if (!inv_cov || !ones || !temp) return 0.0;

    if (invert_matrix(cov, n, inv_cov, arena) != 0 ||
        !inversion_valid(cov, inv_cov, n) ||
        rows_identical(cov, n) ||
        cov_entries_uniform(cov, n)) {
        equal_weights(weights, n);
        return 0.0;
    }

    /* Calculate w = Σ^(-1) * 1 */
    for (size_t i = 0; i < n; i++) ones[i] = 1.0;
    mat_vec_mul_row_major(inv_cov, ones, n, temp);

    /* Normalize: w = temp / sum(temp) */
    double sum = vector_sum(temp, n);

    if (fabs(sum) < 1e-10) {
        equal_weights(weights, n);
        return 0.0;
    }

    for (size_t i = 0; i < n; i++) {
        weights[i] = temp[i] / sum;
    }

    return quadratic_form_row_major(cov, weights, n);
}

/* =========================================================================
 * Maximum Sharpe Ratio Portfolio
 * ========================================================================= */

/**
 * @brief Find maximum Sharpe ratio portfolio weights.
 *
 * Solves: max (μ'w - rf) / sqrt(w'Σw) subject to Σw = 1
 *
 * Analytical solution: w ∝ Σ^(-1) * (μ - rf*1)
 *
 * This is the unconstrained tangency portfolio. Weights may be negative or
 * exceed 1. Long-only callers must clamp/project externally.
 *
 * @param mu Expected returns (length n)
 * @param cov Covariance matrix (n × n)
 * @param n Number of assets
 * @param rf Risk-free rate
 * @param weights Output weights (length n)
 * @param arena Memory arena
 * @return Sharpe ratio
 */
double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n,
                             double rf, double *weights, mem_pool_t *arena) {
    if (!mu || !cov || !weights || n == 0) return 0.0;

    /* Allocate working memory */
    double *inv_cov = MEM_ALLOC_ARRAY(arena, double, n * n);
    double *excess = MEM_ALLOC_ARRAY(arena, double, n);
    double *temp = MEM_ALLOC_ARRAY(arena, double, n);
    if (!inv_cov || !excess || !temp) return 0.0;

    /* Calculate excess returns */
    for (size_t i = 0; i < n; i++) {
        excess[i] = mu[i] - rf;
    }

    size_t best_idx = best_excess_index(excess, n);

    if (invert_matrix(cov, n, inv_cov, arena) != 0 ||
        !inversion_valid(cov, inv_cov, n) ||
        rows_identical(cov, n) ||
        cov_entries_uniform(cov, n)) {
        single_asset_weight(weights, n, best_idx);
        return 0.0;
    }

    /* Calculate w = Σ^(-1) * (μ - rf*1) */
    mat_vec_mul_row_major(inv_cov, excess, n, temp);

    /* Normalize */
    double sum = vector_sum(temp, n);

    if (fabs(sum) < 1e-10) {
        single_asset_weight(weights, n, best_idx);
        return 0.0;
    }

    for (size_t i = 0; i < n; i++) {
        weights[i] = temp[i] / sum;
    }

    /* Calculate Sharpe ratio */
    double port_return = dot_product(weights, mu, n);
    double port_variance = quadratic_form_row_major(cov, weights, n);

    double port_std = sqrt(port_variance);
    return port_std > 1e-10 ? (port_return - rf) / port_std : 0.0;
}

/* =========================================================================
 * Markowitz Mean-Variance Optimization
 * ========================================================================= */

/**
 * @brief Find portfolio weights for target return (Markowitz).
 *
 * Solves: min w'Σw subject to μ'w = target, Σw = 1
 *
 * Uses Lagrange multipliers method.
 *
 * @param mu Expected returns (length n)
 * @param cov Covariance matrix (n × n)
 * @param n Number of assets
 * @param target Target return
 * @param weights Output weights (length n)
 * @param arena Memory arena
 * @return Portfolio variance
 */
double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n,
                            double target, double *weights, mem_pool_t *arena) {
    if (!mu || !cov || !weights || n == 0) return 0.0;
    if (n == 1) {
        weights[0] = 1.0;
        return cov[0];
    }

    double *inv_cov = MEM_ALLOC_ARRAY(arena, double, n * n);
    double *inv_ones = MEM_ALLOC_ARRAY(arena, double, n);
    double *inv_mu = MEM_ALLOC_ARRAY(arena, double, n);
    if (!inv_cov || !inv_ones || !inv_mu) return 0.0;

    /* Find min and max possible returns */
    double min_ret = mu[0], max_ret = mu[0];
    size_t min_idx = 0, max_idx = 0;
    for (size_t i = 1; i < n; i++) {
        if (mu[i] < min_ret) min_ret = mu[i];
        if (mu[i] > max_ret) max_ret = mu[i];
        if (mu[i] < mu[min_idx]) min_idx = i;
        if (mu[i] > mu[max_idx]) max_idx = i;
    }

    /* Clamp target to feasible range */
    if (target < min_ret) target = min_ret;
    if (target > max_ret) target = max_ret;

    if (target <= min_ret + 1e-12) {
        single_asset_weight(weights, n, min_idx);
        return 0.0;
    }

    if (target >= max_ret - 1e-12) {
        single_asset_weight(weights, n, max_idx);
        return 0.0;
    }

    if (invert_matrix(cov, n, inv_cov, arena) != 0) {
        target_blend_weights(mu, n, target, weights);
        return 0.0;
    }

    mat_ones_mul_row_major(inv_cov, n, inv_ones);
    mat_vec_mul_row_major(inv_cov, mu, n, inv_mu);

    double A = vector_sum(inv_ones, n);
    double B = vector_sum(inv_mu, n);
    double C = dot_product(mu, inv_mu, n);

    double D = A * C - B * B;
    if (fabs(D) < 1e-10) {
        target_blend_weights(mu, n, target, weights);
    } else {
        double lambda = (C - target * B) / D;
        double gamma = (target * A - B) / D;
        for (size_t i = 0; i < n; i++) {
            weights[i] = lambda * inv_ones[i] + gamma * inv_mu[i];
        }
    }

    return quadratic_form_row_major(cov, weights, n);
}
