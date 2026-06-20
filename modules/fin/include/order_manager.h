/**
 * @file order_manager.h
 * @brief Per-asset order management, position tracking, and trade logging.
 *
 * The order manager sits between the strategy signal and the market rules
 * validator. Responsibilities:
 *   - Track open positions per asset
 *   - Fill market orders at open price + slippage
 *   - Queue limit/stop orders and match them against each bar's H/L range
 *   - Auto-close positions that hit stop-loss or take-profit levels
 *   - Log closed trades for downstream stats (bt_stats)
 *   - Maintain cash and equity curve
 */
#ifndef ORDER_MANAGER_H
#define ORDER_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "market_rules.h"   /* universe_market_rules_t, order_error_t */
#include "exprtk_types.h"   /* mem_pool_t */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Order Types
 * ========================================================================= */

typedef enum {
    ORDER_TYPE_MARKET    = 0,
    ORDER_TYPE_LIMIT     = 1,
    ORDER_TYPE_STOP      = 2,
    ORDER_TYPE_STOP_LIMIT= 3,
} order_type_t;

typedef enum {
    ORDER_STATUS_PENDING   = 0,
    ORDER_STATUS_FILLED    = 1,
    ORDER_STATUS_REJECTED  = 2,
    ORDER_STATUS_CANCELLED = 3,
    ORDER_STATUS_EXPIRED   = 4,
} order_status_t;

/** A single order record */
typedef struct {
    uint64_t   id;              /**< Unique order ID                           */
    uint32_t   asset_id;        /**< Asset being traded                        */
    order_type_t type;
    int        direction;       /**< +1 = buy, -1 = sell/short                */
    double     quantity;        /**< Requested quantity (positive)             */
    double     limit_price;     /**< For LIMIT/STOP_LIMIT orders               */
    double     stop_price;      /**< For STOP/STOP_LIMIT orders                */
    double     stop_loss;       /**< Auto-close trigger: price level           */
    double     take_profit;     /**< Auto-close trigger: price level           */
    uint64_t   oco_link_id;     /**< Auto-cancel other orders with same link   */
    int        bars_valid;      /**< TIF: -1=GTC, >=0=expire after N bars      */
    order_status_t status;
    double     fill_price;      /**< Actual execution price                    */
    double     fill_date;       /**< Bar date when filled                      */
    double     fill_qty;        /**< Actual filled quantity                    */
} order_t;

/** A completed trade (one round-trip open→close) */
typedef struct {
    uint32_t asset_id;
    double   entry_price;
    double   exit_price;
    double   quantity;
    int      direction;         /**< +1 long, -1 short                         */
    double   entry_date;
    double   exit_date;
    double   pnl;               /**< Realized P&L (cash)                       */
    double   pnl_pct;           /**< P&L as % of entry notional               */
    double   commission;        /**< Total commission paid on this round-trip  */
} trade_record_t;

/* =========================================================================
 * Per-Asset Position State
 * ========================================================================= */

typedef struct {
    uint32_t asset_id;
    double   position;          /**< Signed quantity held (+long, -short)      */
    double   avg_entry_price;   /**< Volume-weighted average entry price       */
    double   entry_date;        /**< Date position was opened                  */
    double   bought_today;      /**< Qty bought this session (for T+1)         */
    double   stop_loss;         /**< Active SL level (0 = disabled)            */
    double   take_profit;       /**< Active TP level (0 = disabled)            */
    double   trailing_dist;     /**< Distance for trailing stop (0 = disabled) */
    bool     trailing_is_pct;   /**< True if trailing_dist is a percentage     */
    double   trailing_peak;     /**< Peak price reached for trailing stop      */
    double   unrealized_pnl;    /**< Marked-to-market P&L                      */
} position_t;

/* =========================================================================
 * Order Manager
 * ========================================================================= */

typedef struct order_executor_s order_executor_t;

typedef struct {
    /* Market rules (shared, not owned) */
    const universe_market_rules_t *rules;

    /* Positions: flat array indexed by asset array position */
    position_t  *positions;     /**< [capacity] slots                          */
    size_t       capacity;      /**< Max number of assets supported            */

    /* Pending orders (grows dynamically) */
    order_t     *pending;
    size_t       num_pending;
    size_t       pending_cap;

    /* Completed trade log */
    trade_record_t *trades;
    size_t          num_trades;
    size_t          trades_cap;

    /* Quick access equity-curve array (caller-managed, written each bar) */
    double     *equity_curve;
    size_t      equity_len;
    size_t      equity_cap;

    /* Account state */
    double      cash;
    double      equity;         /**< cash + mark-to-market of all positions    */
    double      initial_cash;

    /* Slippage model */
    double      slippage_pct;   /**< Fraction of price added on entry/exit     */
    double      spread;         /**< Fixed half-spread (absolute price units)  */

    /* Counters */
    uint64_t    next_order_id;
    double      current_date;

    order_executor_t *executor;  /**< Optional live executor */
    mem_pool_t *arena;
} order_manager_t;

/* =========================================================================
 * Live Execution Interface
 * ========================================================================= */

typedef struct order_executor_s order_executor_t;

struct order_executor_s {
    const char *name;
    void *user_data;

    /** 
     * Submit an order to a real exchange. 
     * For limit/stop orders, internal tracking still occurs if submission is OK.
     */
    order_error_t (*submit)(order_executor_t *self, order_t *order);
    
    /** Cancel an order on the exchange. */
    void (*cancel)(order_executor_t *self, uint64_t order_id);

    /** 
     * Sync state from exchange.  Should update mgr->cash and mgr->positions.
     * Can be called periodically or after specific events.
     */
    void (*sync)(order_executor_t *self, order_manager_t *mgr);
};

/**
 * @brief Register a live executor with the manager.
 */
void order_manager_set_executor(order_manager_t *mgr, order_executor_t *exec);

/**
 * @brief Sync state from live executor (cash, positions).
 */
void order_manager_sync(order_manager_t *mgr);

/**
 * @brief Report a fill from an external source (live executor).
 */
void order_manager_report_fill(order_manager_t *mgr,
                               uint64_t order_id,
                               uint32_t asset_id,
                               int direction,
                               double quantity,
                               double price,
                               double stop_loss,
                               double take_profit);

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Allocate an order manager.
 *
 * @param rules       Market-specific rules for validation.
 * @param cash0       Starting cash.
 * @param asset_cap   Maximum number of concurrent open positions.
 * @param arena       Arena for internal allocations.
 * @return Pointer to manager, or NULL on OOM.
 */
order_manager_t *order_manager_create(const universe_market_rules_t *rules,
                                       double cash0,
                                       size_t asset_cap,
                                       mem_pool_t *arena);

/** Free dynamic allocations (positions, pending, trades). Arena is untouched. */
void order_manager_free(order_manager_t *mgr);

/* =========================================================================
 * Per-Bar Processing
 * ========================================================================= */

/**
 * @brief Begin a new bar: advance session date, reset bought_today.
 * Must be called before any order submission for the current bar.
 *
 * @param date  Current bar date (epoch days).
 */
void order_manager_begin_bar(order_manager_t *mgr, double date);

/**
 * @brief Check pending limit/stop orders and open-position SL/TP triggers
 *        against the current bar's OHLC data.
 *
 * Must be called AFTER begin_bar() but BEFORE strategy signals are processed.
 *
 * @param asset_id   The asset being checked.
 * @param open High, low, close of the current bar.
 * @param prev_close Previous bar's close (for price-limit validation).
 */
void order_manager_check_triggers(order_manager_t *mgr,
                                   uint32_t asset_id,
                                   double open, double high, double low, double close,
                                   double prev_close);

/**
 * @brief Submit a market order for execution at the next open.
 *
 * For backtesting, execution occurs immediately at the bar's open price
 * (adjusted for slippage).  The order may be rejected if it violates
 * market rules.
 *
 * @param asset_id     Asset to trade.
 * @param direction    +1 buy, -1 sell.
 * @param fraction     Fraction of available equity to trade (0 < fraction ≤ 1).
 * @param exec_price   Execution price (typically bar's open).
 * @param prev_close   Previous close for price-limit check.
 * @param stop_loss    Stop-loss price (0 to skip).
 * @param take_profit  Take-profit price (0 to skip).
 * @return ORDER_OK or error code.
 */
order_error_t order_manager_market(order_manager_t *mgr,
                                    uint32_t asset_id,
                                    int direction,
                                    double fraction,
                                    double exec_price,
                                    double prev_close,
                                    double stop_loss,
                                    double take_profit);

order_error_t order_manager_limit(order_manager_t *mgr,
                                   uint32_t asset_id,
                                   int direction,
                                   double fraction,
                                   double limit_price,
                                   double stop_loss,
                                   double take_profit,
                                   int bars_valid,
                                   uint64_t *out_id);

/**
 * @brief Submit a stop order (queued, fills when price crosses stop_price).
 */
order_error_t order_manager_stop(order_manager_t *mgr,
                                  uint32_t asset_id,
                                  int direction,
                                  double fraction,
                                  double stop_price,
                                  double stop_loss,
                                  double take_profit,
                                  int bars_valid,
                                  uint64_t *out_id);

/**
 * @brief Close the full open position for an asset at exec_price.
 * A no-op if there is no position. Returns trade record or NULL.
 */
const trade_record_t *order_manager_close(order_manager_t *mgr,
                                           uint32_t asset_id,
                                           double exec_price,
                                           double date);

void order_manager_set_sl_tp(order_manager_t *mgr,
                              uint32_t asset_id,
                              double stop_loss,
                              double take_profit);

/**
 * @brief Set a trailing stop on the existing open position.
 */
void order_manager_set_trailing_stop(order_manager_t *mgr,
                                     uint32_t asset_id,
                                     double distance,
                                     bool is_pct);

/**
 * @brief Link two pending orders as One-Cancels-the-Other (OCO).
 */
void order_manager_link_oco(order_manager_t *mgr, uint64_t order_id1, uint64_t order_id2);

/**
 * @brief Cancel a specific pending order by its ID.
 */
void order_manager_cancel_order(order_manager_t *mgr, uint64_t order_id);

/**
 * @brief Cancel all pending orders for a specific asset.
 */
void order_manager_cancel_pending(order_manager_t *mgr, uint32_t asset_id);

/**
 * @brief Mark-to-market all positions and update equity.
 *
 * Must be called at end-of-bar with each asset's closing price so that
 * equity_curve reflects the current account value.
 *
 * @param asset_ids  Asset IDs aligned with the prices array.
 * @param prices     Array of close prices aligned with asset_ids.
 * @param n          Length of both arrays.
 */
void order_manager_mark_to_market(order_manager_t *mgr,
                                   const uint32_t *asset_ids,
                                   const double *prices,
                                   size_t n);

/**
 * @brief Append current equity to the equity curve buffer.
 * Grows dynamically.  Returns 0 on success, -1 on OOM.
 */
int order_manager_record_equity(order_manager_t *mgr);

/* =========================================================================
 * Query
 * ========================================================================= */

/**
 * @brief Get current position for an asset (signed quantity).
 * Returns 0 if no position or asset_idx out of range.
 */
double order_manager_position(const order_manager_t *mgr, uint32_t asset_id);

/**
 * @brief Get the average entry price for an open position.
 */
double order_manager_entry_price(const order_manager_t *mgr, uint32_t asset_id);

/**
 * @brief Get pointer to the completed trade log.
 */
const trade_record_t *order_manager_trades(const order_manager_t *mgr, size_t *out_count);

/**
 * @brief Extract the pnl_pct[] array from the trade log for use with bt_stats().
 * The caller must allocate out[] of size at least num_trades.
 * @return Number of trades written.
 */
size_t order_manager_trade_returns(const order_manager_t *mgr, double *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* ORDER_MANAGER_H */
