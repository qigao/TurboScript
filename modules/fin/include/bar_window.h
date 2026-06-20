/**
 * @file bar_window.h
 * @brief Fixed-capacity ring buffer for OHLCV price bars.
 *
 * The bar window maintains a rolling window of the most recent N bars for a
 * single asset.  On each new bar:
 *   1. The oldest bar is evicted (if the buffer is full).
 *   2. The new bar is appended (optionally with price adjustment).
 *   3. The linearized view is updated for binding into TurboScript vectors.
 *
 * All memory lives inside a caller-supplied mem_pool_t.
 */
#ifndef BAR_WINDOW_H
#define BAR_WINDOW_H

#include <stddef.h>
#include <stdbool.h>
#include "exprtk_types.h"   /* mem_pool_t (via exprtk_types -> turbo_buff) */

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Data Structure
 * ========================================================================= */

/**
 * @brief Ring buffer for OHLCV bars.
 *
 * The ring stores raw bars internally; a parallel set of "linear" arrays
 * holds the same data in chronological order (oldest → newest) ready for
 * binding using ts_bind_vec().  Linearization is done lazily on demand via
 * bar_window_linearize().
 */
typedef struct {
    /* Ring storage (circular, length == capacity) */
    double *r_open;
    double *r_high;
    double *r_low;
    double *r_close;
    double *r_volume;
    double *r_date;         /**< Optional: epoch days, 0 if not provided */

    size_t  capacity;       /**< Maximum bars retained                   */
    size_t  count;          /**< Current number of valid bars (≤ capacity)*/
    size_t  write_pos;      /**< Next write index in the ring            */

    /* Linearized view (chronological order, length == count) */
    double *open;
    double *high;
    double *low;
    double *close;
    double *volume;
    double *date;

    bool    dirty;          /**< True if ring was modified since last linearize */
} bar_window_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Allocate a new bar window.
 * @param capacity  Maximum number of bars to retain (e.g., 500).
 * @param arena     All allocations are made from this arena.
 * @return Pointer to window, or NULL on OOM.
 */
bar_window_t *bar_window_create(size_t capacity, mem_pool_t *arena);

/**
 * @brief Reset the window to empty (does not free memory).
 */
void bar_window_clear(bar_window_t *w);

/* =========================================================================
 * Pushing Bars
 * ========================================================================= */

/**
 * @brief Push a raw bar into the window.
 * @param date  Epoch days (pass 0 if unavailable).
 */
void bar_window_push(bar_window_t *w,
                     double date,
                     double open, double high, double low,
                     double close, double volume);

/**
 * @brief Push an adjusted bar: raw prices are multiplied by adj_factor before
 *        storage.  Use this when the universe applies split/dividend factors.
 */
void bar_window_push_adjusted(bar_window_t *w,
                               double date,
                               double open, double high, double low,
                               double close, double volume,
                               double adj_factor);

/**
 * @brief Rescale all stored prices in the window by a constant ratio.
 *
 * This is used when the universe adjustment factor changes after earlier bars
 * have already been pushed, so the whole rolling window stays on one basis.
 * Volume and dates are left unchanged.
 */
void bar_window_rescale_prices(bar_window_t *w, double scale);

/* =========================================================================
 * Accessing Data
 * ========================================================================= */

/**
 * @brief Ensure the linear arrays are up-to-date.
 *
 * After any push, the linear arrays (open, high, low, close, volume, date)
 * are refreshed so they can be passed directly to ts_bind_vec().  This copies
 * the ring into chronological order; O(count).
 *
 * You only need to call this once per bar, just before binding into the
 * TurboScript context.  Subsequent reads from w->open etc. are valid until
 * the next push.
 */
void bar_window_linearize(bar_window_t *w);

/**
 * @brief Return the most recent (last) close price.
 * @return 0.0 if the window is empty.
 */
double bar_window_last_close(const bar_window_t *w);

/**
 * @brief Return the most recent bar's date.
 * @return 0.0 if the window is empty or dates were not provided.
 */
double bar_window_last_date(const bar_window_t *w);

/**
 * @brief Return true when the window has at least min_bars bars.
 *        Useful for checking warm-up conditions before running indicators.
 */
bool bar_window_ready(const bar_window_t *w, size_t min_bars);

#ifdef __cplusplus
}
#endif

#endif /* BAR_WINDOW_H */
