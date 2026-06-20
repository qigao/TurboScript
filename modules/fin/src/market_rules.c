/**
 * @file market_rules.c
 * @brief Pre-defined market rule sets and order validation implementation.
 */

#include "market_rules.h"
#include <math.h>

/* =========================================================================
 * Pre-defined Market Configurations
 * ========================================================================= */

const universe_market_rules_t MARKET_US_EQUITY = {
    .name                = "US_EQUITY",
    .lot_size_default    = 1.0,
    .fractional_allowed  = 0,
    .t_plus              = 0,
    .has_price_limit     = 0,
    .price_limit_up      = 0.0,
    .price_limit_dn      = 0.0,
    .short_allowed       = 1,
    .commission_rate     = 0.0005,
    .min_commission      = 1.0,
    .stamp_duty          = 0.0,
    .trading_days_mask   = 0x1F,   /* Mon–Fri */
    .session_open        = 9.5,    /* 09:30 */
    .session_close       = 16.0,
    .has_lunch_break     = 0,
    .lunch_start         = 0.0,
    .lunch_end           = 0.0,
};

const universe_market_rules_t MARKET_CN_ASTOCK = {
    .name                = "CN_ASTOCK",
    .lot_size_default    = 100.0,
    .fractional_allowed  = 0,
    .t_plus              = 1,
    .has_price_limit     = 1,
    .price_limit_up      = 0.10,
    .price_limit_dn      = 0.10,
    .short_allowed       = 0,
    .commission_rate     = 0.0003,
    .min_commission      = 5.0,
    .stamp_duty          = 0.001,  /* sell side only */
    .trading_days_mask   = 0x1F,
    .session_open        = 9.5,    /* 09:30 */
    .session_close       = 15.0,
    .has_lunch_break     = 1,
    .lunch_start         = 11.5,   /* 11:30 */
    .lunch_end           = 13.0,
};

const universe_market_rules_t MARKET_CN_CHINEXT = {
    .name                = "CN_CHINEXT",
    .lot_size_default    = 100.0,
    .fractional_allowed  = 0,
    .t_plus              = 1,
    .has_price_limit     = 1,
    .price_limit_up      = 0.20,   /* ChiNext / STAR: ±20% */
    .price_limit_dn      = 0.20,
    .short_allowed       = 0,
    .commission_rate     = 0.0003,
    .min_commission      = 5.0,
    .stamp_duty          = 0.001,
    .trading_days_mask   = 0x1F,
    .session_open        = 9.5,
    .session_close       = 15.0,
    .has_lunch_break     = 1,
    .lunch_start         = 11.5,
    .lunch_end           = 13.0,
};

const universe_market_rules_t MARKET_HK_EQUITY = {
    .name                = "HK_EQUITY",
    .lot_size_default    = 1.0,    /* varies per stock; 1 is conservative default */
    .fractional_allowed  = 0,
    .t_plus              = 0,      /* HK: T+2 settlement but can sell same day    */
    .has_price_limit     = 0,
    .price_limit_up      = 0.0,
    .price_limit_dn      = 0.0,
    .short_allowed       = 1,
    .commission_rate     = 0.0025,
    .min_commission      = 50.0,   /* HKD */
    .stamp_duty          = 0.0013, /* HK stamp duty 0.13% both sides */
    .trading_days_mask   = 0x1F,
    .session_open        = 9.5,    /* 09:30 HKT */
    .session_close       = 16.0,
    .has_lunch_break     = 1,
    .lunch_start         = 12.0,
    .lunch_end           = 13.0,
};

const universe_market_rules_t MARKET_CRYPTO = {
    .name                = "CRYPTO",
    .lot_size_default    = 0.001,
    .fractional_allowed  = 1,
    .t_plus              = 0,
    .has_price_limit     = 0,
    .price_limit_up      = 0.0,
    .price_limit_dn      = 0.0,
    .short_allowed       = 1,
    .commission_rate     = 0.001,  /* 0.10% taker typical */
    .min_commission      = 0.0,
    .stamp_duty          = 0.0,
    .trading_days_mask   = 0x7F,   /* 7 days */
    .session_open        = 0.0,
    .session_close       = 24.0,
    .has_lunch_break     = 0,
    .lunch_start         = 0.0,
    .lunch_end           = 0.0,
};

const universe_market_rules_t MARKET_FOREX = {
    .name                = "FOREX",
    .lot_size_default    = 1000.0, /* micro-lot */
    .fractional_allowed  = 0,
    .t_plus              = 0,
    .has_price_limit     = 0,
    .price_limit_up      = 0.0,
    .price_limit_dn      = 0.0,
    .short_allowed       = 1,
    .commission_rate     = 0.0001, /* spread-based; approximate */
    .min_commission      = 0.0,
    .stamp_duty          = 0.0,
    .trading_days_mask   = 0x1F,   /* Mon evening – Fri evening */
    .session_open        = 0.0,
    .session_close       = 24.0,
    .has_lunch_break     = 0,
    .lunch_start         = 0.0,
    .lunch_end           = 0.0,
};

const universe_market_rules_t MARKET_US_FUTURE = {
    .name                = "US_FUTURE",
    .lot_size_default    = 1.0,
    .fractional_allowed  = 0,
    .t_plus              = 0,
    .has_price_limit     = 0,      /* futures use exchange-specific limits; off by default */
    .price_limit_up      = 0.0,
    .price_limit_dn      = 0.0,
    .short_allowed       = 1,
    .commission_rate     = 0.001,
    .min_commission      = 2.0,    /* USD per contract */
    .stamp_duty          = 0.0,
    .trading_days_mask   = 0x1F,
    .session_open        = 0.0,    /* most US futures: near-24h */
    .session_close       = 24.0,
    .has_lunch_break     = 0,
    .lunch_start         = 0.0,
    .lunch_end           = 0.0,
};

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int session_open_check(const universe_market_rules_t *rules, double t) {
    /* t < 0 means "skip check" (backtest mode, no intraday time) */
    if (t < 0.0) return 1;

    /* Check if the bit for this weekday is set in trading_days_mask.
     * Caller must supply day-of-week (0=Mon … 6=Sun) in the high byte
     * of session_time via: session_time = dow * 100.0 + decimal_hours.
     * For simplicity in daily backtest mode pass t = -1. */
    if (t < rules->session_open || t >= rules->session_close) return 0;
    if (rules->has_lunch_break && t >= rules->lunch_start && t < rules->lunch_end) return 0;
    return 1;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

order_error_t market_validate_order(
    const universe_market_rules_t *rules,
    bool    asset_active,
    double  price,
    double  quantity,
    int     direction,
    double  prev_close,
    double  current_pos,
    double  bought_today,
    double  session_time)
{
    if (!rules) return ORDER_OK; // Default to OK if no rules provided
    if (!asset_active)
        return ORDER_ERR_ASSET_INACTIVE;

    if (quantity <= 0.0)
        return ORDER_ERR_ZERO_QTY;

    /* Session hours */
    if (!session_open_check(rules, session_time))
        return ORDER_ERR_MARKET_CLOSED;

    /* Short selling restriction + T+1 checks for sell orders */
    if (direction < 0) {
        /* Would this sell result in a net short position? */
        int goes_short = (quantity > current_pos);
        if (goes_short && !rules->short_allowed)
            return ORDER_ERR_NO_SHORT;

        /* T+1: you cannot sell shares purchased in the same session.
         * Available-to-sell = current_pos - bought_today.
         * If quantity exceeds that, the order violates T+1. */
        if (rules->t_plus >= 1) {
            double available = current_pos - bought_today;
            if (quantity > available + 1e-9)
                return ORDER_ERR_T_PLUS;
        }
    }

    /* Price limits */
    if (rules->has_price_limit && prev_close > 0.0) {
        double upper = prev_close * (1.0 + rules->price_limit_up);
        double lower = prev_close * (1.0 - rules->price_limit_dn);
        if (price > upper || price < lower)
            return ORDER_ERR_PRICE_LIMIT;
    }

    /* Lot size */
    if (!rules->fractional_allowed && rules->lot_size_default > 0.0) {
        double remainder = fmod(quantity, rules->lot_size_default);
        if (remainder > 1e-9)
            return ORDER_ERR_LOT_SIZE;
    }

    return ORDER_OK;
}

double market_round_lot(const universe_market_rules_t *rules, double quantity) {
    if (!rules || rules->fractional_allowed || rules->lot_size_default <= 0.0)
        return quantity;
    double lots = floor(quantity / rules->lot_size_default);
    return lots * rules->lot_size_default;
}

double market_clamp_price(const universe_market_rules_t *rules,
                          double price, double prev_close) {
    if (!rules || !rules->has_price_limit || prev_close <= 0.0)
        return price;
    double upper = prev_close * (1.0 + rules->price_limit_up);
    double lower = prev_close * (1.0 - rules->price_limit_dn);
    if (price > upper) price = upper;
    if (price < lower) price = lower;
    return price;
}

double market_commission(const universe_market_rules_t *rules,
                         double notional, int is_sell) {
    if (!rules) return 0.0;
    double comm = notional * rules->commission_rate;
    if (comm < rules->min_commission)
        comm = rules->min_commission;
    if (is_sell && rules->stamp_duty > 0.0)
        comm += notional * rules->stamp_duty;
    return comm;
}

const char *order_error_str(order_error_t err) {
    switch (err) {
        case ORDER_OK:                return "OK";
        case ORDER_ERR_T_PLUS:        return "T+1 violation: cannot sell same-day purchases";
        case ORDER_ERR_LOT_SIZE:      return "Quantity not a valid lot-size multiple";
        case ORDER_ERR_PRICE_LIMIT:   return "Price outside daily circuit-breaker range";
        case ORDER_ERR_NO_SHORT:      return "Short selling not permitted in this market";
        case ORDER_ERR_MARKET_CLOSED: return "Order submitted outside trading hours";
        case ORDER_ERR_ASSET_INACTIVE:return "Asset is not in the universe on this date";
        case ORDER_ERR_ZERO_QTY:      return "Order quantity must be positive";
        case ORDER_ERR_MARGIN:        return "Insufficient margin";
        default:                      return "Unknown error";
    }
}
