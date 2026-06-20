/**
 * @file strategy_optimizer.c
 * @brief Walk-Forward Optimization engine.
 *
 * For each sliding window:
 *   1. Grid-search all parameter combinations on the in-sample period.
 *   2. Pick the combination that maximizes the chosen metric.
 *   3. Run the out-of-sample period with those params and record the score.
 *   4. Slide the window forward by step_bars and repeat.
 */

#include "strategy_optimizer.h"
#include "strategy.h"
#include "fin.h"
#include "exprtk.h"
#include "exprtk_module.h"
#include <string.h>
#include <math.h>

/* =========================================================================
 * Grid Helpers
 * ========================================================================= */

/** Compute total number of parameter combinations from ranges. */
static size_t param_grid_size(const double *ranges, size_t num_params) {
    size_t total = 1;
    for (size_t i = 0; i < num_params; i++) {
        double lo   = ranges[i * 3 + 0];
        double hi   = ranges[i * 3 + 1];
        double step = ranges[i * 3 + 2];
        if (step <= 0.0 || hi < lo) return 0;
        size_t count = (size_t)floor((hi - lo) / step) + 1;
        total *= count;
    }
    return total;
}

/**
 * Odometer-style extraction of the i-th parameter combination.
 * Writes num_params values into out[].
 */
static void param_grid_get(const double *ranges, size_t num_params,
                           size_t combo_idx, double *out) {
    size_t idx = combo_idx;
    for (size_t p = num_params; p > 0; p--) {
        size_t pi   = p - 1;
        double lo   = ranges[pi * 3 + 0];
        double hi   = ranges[pi * 3 + 1];
        double step = ranges[pi * 3 + 2];
        size_t count = (size_t)floor((hi - lo) / step) + 1;
        size_t slot  = idx % count;
        idx /= count;
        out[pi] = lo + (double)slot * step;
    }
}

/** Inject parameter values into the script environment. */
static void inject_params(exprtk_env_t *env, const char **names,
                          const double *values, size_t n) {
    for (size_t i = 0; i < n; i++) {
        exprtk_value_t v = { .type = EXPRTK_VAL_NUMBER, .data = { .number = values[i] } };
        exprtk_env_set(env, names[i], v);
    }
}

/* =========================================================================
 * Metric Evaluation
 * ========================================================================= */

/**
 * Compute the chosen metric from the strategy context's results.
 * Uses the equity curve for sharpe/sortino/calmar, trade returns for profit_factor.
 */
static double eval_metric(const strategy_ctx_t *ctx, wfo_metric_t metric) {
    size_t eq_len = 0;
    const double *eq = strategy_equity_curve(ctx, &eq_len);

    if (eq_len < 2) return -1e30;

    /* Compute bar-level returns from equity curve */
    size_t nr = eq_len - 1;
    /* Use stack for small arrays, heap for large */
    double stack_buf[1024];
    double *returns = (nr <= 1024) ? stack_buf : (double *)malloc(nr * sizeof(double));
    if (!returns) return -1e30;

    for (size_t i = 0; i < nr; i++) {
        returns[i] = (eq[i] > 1e-9) ? (eq[i + 1] - eq[i]) / eq[i] : 0.0;
    }

    double score;
    switch (metric) {
        case WFO_METRIC_SHARPE:
            score = exprtk_sharpe(returns, nr, 0.0, 252.0);
            break;
        case WFO_METRIC_SORTINO:
            score = exprtk_sortino(returns, nr, 0.0, 252.0);
            break;
        case WFO_METRIC_CALMAR:
            score = exprtk_calmar(eq, eq_len, 252.0);
            break;
        case WFO_METRIC_PROFIT_FACTOR: {
            double gross_profit = 0.0;
            double gross_loss = 0.0;
            size_t nt = 0;
            const trade_record_t *trades = order_manager_trades(ctx->order_mgr, &nt);
            for (size_t i = 0; i < nt; i++) {
                double pnl_pct = trades[i].pnl_pct;
                if (pnl_pct > 0.0) gross_profit += pnl_pct;
                else gross_loss -= pnl_pct;
            }
            score = (gross_loss > 1e-15) ? (gross_profit / gross_loss) : ((gross_profit > 0.0) ? 1e15 : 0.0);
            break;
        }
        default:
            score = -1e30;
            break;
    }

    if (returns != stack_buf) free(returns);

    /* Guard against NaN/Inf */
    if (isnan(score) || isinf(score)) score = -1e30;
    return score;
}

/** Collect the asset's bar dates so walk-forward windows are defined in bars, not calendar days. */
static double *collect_bar_dates(provider_t *provider, uint32_t asset_id,
                                 double start_date, double end_date,
                                 size_t *out_count) {
    void *stream;
    provider_bar_t bar;
    double *dates = NULL;
    size_t count = 0;
    size_t capacity = 0;
    mem_pool_t stream_arena;

    if (!provider || !provider->open_stream || !provider->next_bar || !out_count) return NULL;

    mem_init(&stream_arena, 4096);
    stream = provider->open_stream(provider, asset_id, start_date, end_date, &stream_arena);
    if (!stream) {
        mem_destroy(&stream_arena);
        return NULL;
    }

    while (provider->next_bar(provider, stream, &bar) == 1) {
        if (count == capacity) {
            size_t next_capacity = (capacity == 0) ? 64 : capacity * 2;
            double *next_dates = (double *)realloc(dates, next_capacity * sizeof(double));
            if (!next_dates) {
                free(dates);
                if (provider->close_stream) provider->close_stream(provider, stream);
                mem_destroy(&stream_arena);
                return NULL;
            }
            dates = next_dates;
            capacity = next_capacity;
        }
        dates[count++] = bar.date;
    }

    if (provider->close_stream) provider->close_stream(provider, stream);
    mem_destroy(&stream_arena);
    *out_count = count;
    return dates;
}

/* =========================================================================
 * Walk-Forward Engine
 * ========================================================================= */

int strategy_walk_forward(strategy_ctx_t *ctx,
                          const char *code,
                          uint32_t asset_id,
                          double start_date, double end_date,
                          const wfo_config_t *cfg,
                          wfo_result_t *result,
                          mem_pool_t *arena) {
    (void)code;
    if (!ctx || !cfg || !result || !arena) return -1;
    if (cfg->num_params == 0 || !cfg->param_names || !cfg->ranges) return -1;
    if (cfg->in_sample_bars == 0 || cfg->out_sample_bars == 0) return -1;
    if (cfg->step_bars == 0) return -1;

    size_t grid_sz = param_grid_size(cfg->ranges, cfg->num_params);
    if (grid_sz == 0) return -1;

    size_t window_size = cfg->in_sample_bars + cfg->out_sample_bars;
    size_t num_dates = 0;
    double *bar_dates;
    if (end_date <= start_date) return -1;

    bar_dates = collect_bar_dates(ctx->provider, asset_id, start_date, end_date, &num_dates);
    if (!bar_dates) return -1;
    if (num_dates < window_size) {
        free(bar_dates);
        return -1;
    }

    /* Estimate number of windows */
    size_t max_windows = 1 + (num_dates - window_size) / cfg->step_bars;

    /* Allocate result arrays */
    result->best_params = MEM_ALLOC_ARRAY(arena, double, max_windows * cfg->num_params);
    result->is_scores   = MEM_ALLOC_ARRAY(arena, double, max_windows);
    result->oos_scores  = MEM_ALLOC_ARRAY(arena, double, max_windows);
    if (!result->best_params || !result->is_scores || !result->oos_scores) {
        free(bar_dates);
        return -1;
    }

    /* Temp buffer for current parameter combination */
    double *cur_params = MEM_ALLOC_ARRAY(arena, double, cfg->num_params);
    if (!cur_params) {
        free(bar_dates);
        return -1;
    }

    size_t win_idx = 0;

    for (size_t start_idx = 0; start_idx + window_size <= num_dates; start_idx += cfg->step_bars) {
        double is_start = bar_dates[start_idx];
        double is_end   = bar_dates[start_idx + cfg->in_sample_bars - 1];
        double oos_start = bar_dates[start_idx + cfg->in_sample_bars];
        double oos_end   = bar_dates[start_idx + window_size - 1];

        /* ── In-sample grid search ────────────────────────────────── */
        double best_score = -1e30;
        size_t best_combo = 0;

        for (size_t ci = 0; ci < grid_sz; ci++) {
            param_grid_get(cfg->ranges, cfg->num_params, ci, cur_params);

            /* Reset and run IS period */
            strategy_reset(ctx);
            inject_params(ctx->env, cfg->param_names, cur_params, cfg->num_params);

            int rc = strategy_run_single(ctx, asset_id, is_start, is_end);
            if (rc <= 0) continue;

            double score = eval_metric(ctx, cfg->metric);
            if (score > best_score) {
                best_score = score;
                best_combo = ci;
            }
        }

        /* Store best IS params */
        param_grid_get(cfg->ranges, cfg->num_params, best_combo,
                       &result->best_params[win_idx * cfg->num_params]);
        result->is_scores[win_idx] = best_score;

        /* ── Out-of-sample validation ─────────────────────────────── */
        strategy_reset(ctx);
        inject_params(ctx->env, cfg->param_names,
                      &result->best_params[win_idx * cfg->num_params],
                      cfg->num_params);

        int rc = strategy_run_single(ctx, asset_id, oos_start, oos_end);
        result->oos_scores[win_idx] = (rc > 0) ? eval_metric(ctx, cfg->metric) : -1e30;

        win_idx++;
    }

    free(bar_dates);
    result->num_windows = win_idx;
    return 0;
}
