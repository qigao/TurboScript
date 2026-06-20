/**
 * @file order_manager.c
 * @brief Order manager implementation.
 *
 * Execution model (backtest):
 *   - Market orders: fill at bar OPEN + slippage on the *same* bar the signal fires.
 *   - Limit orders:  fill if bar LOW ≤ limit_price (buy) or HIGH ≥ limit_price (sell).
 *   - Stop orders:   fill if bar LOW ≤ stop_price  (sell) or HIGH ≥ stop_price  (buy).
 *   - SL/TP:         checked against each bar's H/L before processing new signals.
 *
 * Position accounting:
 *   - Average cost (VWAP entry) updated on each partial/full open.
 *   - Realized PnL logged to trade records on close.
 *   - Unrealized PnL mark-to-market'd at bar close via mark_to_market().
 */

#include "order_manager.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

#define INITIAL_PENDING_CAP  32
#define INITIAL_TRADES_CAP   256
#define INITIAL_EQUITY_CAP   4096

static position_t *find_position(order_manager_t *mgr, uint32_t asset_id) {
    for (size_t i = 0; i < mgr->capacity; i++)
        if (mgr->positions[i].asset_id == asset_id && mgr->positions[i].position != 0.0)
            return &mgr->positions[i];
    return NULL;
}

static position_t *find_or_create_position(order_manager_t *mgr, uint32_t asset_id) {
    /* First look for existing */
    for (size_t i = 0; i < mgr->capacity; i++)
        if (mgr->positions[i].asset_id == asset_id)
            return &mgr->positions[i];
    /* Find empty slot */
    for (size_t i = 0; i < mgr->capacity; i++) {
        if (mgr->positions[i].asset_id == 0) {
            memset(&mgr->positions[i], 0, sizeof(position_t));
            mgr->positions[i].asset_id = asset_id;
            return &mgr->positions[i];
        }
    }
    return NULL; /* no room */
}

static double apply_slippage(const order_manager_t *mgr, double price, int direction) {
    double slip = price * mgr->slippage_pct + mgr->spread;
    return price + direction * slip;
}

static int trades_grow(order_manager_t *mgr) {
    size_t new_cap = mgr->trades_cap * 2;
    trade_record_t *tmp = (trade_record_t *)realloc(mgr->trades, new_cap * sizeof(trade_record_t));
    if (!tmp) return -1;
    mgr->trades     = tmp;
    mgr->trades_cap = new_cap;
    return 0;
}

static int pending_grow(order_manager_t *mgr) {
    size_t new_cap = mgr->pending_cap * 2;
    order_t *tmp = (order_t *)realloc(mgr->pending, new_cap * sizeof(order_t));
    if (!tmp) return -1;
    mgr->pending     = tmp;
    mgr->pending_cap = new_cap;
    return 0;
}

/* Record a closed trade */
static void record_trade(order_manager_t *mgr,
                          uint32_t asset_id, int direction,
                          double entry_price, double exit_price,
                          double quantity, double entry_date, double exit_date,
                          double commission) {
    if (mgr->num_trades >= mgr->trades_cap)
        if (trades_grow(mgr) < 0) return;

    trade_record_t *t = &mgr->trades[mgr->num_trades++];
    t->asset_id    = asset_id;
    t->direction   = direction;
    t->entry_price = entry_price;
    t->exit_price  = exit_price;
    t->quantity    = quantity;
    t->entry_date  = entry_date;
    t->exit_date   = exit_date;
    t->commission  = commission;

    double notional = entry_price * quantity;
    t->pnl     = direction * (exit_price - entry_price) * quantity - commission;
    t->pnl_pct = (notional > 1e-9) ? (t->pnl / notional) : 0.0;
}

/* Internal: Update position and log trade after validation/slippage */
static void apply_fill(order_manager_t *mgr,
                        position_t *pos, int direction,
                        double quantity, double fill_price,
                        double comm,
                        double stop_loss, double take_profit) {
    uint32_t asset_id = pos->asset_id;
    mgr->cash -= comm;

    if (direction > 0) {
        /* Buy / long open */
        double old_notional = pos->avg_entry_price * fabs(pos->position);
        double new_notional = fill_price * quantity;
        pos->position      += quantity;
        pos->avg_entry_price = (pos->position > 1e-9)
            ? (old_notional + new_notional) / pos->position
            : fill_price;
        pos->bought_today  += quantity;
        pos->entry_date     = (pos->entry_date == 0.0) ? mgr->current_date : pos->entry_date;
        mgr->cash           -= fill_price * quantity;

        if (stop_loss > 0)   pos->stop_loss   = stop_loss;
        if (take_profit > 0) pos->take_profit = take_profit;
    } else {
        /* Sell / close long */
        double close_qty = quantity < fabs(pos->position) ? quantity : fabs(pos->position);
        mgr->cash += fill_price * close_qty;

        if (close_qty >= fabs(pos->position) - 1e-9) {
            /* Full close */
            record_trade(mgr, asset_id, (pos->position > 0) ? +1 : -1,
                         pos->avg_entry_price, fill_price, fabs(pos->position),
                         pos->entry_date, mgr->current_date, comm);
            pos->position        = 0.0;
            pos->avg_entry_price = 0.0;
            pos->entry_date      = 0.0;
            pos->stop_loss       = 0.0;
            pos->take_profit     = 0.0;
        } else {
            /* Partial close */
            record_trade(mgr, asset_id, (pos->position > 0) ? +1 : -1,
                         pos->avg_entry_price, fill_price, close_qty,
                         pos->entry_date, mgr->current_date, comm);
            pos->position -= (pos->position > 0) ? close_qty : -close_qty;
        }
    }
}

/* Execute a fill against an open or close */
static order_error_t execute_fill(order_manager_t *mgr,
                                   uint32_t asset_id, int direction,
                                   double quantity, double exec_price,
                                   double prev_close,
                                   double stop_loss, double take_profit) {
    /* Validate via market rules */
    position_t *pos = find_or_create_position(mgr, asset_id);
    if (!pos) return ORDER_ERR_ASSET_INACTIVE; /* no room */

    double bought_today = (direction > 0) ? pos->bought_today : 0.0;
    order_error_t err = market_validate_order(
        mgr->rules, true, exec_price, quantity, direction,
        prev_close, pos->position, bought_today, -1.0 /* skip session check */);
    if (err != ORDER_OK) return err;

    /* Apply slippage */
    double fill_price = apply_slippage(mgr, exec_price, direction);

    /* Calculate commission */
    double notional  = fill_price * quantity;
    double comm      = market_commission(mgr->rules, notional, direction < 0);

    apply_fill(mgr, pos, direction, quantity, fill_price, comm, stop_loss, take_profit);
    return ORDER_OK;
}

void order_manager_report_fill(order_manager_t *mgr,
                               uint64_t order_id,
                               uint32_t asset_id,
                               int direction,
                               double quantity,
                               double price,
                               double stop_loss,
                               double take_profit) {
    if (!mgr) return;

    /* 1. Mark pending order as filled if ID matches */
    if (order_id > 0) {
        for (size_t i = 0; i < mgr->num_pending; i++) {
            if (mgr->pending[i].id == order_id) {
                mgr->pending[i].status     = ORDER_STATUS_FILLED;
                mgr->pending[i].fill_price = price;
                mgr->pending[i].fill_date  = mgr->current_date;
                break;
            }
        }
    }

    /* 2. Apply the position change */
    position_t *pos = find_or_create_position(mgr, asset_id);
    if (!pos) return;

    double notional = price * quantity;
    double comm     = market_commission(mgr->rules, notional, direction < 0);

    apply_fill(mgr, pos, direction, quantity, price, comm, stop_loss, take_profit);

    /* 3. Compact pending if needed (or let next bar check do it) */
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

order_manager_t *order_manager_create(const universe_market_rules_t *rules,
                                       double cash0,
                                       size_t asset_cap,
                                       mem_pool_t *arena) {
    order_manager_t *mgr = (order_manager_t *)mem_alloc(arena, sizeof(order_manager_t));
    if (!mgr) return NULL;
    memset(mgr, 0, sizeof(*mgr));

    mgr->rules      = rules;
    mgr->cash       = cash0;
    mgr->equity     = cash0;
    mgr->initial_cash = cash0;
    mgr->capacity   = asset_cap;
    mgr->arena      = arena;
    mgr->slippage_pct = 0.0005;  /* 0.05% default */
    mgr->spread     = 0.0;
    mgr->next_order_id = 1;

    mgr->positions = (position_t *)malloc(asset_cap * sizeof(position_t));
    if (!mgr->positions) return NULL;
    memset(mgr->positions, 0, asset_cap * sizeof(position_t));

    mgr->pending = (order_t *)malloc(INITIAL_PENDING_CAP * sizeof(order_t));
    if (!mgr->pending) { free(mgr->positions); return NULL; }
    mgr->pending_cap = INITIAL_PENDING_CAP;

    mgr->trades = (trade_record_t *)malloc(INITIAL_TRADES_CAP * sizeof(trade_record_t));
    if (!mgr->trades) { free(mgr->pending); free(mgr->positions); return NULL; }
    mgr->trades_cap = INITIAL_TRADES_CAP;

    mgr->equity_curve = (double *)malloc(INITIAL_EQUITY_CAP * sizeof(double));
    if (!mgr->equity_curve) {
        free(mgr->trades); free(mgr->pending); free(mgr->positions);
        return NULL;
    }
    mgr->equity_cap = INITIAL_EQUITY_CAP;

    return mgr;
}

void order_manager_free(order_manager_t *mgr) {
    if (!mgr) return;
    free(mgr->positions);
    free(mgr->pending);
    free(mgr->trades);
    free(mgr->equity_curve);
    mgr->positions    = NULL;
    mgr->pending      = NULL;
    mgr->trades       = NULL;
    mgr->equity_curve = NULL;
}

void order_manager_set_executor(order_manager_t *mgr, order_executor_t *exec) {
    if (mgr) mgr->executor = exec;
}

void order_manager_sync(order_manager_t *mgr) {
    if (mgr && mgr->executor) {
        mgr->executor->sync(mgr->executor, mgr);
    }
}

/* =========================================================================
 * Per-Bar Processing
 * ========================================================================= */

void order_manager_begin_bar(order_manager_t *mgr, double date) {
    mgr->current_date = date;
    /* Reset intraday buy tracker for T+1 markets */
    if (mgr->rules && mgr->rules->t_plus >= 1) {
        for (size_t i = 0; i < mgr->capacity; i++)
            mgr->positions[i].bought_today = 0.0;
    }
}

void order_manager_check_triggers(order_manager_t *mgr,
                                   uint32_t asset_id,
                                   double open, double high, double low, double close,
                                   double prev_close) {
    (void)open; (void)close;

    /* 0. Check expiration of pending limit/stop orders */
    for (size_t i = 0; i < mgr->num_pending; i++) {
        order_t *o = &mgr->pending[i];
        if (o->asset_id == asset_id && o->status == ORDER_STATUS_PENDING) {
            if (o->bars_valid == 0) {
                o->status = ORDER_STATUS_EXPIRED;
            } else if (o->bars_valid > 0) {
                o->bars_valid--;
            }
        }
    }


    /* If we have a live executor, we rely on its sync() to update positions/orders.
     * Local trigger simulation is only for backtesting. */
    if (mgr->executor) return;

    /* 1. Check SL/TP on open positions */
    position_t *pos = find_position(mgr, asset_id);
    if (pos && pos->position != 0.0) {
        int is_long = pos->position > 0;

        /* Trailing stop: adjust SL if peak improved */
        if (pos->trailing_dist > 0.0) {
            if (is_long && high > pos->trailing_peak) {
                pos->trailing_peak = high;
                double new_sl = pos->trailing_is_pct ? 
                                pos->trailing_peak * (1.0 - pos->trailing_dist) :
                                pos->trailing_peak - pos->trailing_dist;
                if (new_sl > pos->stop_loss) pos->stop_loss = new_sl;
            } else if (!is_long && low < pos->trailing_peak) {
                pos->trailing_peak = low;
                double new_sl = pos->trailing_is_pct ? 
                                pos->trailing_peak * (1.0 + pos->trailing_dist) :
                                pos->trailing_peak + pos->trailing_dist;
                if (pos->stop_loss == 0.0 || new_sl < pos->stop_loss) pos->stop_loss = new_sl;
            }
        }

        /* Stop-loss: long hits below SL, short hits above SL */
        if (pos->stop_loss > 0.0) {
            int sl_hit = is_long ? (low <= pos->stop_loss) : (high >= pos->stop_loss);
            if (sl_hit) {
                double fill = is_long ? pos->stop_loss : pos->stop_loss;
                fill = market_clamp_price(mgr->rules, fill, prev_close);
                order_manager_close(mgr, asset_id, fill, mgr->current_date);
                return;
            }
        }

        /* Take-profit: long hits above TP, short hits below TP */
        if (pos->take_profit > 0.0) {
            int tp_hit = is_long ? (high >= pos->take_profit) : (low <= pos->take_profit);
            if (tp_hit) {
                double fill = is_long ? pos->take_profit : pos->take_profit;
                fill = market_clamp_price(mgr->rules, fill, prev_close);
                order_manager_close(mgr, asset_id, fill, mgr->current_date);
                return;
            }
        }
    }

    /* 2. Try to fill pending limit/stop orders */
    for (size_t i = 0; i < mgr->num_pending; ) {
        order_t *o = &mgr->pending[i];
        if (o->asset_id != asset_id || o->status != ORDER_STATUS_PENDING) {
            i++; continue;
        }

        int filled = 0;
        double fill_price = 0.0;

        if (o->type == ORDER_TYPE_LIMIT) {
            if (o->direction > 0 && low <= o->limit_price) {
                fill_price = o->limit_price; filled = 1;
            } else if (o->direction < 0 && high >= o->limit_price) {
                fill_price = o->limit_price; filled = 1;
            }
        } else if (o->type == ORDER_TYPE_STOP) {
            if (o->direction > 0 && high >= o->stop_price) {
                fill_price = o->stop_price; filled = 1;
            } else if (o->direction < 0 && low <= o->stop_price) {
                fill_price = o->stop_price; filled = 1;
            }
        }

        if (filled) {
            fill_price = market_clamp_price(mgr->rules, fill_price, prev_close);
            execute_fill(mgr, asset_id, o->direction, o->quantity,
                         fill_price, prev_close, o->stop_loss, o->take_profit);
            o->status     = ORDER_STATUS_FILLED;
            o->fill_price = fill_price;
            o->fill_date  = mgr->current_date;
            
            /* Handle OCO if linked */
            if (o->oco_link_id > 0) {
                for (size_t j = 0; j < mgr->num_pending; j++) {
                    if (mgr->pending[j].oco_link_id == o->oco_link_id && 
                        mgr->pending[j].status == ORDER_STATUS_PENDING && j != i) {
                        mgr->pending[j].status = ORDER_STATUS_CANCELLED;
                    }
                }
            }
        }
        i++;
    }

    /* Compact pending list: remove filled/cancelled orders */
    size_t w = 0;
    for (size_t i = 0; i < mgr->num_pending; i++) {
        if (mgr->pending[i].status == ORDER_STATUS_PENDING)
            mgr->pending[w++] = mgr->pending[i];
    }
    mgr->num_pending = w;
}

/* =========================================================================
 * Order Submission
 * ========================================================================= */

order_error_t order_manager_market(order_manager_t *mgr,
                                    uint32_t asset_id,
                                    int direction,
                                    double fraction,
                                    double exec_price,
                                    double prev_close,
                                    double stop_loss,
                                    double take_profit) {
    if (!mgr) return ORDER_ERR_ZERO_QTY;
    if (fraction <= 0.0) return ORDER_ERR_ZERO_QTY;
    if (fraction > 1.0) fraction = 1.0;
    if (exec_price <= 0.0) return ORDER_ERR_ZERO_QTY;

    /* Calculate quantity from equity fraction */
    double notional = mgr->equity * fraction;
    double raw_qty  = notional / exec_price;
    double quantity = market_round_lot(mgr->rules, raw_qty);
    if (quantity <= 0.0) return ORDER_ERR_ZERO_QTY;

    /* Ensure we have enough cash */
    double cost = quantity * exec_price * (1.0 + mgr->slippage_pct) + mgr->spread;
    if (direction > 0 && cost > mgr->cash)
        quantity = market_round_lot(mgr->rules, mgr->cash / (exec_price * (1.0 + mgr->slippage_pct)));
    if (quantity <= 0.0) return ORDER_ERR_ZERO_QTY;

    if (mgr->executor) {
        order_t order;
        memset(&order, 0, sizeof(order));
        order.id          = mgr->next_order_id++;
        order.asset_id    = asset_id;
        order.type        = ORDER_TYPE_MARKET;
        order.direction   = direction;
        order.quantity    = quantity;
        order.status      = ORDER_STATUS_PENDING;
        order.stop_loss   = stop_loss;
        order.take_profit = take_profit;
        return mgr->executor->submit(mgr->executor, &order);
    }

    return execute_fill(mgr, asset_id, direction, quantity,
                        exec_price, prev_close, stop_loss, take_profit);
}

order_error_t order_manager_limit(order_manager_t *mgr,
                                   uint32_t asset_id,
                                   int direction,
                                   double fraction,
                                   double limit_price,
                                   double stop_loss,
                                   double take_profit,
                                   int bars_valid,
                                   uint64_t *out_id) {
    if (out_id) *out_id = 0;
    if (mgr->num_pending >= mgr->pending_cap)
        if (pending_grow(mgr) < 0) return ORDER_ERR_ASSET_INACTIVE;

    double quantity = market_round_lot(mgr->rules, (mgr->equity * fraction) / limit_price);
    if (quantity <= 0.0) return ORDER_ERR_ZERO_QTY;

    order_t *o = &mgr->pending[mgr->num_pending++];
    memset(o, 0, sizeof(*o));
    o->id          = mgr->next_order_id++;
    o->asset_id    = asset_id;
    o->type        = ORDER_TYPE_LIMIT;
    o->direction   = direction;
    o->quantity    = quantity;
    o->limit_price = limit_price;
    o->stop_loss   = stop_loss;
    o->take_profit = take_profit;
    o->bars_valid  = bars_valid;
    o->status      = ORDER_STATUS_PENDING;
    if (out_id) *out_id = o->id;

    if (mgr->executor) {
        order_error_t err = mgr->executor->submit(mgr->executor, o);
        if (err != ORDER_OK) {
            mgr->num_pending--;
            return err;
        }
    }

    return ORDER_OK;
}

order_error_t order_manager_stop(order_manager_t *mgr,
                                  uint32_t asset_id,
                                  int direction,
                                  double fraction,
                                  double stop_price,
                                  double stop_loss,
                                  double take_profit,
                                  int bars_valid,
                                  uint64_t *out_id) {
    if (!mgr) return ORDER_ERR_ZERO_QTY;
    if (out_id) *out_id = 0;
    if (mgr->num_pending >= mgr->pending_cap)
        if (pending_grow(mgr) < 0) return ORDER_ERR_ASSET_INACTIVE;

    double quantity = market_round_lot(mgr->rules, (mgr->equity * fraction) / stop_price);
    if (quantity <= 0.0) return ORDER_ERR_ZERO_QTY;

    order_t *o = &mgr->pending[mgr->num_pending++];
    memset(o, 0, sizeof(*o));
    o->id          = mgr->next_order_id++;
    o->asset_id    = asset_id;
    o->type        = ORDER_TYPE_STOP;
    o->direction   = direction;
    o->quantity    = quantity;
    o->stop_price  = stop_price;
    o->stop_loss   = stop_loss;
    o->take_profit = take_profit;
    o->bars_valid  = bars_valid;
    o->status      = ORDER_STATUS_PENDING;
    if (out_id) *out_id = o->id;

    if (mgr->executor) {
        order_error_t err = mgr->executor->submit(mgr->executor, o);
        if (err != ORDER_OK) {
            mgr->num_pending--;
            return err;
        }
    }

    return ORDER_OK;
}

const trade_record_t *order_manager_close(order_manager_t *mgr,
                                           uint32_t asset_id,
                                           double exec_price,
                                           double date) {
    if (!mgr) return NULL;
    (void)date;
    position_t *pos = find_position(mgr, asset_id);
    if (!pos || pos->position == 0.0) return NULL;

    size_t trades_before = mgr->num_trades;
    int direction = (pos->position > 0) ? -1 : +1;
    double qty    = fabs(pos->position);

    if (mgr->executor) {
        order_t order;
        memset(&order, 0, sizeof(order));
        order.id        = mgr->next_order_id++;
        order.asset_id  = asset_id;
        order.type      = ORDER_TYPE_MARKET;
        order.direction = direction;
        order.quantity  = qty;
        order.status    = ORDER_STATUS_PENDING;
        mgr->executor->submit(mgr->executor, &order);
        return NULL; /* Live close doesn't return immediate trade record */
    }

    execute_fill(mgr, asset_id, direction, qty, exec_price, 0.0, 0.0, 0.0);

    if (mgr->num_trades > trades_before)
        return &mgr->trades[mgr->num_trades - 1];
    return NULL;
}

void order_manager_set_sl_tp(order_manager_t *mgr,
                              uint32_t asset_id,
                              double stop_loss,
                              double take_profit) {
    if (!mgr) return;
    position_t *pos = find_position(mgr, asset_id);
    if (!pos) return;
    if (stop_loss > 0)   pos->stop_loss   = stop_loss;
    if (take_profit > 0) pos->take_profit = take_profit;
}

void order_manager_set_trailing_stop(order_manager_t *mgr,
                                     uint32_t asset_id,
                                     double distance,
                                     bool is_pct) {
    if (!mgr) return;
    position_t *pos = find_position(mgr, asset_id);
    if (!pos || pos->position == 0.0) return;
    
    pos->trailing_dist   = distance;
    pos->trailing_is_pct = is_pct;
    /* Initialize peak to current average entry, it will adjust on next bar check */
    pos->trailing_peak   = pos->avg_entry_price;
}

void order_manager_link_oco(order_manager_t *mgr, uint64_t order_id1, uint64_t order_id2) {
    if (!mgr) return;
    uint64_t link = mgr->next_order_id++;
    for (size_t i = 0; i < mgr->num_pending; i++) {
        if (mgr->pending[i].id == order_id1 || mgr->pending[i].id == order_id2) {
            mgr->pending[i].oco_link_id = link;
        }
    }
}

void order_manager_cancel_order(order_manager_t *mgr, uint64_t order_id) {
    if (!mgr) return;
    for (size_t i = 0; i < mgr->num_pending; i++) {
        if (mgr->pending[i].id == order_id) {
            mgr->pending[i].status = ORDER_STATUS_CANCELLED;
            if (mgr->executor) {
                mgr->executor->cancel(mgr->executor, order_id);
            }
            break;
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < mgr->num_pending; i++)
        if (mgr->pending[i].status == ORDER_STATUS_PENDING)
            mgr->pending[w++] = mgr->pending[i];
    mgr->num_pending = w;
}

void order_manager_cancel_pending(order_manager_t *mgr, uint32_t asset_id) {
    if (!mgr) return;
    for (size_t i = 0; i < mgr->num_pending; i++) {
        if (mgr->pending[i].asset_id == asset_id) {
            mgr->pending[i].status = ORDER_STATUS_CANCELLED;
            if (mgr->executor) {
                mgr->executor->cancel(mgr->executor, mgr->pending[i].id);
            }
        }
    }

    /* Compact */
    size_t w = 0;
    for (size_t i = 0; i < mgr->num_pending; i++)
        if (mgr->pending[i].status == ORDER_STATUS_PENDING)
            mgr->pending[w++] = mgr->pending[i];
    mgr->num_pending = w;
}

/* =========================================================================
 * Mark-to-Market and Equity Curve
 * ========================================================================= */

void order_manager_mark_to_market(order_manager_t *mgr,
                                   const uint32_t *asset_ids,
                                   const double *prices,
                                   size_t n) {
    if (!mgr || !asset_ids || !prices) return;
    double pos_value = 0.0;
    for (size_t i = 0; i < mgr->capacity && i < n; i++) {
        position_t *p = &mgr->positions[i];
        if (p->position == 0.0 || p->asset_id == 0) continue;
        double price = 0.0;
        int found = 0;
        for (size_t j = 0; j < n; j++) {
            if (asset_ids[j] == p->asset_id) {
                price = prices[j];
                found = 1;
                break;
            }
        }
        if (!found) continue;
        double mkt = price * p->position;
        p->unrealized_pnl = mkt - p->avg_entry_price * fabs(p->position);
        pos_value += mkt;
    }
    mgr->equity = mgr->cash + pos_value;
}

int order_manager_record_equity(order_manager_t *mgr) {
    if (!mgr) return -1;
    if (mgr->equity_len >= mgr->equity_cap) {
        size_t new_cap = mgr->equity_cap * 2;
        double *tmp = (double *)realloc(mgr->equity_curve, new_cap * sizeof(double));
        if (!tmp) return -1;
        mgr->equity_curve = tmp;
        mgr->equity_cap   = new_cap;
    }
    mgr->equity_curve[mgr->equity_len++] = mgr->equity;
    return 0;
}

/* =========================================================================
 * Query
 * ========================================================================= */

double order_manager_position(const order_manager_t *mgr, uint32_t asset_id) {
    if (!mgr) return 0.0;
    for (size_t i = 0; i < mgr->capacity; i++)
        if (mgr->positions[i].asset_id == asset_id)
            return mgr->positions[i].position;
    return 0.0;
}

double order_manager_entry_price(const order_manager_t *mgr, uint32_t asset_id) {
    if (!mgr) return 0.0;
    for (size_t i = 0; i < mgr->capacity; i++)
        if (mgr->positions[i].asset_id == asset_id)
            return mgr->positions[i].avg_entry_price;
    return 0.0;
}

const trade_record_t *order_manager_trades(const order_manager_t *mgr, size_t *out_count) {
    if (!mgr) { if(out_count) *out_count=0; return NULL; }
    if (out_count) *out_count = mgr->num_trades;
    return mgr->trades;
}

size_t order_manager_trade_returns(const order_manager_t *mgr, double *out, size_t max) {
    if (!mgr) return 0;
    size_t n = mgr->num_trades < max ? mgr->num_trades : max;
    for (size_t i = 0; i < n; i++)
        out[i] = mgr->trades[i].pnl_pct;
    return n;
}
