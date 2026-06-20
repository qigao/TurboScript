/**
 * @file provider.h
 * @brief Pluggable data provider interface for the universe system.
 *
 * A provider bridges a specific data source (CSV files, HTTP/REST APIs,
 * WebSocket streams) and the normalized universe + bar-window format
 * expected by the strategy engine.
 *
 * Each market / data vendor gets its own provider implementation.
 * The strategy layer only ever calls through this interface, so swapping
 * a backtest CSV provider for a live Binance WebSocket provider requires
 * only changing the provider pointer — no strategy code changes.
 */
#ifndef PROVIDER_H
#define PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include "universe.h"       /* universe_t, universe_asset_t */
#include "market_rules.h"   /* universe_market_rules_t      */
#include "bar_window.h"     /* bar_window_t                 */
#include "exprtk_types.h"   /* mem_pool_t                */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * OHLCV Bar (normalized format all providers output)
 * ========================================================================= */

typedef struct {
    double date;        /**< Epoch days (e.g., 19754.0 = 2024-01-05)         */
    double open;
    double high;
    double low;
    double close;
    double volume;
    uint32_t asset_id;  /**< Populated by multi-asset providers              */
} provider_bar_t;

/* =========================================================================
 * Provider Interface (vtable)
 * ========================================================================= */

typedef struct provider_s provider_t;

struct provider_s {
    const char *name;                       /**< e.g., "csv", "tushare", "binance" */
    const universe_market_rules_t *market;  /**< Associated market rules            */

    /* ── Setup ─────────────────────────────────────────────────────────── */

    /**
     * Load asset metadata into the universe.
     * @param source  Datasource descriptor: file path, API endpoint, index name, etc.
     * @return 0 = OK, <0 = error.
     */
    int (*load_assets)(provider_t *self,
                       universe_t *u,
                       const char *source,
                       mem_pool_t *arena);

    /**
     * Load price-adjustment events (for example splits and cash dividends)
     * into the universe. May be NULL if the data source doesn't supply them.
     */
    int (*load_adjustments)(provider_t *self,
                            universe_t *u,
                            const char *source,
                            mem_pool_t *arena);

    /* ── Bar Streaming ──────────────────────────────────────────────────── */

    /**
     * Open a bar stream for a given asset and date range.
     * Returns an opaque stream handle, or NULL on failure.
     * The handle is passed to next_bar() and close_stream().
     */
    void *(*open_stream)(provider_t *self,
                         uint32_t asset_id,
                         double start_date,
                         double end_date,
                         mem_pool_t *arena);

    /**
     * Fetch the next bar from the stream.
     * @param stream  Handle returned by open_stream().
     * @param out     Output bar.
     * @return 1 = bar written to out, 0 = end of stream, <0 = error.
     */
    int (*next_bar)(provider_t *self,
                    void *stream,
                    provider_bar_t *out);

    /**
     * Close and free a stream handle.
     */
    void (*close_stream)(provider_t *self, void *stream);

    /* ── Optional: Multi-Asset Aligned Streaming ────────────────────────── */

    /**
     * Open a cross-sectional stream: all active assets on the same date axis.
     * Each call to next_date() returns one trading date with bars for all assets.
     * May be NULL if the provider doesn't support aligned multi-asset data.
     */
    void *(*open_multi_stream)(provider_t *self,
                               universe_t *u,
                               double start_date,
                               double end_date,
                               mem_pool_t *arena);

    /**
     * Fetch one date slice from a multi-asset stream.
     * @param stream   Handle from open_multi_stream().
     * @param out_bars Array of provider_bar_t, one per asset (in asset ID order).
     * @param out_date Current date being delivered.
     * @return number of bars written, 0 = end, <0 = error.
     */
    int (*next_date)(provider_t *self,
                     void *stream,
                     provider_bar_t *out_bars,
                     double *out_date);

    void (*close_multi_stream)(provider_t *self, void *stream);

    /* ── Provider-specific context ──────────────────────────────────────── */
    void *user_data;
};

/* =========================================================================
 * Built-in Provider Factories
 * ========================================================================= */

/**
 * @brief Create a CSV file provider.
 *
 * Expected directory layout:
 *   <data_dir>/
 *     assets.csv            — asset metadata
 *     adjustments.csv       — (optional) split/dividend events
 *     bars/<ticker>.csv     — OHLCV bars, preferred naming
 *     bars/<asset_id>.csv   — OHLCV bars, supported fallback naming
 *
 * assets.csv columns:     id,ticker,exchange,type,start_date,end_date,lot_size,tick_size
 * adjustments.csv columns: asset_id,date,type,factor
 * bars/<X>.csv columns:   date,open,high,low,close,volume
 *   (dates as YYYY-MM-DD strings, converted to epoch days internally)
 *
 * @param market   Market rules to attach.
 * @param data_dir Root directory (absolute path).
 * @param arena    Lifetime arena.
 * @return Provider pointer, or NULL on OOM.
 */
provider_t *provider_csv_create(const universe_market_rules_t *market,
                                 const char *data_dir,
                                 mem_pool_t *arena);

/**
 * @brief Free a provider created by provider_csv_create().
 */
void provider_free(provider_t *p);

/* =========================================================================
 * Convenience wrappers
 * ========================================================================= */

/**
 * @brief Load asset metadata + adjustments using a provider.
 * Calls load_assets then load_adjustments (if available), then universe_finalize.
 */
int provider_load_universe(provider_t *p, universe_t *u,
                            const char *assets_source,
                            const char *adj_source,     /* NULL to skip */
                            mem_pool_t *arena);

/**
 * @brief Stream all bars for a single asset into a bar_window.
 * Useful for pre-warming indicator look-back periods.
 * @return Number of bars loaded, or <0 on error.
 */
int provider_load_window(provider_t *p, universe_t *u,
                          uint32_t asset_id,
                          double start_date, double end_date,
                          bar_window_t *window);

#ifdef __cplusplus
}
#endif

#endif /* PROVIDER_H */
