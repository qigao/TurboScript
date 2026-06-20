/**
 * @file exprtk_backtest.c
 * @brief Simplified backtest functions.
 *
 * Provides functional-style backtest interfaces for simple strategies.
 * For complex event-driven strategies, use strategy.c framework.
 */

#include "fin.h"
#include "simd_helpers.h"
#include <math.h>
#include <string.h>

/* =========================================================================
 * Simple Backtest
 * ========================================================================= */

/**
 * @brief Simple vectorized backtest.
 *
 * Backtests a signal-based strategy with basic position sizing.
 *
 * Execution semantics are intentionally simple and explicit:
 *   - signal[i] is filled at open[i]
 *   - equity[i] is then marked using close[i]
 * This means callers must pre-lag any signal built from close[i].
 *
 * @param open Open prices
 * @param close Close prices
 * @param signal Trading signals: +1 (long), -1 (short), 0 (flat)
 * @param n Number of bars
 * @param cash0 Initial cash
 * @param commission Commission rate (e.g., 0.001 for 0.1%)
 * @param equity Output equity curve (length n)
 * @param trades Output sparse per-bar trade P&L array (length n)
 * @return Number of trades executed
 */
size_t exprtk_bt_backtest(const double *open, const double *close,
                           const double *signal, size_t n,
                           double cash0, double commission,
                           double *equity, double *trades) {
    if (!open || !close || !signal || !equity || !trades || n == 0) return 0;

    double cash = cash0;
    double position = 0.0;
    double entry_price = 0.0;
    size_t num_trades = 0;

    for (size_t i = 0; i < n; i++) {
        trades[i] = 0.0;

        /* Check for position change */
        double new_signal = signal[i];
        double current_pos = (position > 0) ? 1.0 : (position < 0) ? -1.0 : 0.0;

        if (fabs(new_signal - current_pos) > 0.5) {
            /* Close existing position */
            if (fabs(position) > 1e-10) {
                double exit_price = open[i];
                double pnl = position * (exit_price - entry_price);
                double cost = fabs(position * exit_price) * commission;
                pnl -= cost;

                cash += position * entry_price + pnl;
                trades[i] = pnl;
                num_trades++;
                position = 0.0;
            }

            /* Open new position */
            if (fabs(new_signal) > 0.5) {
                double size = cash * 0.95; // Use 95% of cash
                double notional;
                position = (new_signal > 0 ? 1.0 : -1.0) * size / open[i];
                entry_price = open[i];
                notional = fabs(position * entry_price);
                if (position > 0.0)
                    cash -= notional;
                else
                    cash += notional;
                cash -= notional * commission;
            }
        }

        /* Mark to market */
        double market_value = cash;
        if (fabs(position) > 1e-10) {
            market_value += position * close[i];
        }
        equity[i] = market_value;
    }

    return num_trades;
}

/**
 * @brief Calculate backtest statistics.
 *
 * @param equity Equity curve
 * @param trades Trade P&L array
 * @param n Number of bars
 * @param num_trades Number of trades
 * @param annual Annualization factor (252 for daily, 12 for monthly)
 * @param out Output statistics array (length 10):
 *   [0] Total return
 *   [1] Annualized return
 *   [2] Sharpe ratio
 *   [3] Max drawdown
 *   [4] Win rate
 *   [5] Profit factor
 *   [6] Average win
 *   [7] Average loss
 *   [8] Max win
 *   [9] Max loss
 * @return Number of statistics calculated
 */
size_t exprtk_bt_stats(const double *equity, const double *trades, size_t n,
                        size_t num_trades, double annual, double *out) {
    if (!equity || !out || n == 0) return 0;

    /* Total return */
    double total_ret = (equity[n-1] - equity[0]) / equity[0];
    out[0] = total_ret;

    /* Annualized return */
    double periods = (n > 1) ? (double)(n - 1) : 0.0;
    double ann_ret = 0.0;
    if (periods > 0.0 && (1.0 + total_ret) > 0.0)
        ann_ret = pow(1.0 + total_ret, annual / periods) - 1.0;
    out[1] = ann_ret;

    /* Calculate returns for Sharpe */
    double *returns = (double *)malloc(n * sizeof(double));
    if (!returns) return 2;

    returns[0] = 0.0;
    for (size_t i = 1; i < n; i++) {
        returns[i] = (equity[i] - equity[i-1]) / equity[i-1];
    }

    /* Sharpe ratio */
    double mean_ret = 0.0, var_ret = 0.0;
    for (size_t i = 1; i < n; i++) {
        mean_ret += returns[i];
    }
    mean_ret /= (n - 1);

    for (size_t i = 1; i < n; i++) {
        double diff = returns[i] - mean_ret;
        var_ret += diff * diff;
    }
    var_ret /= (n - 1);

    double sharpe = (var_ret > 1e-10) ? (mean_ret * sqrt(annual)) / sqrt(var_ret) : 0.0;
    out[2] = sharpe;

    free(returns);

    /* Max drawdown */
    double peak = equity[0];
    double max_dd = 0.0;
    for (size_t i = 1; i < n; i++) {
        if (equity[i] > peak) peak = equity[i];
        double dd = (peak - equity[i]) / peak;
        if (dd > max_dd) max_dd = dd;
    }
    out[3] = -max_dd;

    /* Trade statistics */
    if (!trades || num_trades == 0) {
        out[4] = out[5] = out[6] = out[7] = out[8] = out[9] = 0.0;
        return 4;
    }

    double wins = 0.0, losses = 0.0;
    double sum_wins = 0.0, sum_losses = 0.0;
    double max_win = 0.0, max_loss = 0.0;
    size_t win_count = 0, loss_count = 0;

    for (size_t i = 0; i < n; i++) {
        if (fabs(trades[i]) > 1e-10) {
            if (trades[i] > 0) {
                wins += trades[i];
                sum_wins += trades[i];
                win_count++;
                if (trades[i] > max_win) max_win = trades[i];
            } else {
                losses += fabs(trades[i]);
                sum_losses += fabs(trades[i]);
                loss_count++;
                if (trades[i] < max_loss) max_loss = trades[i];
            }
        }
    }

    out[4] = (win_count + loss_count > 0) ? (double)win_count / (win_count + loss_count) : 0.0;
    out[5] = (losses > 1e-10) ? wins / losses : 0.0;
    out[6] = (win_count > 0) ? sum_wins / win_count : 0.0;
    out[7] = (loss_count > 0) ? sum_losses / loss_count : 0.0;
    out[8] = max_win;
    out[9] = max_loss;

    return 10;
}

/**
 * @brief Calculate slippage cost.
 *
 * Models market impact based on order size and liquidity.
 *
 * @param price Current price
 * @param size Order size (shares)
 * @param vol Current volume
 * @param avg_vol Average volume
 * @param lambda Market impact parameter (typically 0.1-0.5)
 * @return Slippage cost per share
 */
double exprtk_bt_slippage(double price, double size, double vol,
                           double avg_vol, double lambda) {
    if (price < 1e-10 || avg_vol < 1e-10) return 0.0;
    (void)vol;

    /* Market impact: price * lambda * (size/avg_vol)^0.5 */
    return price * lambda * sqrt(fabs(size) / avg_vol);
}

/**
 * @brief Calculate total trading cost.
 *
 * Combines commission, tax, and slippage.
 *
 * This helper is intentionally side-agnostic: tax_pct is charged on notional
 * for both buys and sells. Use market_commission() when market-specific
 * one-way duties matter.
 *
 * @param price Execution price
 * @param size Order size (shares)
 * @param commission_pct Commission rate (e.g., 0.001)
 * @param tax_pct Side-agnostic tax/fee rate on notional
 * @param slippage Slippage per share
 * @return Total cost
 */
double exprtk_bt_cost(double price, double size, double commission_pct,
                       double tax_pct, double slippage) {
    double notional = fabs(price * size);
    double commission = notional * commission_pct;
    double tax = notional * tax_pct;
    double slip_cost = fabs(size) * slippage;

    return commission + tax + slip_cost;
}
