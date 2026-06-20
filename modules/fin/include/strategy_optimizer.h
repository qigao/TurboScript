/**
 * @file strategy_optimizer.h
 * @brief Walk-Forward Optimization for strategy parameter tuning.
 *
 * Sliding-window grid search: for each window, exhaustively search the
 * in-sample period, pick the best parameter set by chosen metric, then
 * validate on the out-of-sample period.  Results are stored per-window.
 */
#ifndef STRATEGY_OPTIMIZER_H
#define STRATEGY_OPTIMIZER_H

#include <stddef.h>
#include <stdint.h>
#include "strategy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Metric Selection
 * ========================================================================= */

typedef enum {
    WFO_METRIC_SHARPE        = 0,
    WFO_METRIC_SORTINO       = 1,
    WFO_METRIC_CALMAR        = 2,
    WFO_METRIC_PROFIT_FACTOR = 3
} wfo_metric_t;

/* =========================================================================
 * Configuration
 * ========================================================================= */

/**
 * Walk-forward configuration.
 *
 * param_names:    array of env variable names to inject (e.g. "period", "threshold")
 * ranges:         flat array [min1, max1, step1, min2, max2, step2, ...]
 *                 length = num_params * 3
 * num_params:     number of parameters to optimize
 * in_sample_bars: length of in-sample training window in observed bars
 * out_sample_bars:length of out-of-sample validation window in observed bars
 * step_bars:      how far to slide the window each iteration in observed bars
 * metric:         which performance metric to maximize
 */
typedef struct {
    const char **param_names;
    double      *ranges;         /* [min1,max1,step1, min2,max2,step2, ...] */
    size_t       num_params;
    size_t       in_sample_bars;
    size_t       out_sample_bars;
    size_t       step_bars;
    wfo_metric_t metric;
} wfo_config_t;

/* =========================================================================
 * Results
 * ========================================================================= */

/**
 * Walk-forward results (all arrays arena-allocated).
 *
 * best_params:  [num_windows * num_params] — best param set per window
 * is_scores:    [num_windows] — in-sample metric score per window
 * oos_scores:   [num_windows] — out-of-sample metric score per window
 * num_windows:  total number of walk-forward windows
 */
typedef struct {
    double *best_params;
    double *is_scores;
    double *oos_scores;
    size_t  num_windows;
} wfo_result_t;

/* =========================================================================
 * API
 * ========================================================================= */

/**
 * @brief Run walk-forward optimization on a single asset.
 *
 * @param ctx        Strategy context (must already have a compiled script).
 * @param code       Reserved for future use; currently ignored and may be NULL.
 * @param asset_id   Asset to optimize on.
 * @param start_date Start of the full data range (epoch days).
 * @param end_date   End of the full data range (epoch days).
 *
 * Windows are built from the asset's actual streamed bars inside [start_date, end_date].
 * Weekends, holidays, and data gaps do not count as bars.
 * @param cfg        Walk-forward configuration.
 * @param result     Output results (arrays allocated from arena).
 * @param arena      Arena for result allocations.
 * @return 0 on success, -1 on error.
 */
int strategy_walk_forward(strategy_ctx_t *ctx,
                          const char *code,
                          uint32_t asset_id,
                          double start_date, double end_date,
                          const wfo_config_t *cfg,
                          wfo_result_t *result,
                          mem_pool_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* STRATEGY_OPTIMIZER_H */
