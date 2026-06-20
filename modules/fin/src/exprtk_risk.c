/**
 * @file exprtk_risk.c
 * @brief Advanced risk management functions.
 *
 * Provides position sizing and risk analysis tools.
 */

#include "fin.h"
#include "simd_helpers.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Position Sizing
 * ========================================================================= */

/**
 * @brief Calculate fixed fractional position size.
 *
 * Kelly-style position sizing based on risk per trade.
 *
 * Formula: size = (equity * risk_pct) / stop_dist
 *
 * @param equity Current equity
 * @param risk_pct Risk percentage per trade (e.g., 0.02 for 2%)
 * @param stop_dist Stop loss distance as fraction (e.g., 0.05 for 5%)
 * @return Position size in dollars
 */
double exprtk_fixed_frac(double equity, double risk_pct, double stop_dist) {
    if (equity < 1e-10 || stop_dist < 1e-10) return 0.0;

    /* Risk amount */
    double risk_amount = equity * risk_pct;

    /* Position size = risk_amount / stop_dist */
    double size = risk_amount / stop_dist;

    /* Cap at equity to avoid over-leverage */
    if (size > equity) size = equity;

    return size;
}

/**
 * @brief Calculate optimal f (optimal fixed fraction).
 *
 * Finds the fraction of capital that maximizes geometric growth.
 * Based on Ralph Vince's work.
 *
 * @param trades Array of trade P&Ls
 * @param n Number of trades
 * @param out Output: [0] optimal_f, [1] max_TWR
 * @return Optimal f value
 */
double exprtk_optimal_f(const double *trades, size_t n, double *out) {
    if (!trades || n == 0) return 0.0;

    /* Find largest loss (for normalization) */
    double max_loss = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (trades[i] < max_loss) max_loss = trades[i];
    }

    if (max_loss >= 0.0) {
        /* No losses - can't calculate optimal f */
        if (out) {
            out[0] = 0.0;
            out[1] = 0.0;
        }
        return 0.0;
    }

    /* Grid search for optimal f */
    double best_f = 0.0;
    double best_twr = 0.0;

    for (double f = 0.01; f <= 1.0; f += 0.01) {
        /* Calculate Terminal Wealth Relative (TWR) */
        double twr = 1.0;
        for (size_t i = 0; i < n; i++) {
            double hpr = 1.0 + (f * trades[i] / fabs(max_loss));
            if (hpr <= 0.0) {
                twr = 0.0;
                break;
            }
            twr *= hpr;
        }

        if (twr > best_twr) {
            best_twr = twr;
            best_f = f;
        }
    }

    if (out) {
        out[0] = best_f;
        out[1] = best_twr;
    }

    return best_f;
}

/**
 * @brief Monte Carlo simulation for risk analysis.
 *
 * Simulates price paths using Geometric Brownian Motion.
 *
 * Formula: S(t+dt) = S(t) * exp((μ - σ²/2)*dt + σ*sqrt(dt)*Z)
 *
 * @param s0 Initial price
 * @param mu Drift (expected return)
 * @param sigma Volatility
 * @param dt Time step
 * @param steps Number of time steps
 * @param paths Number of simulation paths
 * @param out Output array (paths × steps)
 * @param arena Memory arena
 * @return Number of paths generated
 */
size_t exprtk_mc_simulate(double s0, double mu, double sigma, double dt,
                           size_t steps, size_t paths, double *out,
                           mem_pool_t *arena) {
    if (!out || steps == 0 || paths == 0) return 0;
    (void)arena;

    double drift = (mu - 0.5 * sigma * sigma) * dt;
    double vol = sigma * sqrt(dt);

    /* Simple Box-Muller for normal random numbers */
    for (size_t p = 0; p < paths; p++) {
        double price = s0;
        out[p * steps] = price;

        for (size_t t = 1; t < steps; t++) {
            /* Generate standard normal random variable */
            double u1 = (double)rand() / RAND_MAX;
            double u2 = (double)rand() / RAND_MAX;
            if (u1 < 1e-10) u1 = 1e-10;

            double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);

            /* Update price */
            price *= exp(drift + vol * z);
            out[p * steps + t] = price;
        }
    }

    return paths;
}
