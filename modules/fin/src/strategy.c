/**
 * @file strategy.c
 * @brief Event-driven on_bar strategy runner implementation.
 */

#include "strategy.h"
#include "fin.h"               /* exprtk_module_strategy, exprtk_module_ta, etc. */
#include "exprtk_types.h"
#include "exprtk.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Internal bind helpers
 * ========================================================================= */

/** Bind a named scalar variable into the TurboScript environment. */
static void ts_bind_num(exprtk_env_t *env, const char *name, double val) {
    exprtk_value_t v = { .type = EXPRTK_VAL_NUMBER, .data = { .number = val } };
    exprtk_env_set(env, name, v);
}

/** Bind a named vector (double array) into the TurboScript environment. */
static void ts_bind_vec(exprtk_env_t *env, const char *name,
                     const double *data, size_t len) {
    exprtk_env_set(env, name, exprtk_val_vec((double *)data, len));
}

/** Read a scalar output variable written by the strategy script. */
static double read_num(exprtk_env_t *env, const char *name, double def) {
    exprtk_value_t v = exprtk_env_get(env, name);
    if (v.type == EXPRTK_VAL_NUMBER) return v.data.number;
    return def;
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

strategy_ctx_t *strategy_create(universe_t *universe,
                                  provider_t *provider,
                                  const strategy_config_t *cfg,
                                  mem_pool_t *arena) {
    strategy_ctx_t *ctx = (strategy_ctx_t *)mem_alloc(arena, sizeof(strategy_ctx_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    ctx->universe    = universe;
    ctx->provider    = provider;
    ctx->arena       = arena;
    ctx->config      = cfg ? *cfg : strategy_config_default();
    ctx->warmup_bars = ctx->config.warmup_bars;
    ctx->num_assets  = universe ? universe->num_assets : 0;

    /* Allocate per-asset bar windows */
    if (ctx->num_assets > 0) {
        ctx->windows = (bar_window_t **)mem_alloc(
            arena, ctx->num_assets * sizeof(bar_window_t *));
        if (!ctx->windows) goto fail;
        for (size_t i = 0; i < ctx->num_assets; i++) {
            ctx->windows[i] = bar_window_create(ctx->config.window_capacity, arena);
            if (!ctx->windows[i]) goto fail;
        }
    }

    /* Create order manager */
    ctx->order_mgr = order_manager_create(
        provider ? provider->market : NULL,
        ctx->config.initial_cash,
        ctx->num_assets > 0 ? ctx->num_assets : 256,
        arena);
    if (!ctx->order_mgr) goto fail;

    ctx->order_mgr->slippage_pct = ctx->config.slippage_pct;
    ctx->order_mgr->spread       = ctx->config.spread;

    /* Create TurboScript environment */
    ctx->env = (exprtk_env_t *)mem_alloc(arena, sizeof(exprtk_env_t));
    if (!ctx->env) goto fail;
    exprtk_env_init(ctx->env);

    /* Mount strategy scripting module (buy/sell/flat/rank/...) */
    exprtk_env_add_module(ctx->env, exprtk_module_strategy());

    /* Pre-declare output variables so the script can assign them */
    ts_bind_num(ctx->env, "signal",      0.0);
    ts_bind_num(ctx->env, "size",        0.1);
    ts_bind_num(ctx->env, "stop_loss",   0.0);
    ts_bind_num(ctx->env, "take_profit", 0.0);

    /* Trade-log state (updated per-bar for adaptive strategies) */
    ts_bind_num(ctx->env, "num_trades", 0.0);
    ts_bind_num(ctx->env, "num_wins",   0.0);
    ts_bind_num(ctx->env, "cum_pnl",    0.0);
    ts_bind_num(ctx->env, "entry_price", 0.0);

    return ctx;

fail:
    if (ctx->env) exprtk_env_free(ctx->env);
    if (ctx->order_mgr) order_manager_free(ctx->order_mgr);
    return NULL;
}

void strategy_free(strategy_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->env) exprtk_env_free(ctx->env);
    if (ctx->order_mgr) order_manager_free(ctx->order_mgr);
    /* windows and ctx itself are arena-owned */
}

/* =========================================================================
 * Hook Detection & Invocation
 * ========================================================================= */

void strategy_init_hooks(strategy_ctx_t *ctx) {
    if (!ctx || !ctx->env) return;
    ctx->has_hooks    = false;
    ctx->hook_on_init = NULL;
    ctx->hook_on_bar  = NULL;
    ctx->hook_on_fill = NULL;
    ctx->hook_on_stop = NULL;

    for (exprtk_func_t *f = ctx->env->funcs; f; f = f->next) {
        if (!f->name || !f->is_script) continue;
        if      (strcmp(f->name, "on_init") == 0) ctx->hook_on_init = f;
        else if (strcmp(f->name, "on_bar")  == 0) ctx->hook_on_bar  = f;
        else if (strcmp(f->name, "on_fill") == 0) ctx->hook_on_fill = f;
        else if (strcmp(f->name, "on_stop") == 0) ctx->hook_on_stop = f;
    }
    ctx->has_hooks = (ctx->hook_on_init || ctx->hook_on_bar ||
                      ctx->hook_on_fill || ctx->hook_on_stop);
}

/** Invoke a script-defined hook function (0 args). */
static void call_hook(strategy_ctx_t *ctx, exprtk_func_t *hook) {
    if (!hook || !hook->data.script.body) return;
    exprtk_env_t *env = ctx->env;
    env->flow = exprtk_FLOW_NORMAL;
    exprtk_eval(hook->data.script.body, env);
    env->flow = exprtk_FLOW_NORMAL;
}

/** Invoke on_fill hook with a fill map argument. */
static void call_hook_on_fill(strategy_ctx_t *ctx, const trade_record_t *tr) {
    if (!ctx->hook_on_fill || !tr) return;
    exprtk_env_t *env = ctx->env;

    /* Build fill map and bind as "fill" variable */
    exprtk_value_t m = exprtk_val_map();
    exprtk_map_set(&m, "asset_id",  exprtk_val_num((double)tr->asset_id));
    exprtk_map_set(&m, "direction", exprtk_val_num((double)tr->direction));
    exprtk_map_set(&m, "price",     exprtk_val_num(tr->exit_price));
    exprtk_map_set(&m, "quantity",  exprtk_val_num(tr->quantity));
    exprtk_map_set(&m, "pnl",       exprtk_val_num(tr->pnl));
    exprtk_env_set(env, "fill", m);

    env->flow = exprtk_FLOW_NORMAL;
    exprtk_eval(ctx->hook_on_fill->data.script.body, env);
    env->flow = exprtk_FLOW_NORMAL;

    /* Free the map we allocated */
    exprtk_map_free(&m);
}

/* =========================================================================
 * Strategy Reset (for walk-forward re-runs)
 * ========================================================================= */

void strategy_reset(strategy_ctx_t *ctx) {
    if (!ctx) return;

    /* Reset order manager to initial state */
    if (ctx->order_mgr) {
        ctx->order_mgr->cash         = ctx->order_mgr->initial_cash;
        ctx->order_mgr->equity       = ctx->order_mgr->initial_cash;
        ctx->order_mgr->num_pending  = 0;
        ctx->order_mgr->num_trades   = 0;
        ctx->order_mgr->equity_len   = 0;
        ctx->order_mgr->next_order_id = 1;
        /* Zero out all positions */
        if (ctx->order_mgr->positions)
            memset(ctx->order_mgr->positions, 0,
                   ctx->order_mgr->capacity * sizeof(position_t));
    }

    /* Clear all bar windows */
    for (size_t i = 0; i < ctx->num_assets; i++) {
        if (ctx->windows && ctx->windows[i])
            bar_window_clear(ctx->windows[i]);
    }

    if (ctx->universe)
        universe_reset(ctx->universe);

    ctx->total_bars   = 0;
    ctx->current_date = 0.0;
}

/* =========================================================================
 * Script Compilation
 * ========================================================================= */

int strategy_compile(strategy_ctx_t *ctx, const char *code) {
    if (!ctx || !ctx->env || !code) return -1;
    exprtk_parse_ctx_t parse = {0};
    mem_pool_t scratch;
    mem_init(&scratch, 1024 * 256);
    parse.arena = &scratch;

    ctx->parsed = (struct exprtk_parse_ctx_s *)mem_alloc(
        ctx->arena, sizeof(exprtk_parse_ctx_t));
    if (!ctx->parsed) { mem_destroy(&scratch); return -1; }

    /* Parse the script */
    exprtk_parse_ctx_t *pc = (exprtk_parse_ctx_t *)ctx->parsed;
    *pc = parse;

    int err = 0;
    pc->root = exprtk_parse_ext(code, strlen(code), ctx->arena, &err,
                                 pc->error_msg, sizeof(pc->error_msg));
    pc->error = err;

    /* Execute top-level code to register script-defined functions */
    if (!err && pc->root) {
        exprtk_eval(pc->root, ctx->env);
        ctx->env->flow = exprtk_FLOW_NORMAL;
    }

    /* Detect on_init/on_bar/on_fill/on_stop hooks */
    if (!err) strategy_init_hooks(ctx);

    mem_destroy(&scratch);
    return err ? -1 : 0;
}

/* =========================================================================
 * Per-Bar Execution
 * ========================================================================= */

/** Bind bar data and context variables to script environment. */
static void bind_bar_context(strategy_ctx_t *ctx, bar_window_t *w,
                               uint32_t asset_id, double prev_close) {
    ts_bind_vec(ctx->env, "O", w->open,   w->count);
    ts_bind_vec(ctx->env, "H", w->high,   w->count);
    ts_bind_vec(ctx->env, "L", w->low,    w->count);
    ts_bind_vec(ctx->env, "C", w->close,  w->count);
    ts_bind_vec(ctx->env, "V", w->volume, w->count);
    ts_bind_num(ctx->env, "n",          (double)w->count);
    ts_bind_num(ctx->env, "asset_id",   (double)asset_id);
    ts_bind_num(ctx->env, "bar_index",  (double)ctx->total_bars);
    ts_bind_num(ctx->env, "position",   order_manager_position(ctx->order_mgr, asset_id));
    ts_bind_num(ctx->env, "entry_price",order_manager_entry_price(ctx->order_mgr, asset_id));
    ts_bind_num(ctx->env, "cash",       ctx->order_mgr->cash);
    ts_bind_num(ctx->env, "equity",     ctx->order_mgr->equity);
    ts_bind_num(ctx->env, "prev_close", prev_close);
}

/** Update trade statistics in script environment. */
static void bind_trade_stats(strategy_ctx_t *ctx) {
    size_t nt = ctx->order_mgr->num_trades;
    double nw = 0.0, cpnl = 0.0;
    for (size_t ti = 0; ti < nt; ti++) {
        cpnl += ctx->order_mgr->trades[ti].pnl;
        if (ctx->order_mgr->trades[ti].pnl > 0.0) nw += 1.0;
    }
    ts_bind_num(ctx->env, "num_trades", (double)nt);
    ts_bind_num(ctx->env, "num_wins",   nw);
    ts_bind_num(ctx->env, "cum_pnl",    cpnl);
}

/** Reset output variables before strategy execution. */
static void reset_output_vars(strategy_ctx_t *ctx) {
    ts_bind_num(ctx->env, "signal",      0.0);
    ts_bind_num(ctx->env, "size",        0.1);
    ts_bind_num(ctx->env, "signal_type", 0.0); /* 0: MKT, 1: LIMIT, 2: STOP */
    ts_bind_num(ctx->env, "signal_price",0.0);
    ts_bind_num(ctx->env, "bars_valid",  1.0); /* 1: DAY, -1: GTC */
    ts_bind_num(ctx->env, "stop_loss",   0.0);
    ts_bind_num(ctx->env, "take_profit", 0.0);
    ts_bind_num(ctx->env, "trailing_dist", 0.0);
    ts_bind_num(ctx->env, "trailing_pct",  0.0);
}

/** Execute the compiled strategy script. */
static void execute_strategy_script(strategy_ctx_t *ctx) {
    exprtk_parse_ctx_t *pc = (exprtk_parse_ctx_t *)ctx->parsed;
    if (ctx->has_hooks && ctx->hook_on_bar) {
        call_hook(ctx, ctx->hook_on_bar);
    } else if (pc->root) {
        exprtk_eval(pc->root, ctx->env);
    }
}

/** Process strategy signals and generate orders. */
static void process_strategy_signals(strategy_ctx_t *ctx, uint32_t asset_id,
                                       double open_price, double prev_close) {
    double sig  = read_num(ctx->env, "signal",      0.0);
    double sz   = read_num(ctx->env, "size",        0.1);
    double type = read_num(ctx->env, "signal_type", 0.0);
    double prc  = read_num(ctx->env, "signal_price",0.0);
    int valid   = (int)read_num(ctx->env, "bars_valid",  1.0);
    double sl   = read_num(ctx->env, "stop_loss",   0.0);
    double tp   = read_num(ctx->env, "take_profit", 0.0);
    double tdst = read_num(ctx->env, "trailing_dist", 0.0);
    double tpct = read_num(ctx->env, "trailing_pct",  0.0);

    double pos = order_manager_position(ctx->order_mgr, asset_id);

    /* Set trailing stop if requested */
    if (tdst > 0.0 && pos != 0.0) {
        order_manager_set_trailing_stop(ctx->order_mgr, asset_id, tdst, tpct > 0.0);
    }

    /* Buy signal */
    if (sig > 0.5 && pos <= 0.0) {
        if (pos < 0.0)
            order_manager_close(ctx->order_mgr, asset_id, open_price, ctx->current_date);

        if (type == 1.0) {
            order_manager_limit(ctx->order_mgr, asset_id, +1, sz, prc, sl, tp, valid, NULL);
        } else if (type == 2.0) {
            order_manager_stop(ctx->order_mgr, asset_id, +1, sz, prc, sl, tp, valid, NULL);
        } else {
            order_manager_market(ctx->order_mgr, asset_id, +1, sz,
                                  open_price, prev_close, sl, tp);
        }
    }
    /* Sell/short signal */
    else if (sig < -0.5 && pos >= 0.0) {
        if (pos > 0.0)
            order_manager_close(ctx->order_mgr, asset_id, open_price, ctx->current_date);

        if (ctx->order_mgr->rules && ctx->order_mgr->rules->short_allowed) {
            if (type == 1.0) {
                order_manager_limit(ctx->order_mgr, asset_id, -1, sz, prc, sl, tp, valid, NULL);
            } else if (type == 2.0) {
                order_manager_stop(ctx->order_mgr, asset_id, -1, sz, prc, sl, tp, valid, NULL);
            } else {
                order_manager_market(ctx->order_mgr, asset_id, -1, sz,
                                      open_price, prev_close, sl, tp);
            }
        }
    }
    /* Flat signal */
    else if (fabs(sig) < 0.01 && pos != 0.0) {
        order_manager_cancel_pending(ctx->order_mgr, asset_id);
        order_manager_close(ctx->order_mgr, asset_id, open_price, ctx->current_date);
    }
}

void strategy_on_bar(strategy_ctx_t *ctx,
                      uint32_t asset_id,
                      const provider_bar_t *bar,
                      double prev_close,
                      bool is_warmup) {
    /* Find array index for this asset */
    size_t idx = universe_find_asset(ctx->universe, asset_id);
    if (idx == SIZE_MAX) return;

    bar_window_t *w = ctx->windows[idx];

    /* 1. Push adjusted bar into window */
    double prev_adj = universe_adj_factor(ctx->universe, asset_id);
    universe_apply_runtime_adjustments(ctx->universe, asset_id, bar->date, prev_close);
    double adj = universe_adj_factor(ctx->universe, asset_id);
    if (w && w->count > 0 && prev_adj > 0.0 && adj > 0.0 && prev_adj != adj)
        bar_window_rescale_prices(w, adj / prev_adj);
    bar_window_push_adjusted(w, bar->date,
                              bar->open, bar->high, bar->low, bar->close, bar->volume,
                              adj);

    /* During warm-up: update indicators but don't trade */
    if (is_warmup || !bar_window_ready(w, ctx->warmup_bars)) return;

    /* 2. Linearize ring buffer → chronological arrays */
    bar_window_linearize(w);

    /* 3. Check SL/TP and pending orders against this bar's H/L */
    order_manager_check_triggers(ctx->order_mgr, asset_id,
                                  bar->open, bar->high, bar->low, bar->close,
                                  prev_close);

    if (!ctx->parsed || !ctx->env) return;

    /* Bind context and execute strategy */
    bind_bar_context(ctx, w, asset_id, prev_close);
    bind_trade_stats(ctx);
    reset_output_vars(ctx);
    execute_strategy_script(ctx);
    process_strategy_signals(ctx, asset_id, bar->open, prev_close);
}

/* =========================================================================
 * Single-Asset Backtest
 * ========================================================================= */

int strategy_run_single(strategy_ctx_t *ctx,
                          uint32_t asset_id,
                          double start_date,
                          double end_date) {
    if (!ctx || !ctx->provider || !ctx->provider->open_stream) return -1;

    provider_t *p = ctx->provider;

    /* Find asset in universe */
    size_t idx = universe_find_asset(ctx->universe, asset_id);
    if (idx == SIZE_MAX) return -1;
    const universe_asset_t *a = &ctx->universe->assets[idx];

    if (start_date <= 0.0) start_date = a->start_date;
    if (end_date   <= 0.0) end_date   = (a->end_date > 0.0) ? a->end_date : 1e12;

    /* Open bar stream */
    void *stream = p->open_stream(p, asset_id, start_date, end_date, ctx->arena);
    if (!stream) return -1;

    provider_bar_t bar;
    provider_bar_t prev_bar = {0};
    uint32_t mtm_asset_ids[1] = { asset_id };
    int processed = 0;

    order_manager_begin_bar(ctx->order_mgr, start_date);

    /* Call on_init() hook before the bar loop */
    if (ctx->has_hooks && ctx->hook_on_init)
        call_hook(ctx, ctx->hook_on_init);

    /* Track trade count to detect new fills */
    size_t prev_num_trades = ctx->order_mgr->num_trades;

    while (p->next_bar(p, stream, &bar) == 1) {
        universe_advance(ctx->universe, bar.date);
        order_manager_begin_bar(ctx->order_mgr, bar.date);
        ctx->current_date = bar.date;

        bool warmup = (processed < (int)ctx->warmup_bars);
        strategy_on_bar(ctx, asset_id, &bar, prev_bar.close, warmup);

        /* Fire on_fill() for any new trades generated this bar */
        if (ctx->has_hooks && ctx->hook_on_fill) {
            size_t cur_trades = ctx->order_mgr->num_trades;
            for (size_t ti = prev_num_trades; ti < cur_trades; ti++)
                call_hook_on_fill(ctx, &ctx->order_mgr->trades[ti]);
            prev_num_trades = cur_trades;
        }

        /* Mark-to-market using this bar's close */
        double closes[1] = { bar.close * universe_adj_factor(ctx->universe, asset_id) };
        order_manager_mark_to_market(ctx->order_mgr, mtm_asset_ids, closes, 1);
        order_manager_record_equity(ctx->order_mgr);

        prev_bar = bar;
        processed++;
        ctx->total_bars++;
    }

    p->close_stream(p, stream);

    /* Close any open position at the last price */
    if (prev_bar.close > 0.0) {
        const trade_record_t *final_tr =
            order_manager_close(ctx->order_mgr, asset_id, prev_bar.close, end_date);
        if (ctx->has_hooks && ctx->hook_on_fill && final_tr)
            call_hook_on_fill(ctx, final_tr);
    }

    /* Call on_stop() hook after the bar loop */
    if (ctx->has_hooks && ctx->hook_on_stop)
        call_hook(ctx, ctx->hook_on_stop);

    return processed;
}

/* =========================================================================
 * Multi-Asset Universe Backtest
 * ========================================================================= */

int strategy_run_universe(strategy_ctx_t *ctx,
                            double start_date,
                            double end_date) {
    if (!ctx || !ctx->universe || !ctx->provider) return -1;
    if (!ctx->provider->open_stream || !ctx->provider->next_bar || !ctx->provider->close_stream)
        return -1;

    size_t na = ctx->universe->num_assets;
    int total = 0;
    void **streams = (void **)mem_alloc(ctx->arena, na * sizeof(*streams));
    provider_bar_t *bars = (provider_bar_t *)mem_alloc(ctx->arena, na * sizeof(*bars));
    uint8_t *has_bar = (uint8_t *)mem_alloc(ctx->arena, na * sizeof(*has_bar));
    double *prev_close = (double *)mem_alloc(ctx->arena, na * sizeof(*prev_close));
    double *mtm_prices = (double *)mem_alloc(ctx->arena, na * sizeof(*mtm_prices));
    uint32_t *asset_ids = (uint32_t *)mem_alloc(ctx->arena, na * sizeof(*asset_ids));
    size_t *processed = (size_t *)mem_alloc(ctx->arena, na * sizeof(*processed));

    if (!streams || !bars || !has_bar || !prev_close || !mtm_prices || !asset_ids || !processed)
        return -1;

    memset(streams, 0, na * sizeof(*streams));
    memset(has_bar, 0, na * sizeof(*has_bar));
    memset(prev_close, 0, na * sizeof(*prev_close));
    memset(mtm_prices, 0, na * sizeof(*mtm_prices));
    memset(processed, 0, na * sizeof(*processed));

    for (size_t i = 0; i < na; i++) {
        const universe_asset_t *a = &ctx->universe->assets[i];
        double s = (start_date > 0) ? start_date : a->start_date;
        double e = (end_date > 0) ? end_date : (a->end_date > 0 ? a->end_date : 1e12);

        asset_ids[i] = a->id;
        if (ctx->windows[i]) bar_window_clear(ctx->windows[i]);

        streams[i] = ctx->provider->open_stream(ctx->provider, a->id, s, e, ctx->arena);
        if (!streams[i])
            continue;
        if (ctx->provider->next_bar(ctx->provider, streams[i], &bars[i]) == 1) {
            has_bar[i] = 1;
        } else {
            ctx->provider->close_stream(ctx->provider, streams[i]);
            streams[i] = NULL;
        }
    }

    if (ctx->has_hooks && ctx->hook_on_init)
        call_hook(ctx, ctx->hook_on_init);

    size_t prev_num_trades = ctx->order_mgr->num_trades;

    for (;;) {
        double next_date = 0.0;
        int found = 0;
        for (size_t i = 0; i < na; i++) {
            if (!has_bar[i])
                continue;
            if (!found || bars[i].date < next_date) {
                next_date = bars[i].date;
                found = 1;
            }
        }
        if (!found)
            break;

        universe_advance(ctx->universe, next_date);
        order_manager_begin_bar(ctx->order_mgr, next_date);
        ctx->current_date = next_date;

        for (size_t i = 0; i < na; i++) {
            const universe_asset_t *a = &ctx->universe->assets[i];
            if (!has_bar[i] || bars[i].date != next_date)
                continue;

            strategy_on_bar(ctx, a->id, &bars[i], prev_close[i], processed[i] < ctx->warmup_bars);

            if (ctx->has_hooks && ctx->hook_on_fill) {
                size_t cur_trades = ctx->order_mgr->num_trades;
                for (size_t ti = prev_num_trades; ti < cur_trades; ti++)
                    call_hook_on_fill(ctx, &ctx->order_mgr->trades[ti]);
                prev_num_trades = cur_trades;
            }

            prev_close[i] = bars[i].close;
            mtm_prices[i] = bars[i].close * universe_adj_factor(ctx->universe, a->id);
            processed[i]++;
            total++;
            ctx->total_bars++;

            if (ctx->provider->next_bar(ctx->provider, streams[i], &bars[i]) != 1) {
                ctx->provider->close_stream(ctx->provider, streams[i]);
                streams[i] = NULL;
                has_bar[i] = 0;
            }
        }

        order_manager_mark_to_market(ctx->order_mgr, asset_ids, mtm_prices, na);
        order_manager_record_equity(ctx->order_mgr);
    }

    for (size_t i = 0; i < na; i++) {
        if (streams[i])
            ctx->provider->close_stream(ctx->provider, streams[i]);
    }

    for (size_t i = 0; i < na; i++) {
        const trade_record_t *final_tr;
        if (mtm_prices[i] <= 0.0)
            continue;
        final_tr = order_manager_close(ctx->order_mgr, asset_ids[i], mtm_prices[i], ctx->current_date);
        if (ctx->has_hooks && ctx->hook_on_fill && final_tr)
            call_hook_on_fill(ctx, final_tr);
    }

    if (ctx->has_hooks && ctx->hook_on_stop)
        call_hook(ctx, ctx->hook_on_stop);

    return total;
}

/* =========================================================================
 * Results Access
 * ========================================================================= */

const double *strategy_equity_curve(const strategy_ctx_t *ctx, size_t *out_len) {
    if (!ctx || !ctx->order_mgr) { if (out_len) *out_len = 0; return NULL; }
    if (out_len) *out_len = ctx->order_mgr->equity_len;
    return ctx->order_mgr->equity_curve;
}

size_t strategy_trade_returns(const strategy_ctx_t *ctx, double *out, size_t max) {
    if (!ctx || !ctx->order_mgr) return 0;
    return order_manager_trade_returns(ctx->order_mgr, out, max);
}

double strategy_final_equity(const strategy_ctx_t *ctx) {
    if (!ctx || !ctx->order_mgr) return 0.0;
    return ctx->order_mgr->equity;
}

double strategy_total_return(const strategy_ctx_t *ctx) {
    if (!ctx || !ctx->order_mgr) return 0.0;
    double init = ctx->order_mgr->initial_cash;
    return (init > 1e-9) ? (ctx->order_mgr->equity - init) / init : 0.0;
}
