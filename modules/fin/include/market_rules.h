/**
 * @file market_rules.h
 * @brief Market-specific trading rules and order validation.
 *
 * Defines pre-built rule sets for common markets and a validation function
 * that enforces them at every order submission.  The strategy layer remains
 * market-agnostic; correctness is enforced here.
 *
 * Supported constraint categories:
 *   - Lot size  (minimum and multiple-of quantity)
 *   - T+N settlement  (e.g., CN A-shares T+1 no-same-day-sell)
 *   - Daily price limits  (CN ±10%, ChiNext ±20%)
 *   - Short-selling restrictions  (CN A-shares: no short)
 *   - Fractional-share trading  (crypto: yes, equities: no)
 *   - Trading session hours
 */
#ifndef MARKET_RULES_H
#define MARKET_RULES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Market Rule Descriptor
 * ========================================================================= */

typedef struct {
    const char *name;               /**< Human-readable id: "US_EQUITY", "CN_ASTOCK" */

    /* ── Quantity constraints ──────────────────────────────────────────── */
    double  lot_size_default;       /**< Min tradable unit (1, 100, 0.001, …)         */
    int     fractional_allowed;     /**< 1 = crypto-style partial lots OK             */

    /* ── Settlement ────────────────────────────────────────────────────── */
    uint8_t t_plus;                 /**< 0=T+0, 1=T+1 (can't sell same-day buys)     */

    /* ── Price limits ───────────────────────────────────────────────────── */
    int     has_price_limit;        /**< 1 = daily price circuit-breaker             */
    double  price_limit_up;         /**< Upper bound fraction, e.g. 0.10 (+10%)      */
    double  price_limit_dn;         /**< Lower bound fraction, e.g. 0.10 (-10%)      */

    /* ── Short selling ──────────────────────────────────────────────────── */
    int     short_allowed;          /**< 0 = no short (CN A-shares main board)       */

    /* ── Commission defaults (can be overridden per-account) ────────────── */
    double  commission_rate;        /**< Fraction of notional, e.g. 0.0003           */
    double  min_commission;         /**< Minimum per-trade, e.g. 5.0 (USD/CNY)       */
    double  stamp_duty;             /**< One-way tax on sell (CN: 0.001)             */

    /* ── Trading calendar ───────────────────────────────────────────────── */
    uint8_t trading_days_mask;      /**< Bit 0=Mon … Bit 6=Sun; 0x1F=Mon-Fri, 0x7F=all */
    double  session_open;           /**< Decimal hours: 9.5 = 09:30                  */
    double  session_close;          /**< Decimal hours: 16.0, 15.0, 24.0             */
    int     has_lunch_break;        /**< 1 = midday pause (CN: 11:30-13:00)          */
    double  lunch_start;            /**< Decimal hours: 11.5                         */
    double  lunch_end;              /**< Decimal hours: 13.0                         */

} universe_market_rules_t;

/* =========================================================================
 * Pre-defined Market Configurations
 * ========================================================================= */

/** US Equities (NYSE / NASDAQ) */
extern const universe_market_rules_t MARKET_US_EQUITY;

/** China A-Shares — Main Board (Shanghai + Shenzhen) ±10% limit, T+1, no short */
extern const universe_market_rules_t MARKET_CN_ASTOCK;

/** China ChiNext / STAR Market — ±20% first 5 days, ±20% normal, T+1, limited short */
extern const universe_market_rules_t MARKET_CN_CHINEXT;

/** Hong Kong Equities (HKEX) — T+2 settlement, no circuit breaker for most stocks */
extern const universe_market_rules_t MARKET_HK_EQUITY;

/** Cryptocurrency — 24/7, fractional, no price limits */
extern const universe_market_rules_t MARKET_CRYPTO;

/** Forex (spot) — Mon-Fri continuous, pip-sized ticks, 1000-unit lots */
extern const universe_market_rules_t MARKET_FOREX;

/** US Equity Options / Futures (generic) */
extern const universe_market_rules_t MARKET_US_FUTURE;

/* =========================================================================
 * Order Validation
 * ========================================================================= */

/** Return codes from market_validate_order() */
typedef enum {
    ORDER_OK                = 0,
    ORDER_ERR_T_PLUS        = 1,    /**< Selling shares bought today (T+1 violation)  */
    ORDER_ERR_LOT_SIZE      = 2,    /**< Qty not a valid multiple of lot_size         */
    ORDER_ERR_PRICE_LIMIT   = 3,    /**< Price outside daily circuit-breaker range    */
    ORDER_ERR_NO_SHORT      = 4,    /**< Short selling prohibited in this market      */
    ORDER_ERR_MARKET_CLOSED = 5,    /**< Order outside trading session hours          */
    ORDER_ERR_ASSET_INACTIVE= 6,    /**< Asset not currently in universe              */
    ORDER_ERR_ZERO_QTY      = 7,    /**< Order quantity is zero or negative           */
    ORDER_ERR_MARGIN        = 8,    /**< Insufficient margin for leveraged instrument */
} order_error_t;

/**
 * @brief Validate a proposed order against the market rules.
 *
 * @param rules       Market rule set to use.
 * @param asset_active  Whether the asset is currently in the universe.
 * @param price       Proposed execution price.
 * @param quantity    Proposed quantity (positive = buy/cover, negative = sell/short).
 * @param direction   +1 = buy/long, -1 = sell/short.
 * @param prev_close  Previous bar's close (for price-limit calculation).
 * @param current_pos Current position in this asset (+/- shares held).
 * @param bought_today  Quantity bought during the same trading session (for T+1 check).
 * @param session_time  Current time as decimal hours (e.g. 10.5 = 10:30).
 *                      Pass -1.0 to skip session-hour check (backtest default).
 * @return ORDER_OK (0) or one of the ORDER_ERR_* codes.
 */
order_error_t market_validate_order(
    const universe_market_rules_t *rules,
    bool     asset_active,
    double   price,
    double   quantity,
    int      direction,
    double   prev_close,
    double   current_pos,
    double   bought_today,
    double   session_time
);

/**
 * @brief Round a quantity down to the nearest valid lot boundary.
 *        Returns the largest valid quantity ≤ quantity.
 */
double market_round_lot(const universe_market_rules_t *rules, double quantity);

/**
 * @brief Clamp a price to the valid daily range around prev_close.
 *        If the market has no price limit, returns price unchanged.
 */
double market_clamp_price(const universe_market_rules_t *rules,
                          double price, double prev_close);

/**
 * @brief Calculate total commission for a trade.
 *        Includes commission_rate, min_commission, and stamp_duty (sells only).
 *
 * @param rules      Market rule set.
 * @param notional   Absolute trade value (price * quantity).
 * @param is_sell    1 if a sell/short-open (applies stamp_duty).
 * @return Total cost as a positive value.
 */
double market_commission(const universe_market_rules_t *rules,
                         double notional, int is_sell);

/**
 * @brief Return a human-readable name for an order_error_t code.
 */
const char *order_error_str(order_error_t err);

#ifdef __cplusplus
}
#endif

#endif /* MARKET_RULES_H */
