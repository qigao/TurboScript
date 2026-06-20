/**
 * @file bar_window.c
 * @brief Fixed-capacity OHLCV ring buffer implementation.
 */

#include "bar_window.h"
#include "exprtk_types.h"   /* mem_alloc */
#include <string.h>
#include <stdlib.h>
#include <assert.h>

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

bar_window_t *bar_window_create(size_t capacity, mem_pool_t *arena) {
    if (capacity == 0) return NULL;

    bar_window_t *w = (bar_window_t *)mem_alloc(arena, sizeof(bar_window_t));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));

    w->capacity = capacity;

    /* Ring storage — 6 arrays × capacity */
#define ALLOC_RING(field) \
    w->field = (double *)mem_alloc(arena, capacity * sizeof(double)); \
    if (!w->field) return NULL; \
    memset(w->field, 0, capacity * sizeof(double))

    ALLOC_RING(r_open);
    ALLOC_RING(r_high);
    ALLOC_RING(r_low);
    ALLOC_RING(r_close);
    ALLOC_RING(r_volume);
    ALLOC_RING(r_date);

    /* Linearized view — same size, allocated separately */
#define ALLOC_LIN(field, ring_field) \
    w->field = (double *)mem_alloc(arena, capacity * sizeof(double)); \
    if (!w->field) return NULL; \
    memset(w->field, 0, capacity * sizeof(double))

    ALLOC_LIN(open,   r_open);
    ALLOC_LIN(high,   r_high);
    ALLOC_LIN(low,    r_low);
    ALLOC_LIN(close,  r_close);
    ALLOC_LIN(volume, r_volume);
    ALLOC_LIN(date,   r_date);

#undef ALLOC_RING
#undef ALLOC_LIN

    w->count     = 0;
    w->write_pos = 0;
    w->dirty     = false;
    return w;
}

void bar_window_clear(bar_window_t *w) {
    if (!w) return;
    w->count     = 0;
    w->write_pos = 0;
    w->dirty     = false;
    memset(w->r_open,   0, w->capacity * sizeof(double));
    memset(w->r_high,   0, w->capacity * sizeof(double));
    memset(w->r_low,    0, w->capacity * sizeof(double));
    memset(w->r_close,  0, w->capacity * sizeof(double));
    memset(w->r_volume, 0, w->capacity * sizeof(double));
    memset(w->r_date,   0, w->capacity * sizeof(double));
}

/* =========================================================================
 * Internal: write one bar at write_pos and advance the ring
 * ========================================================================= */

static void ring_write(bar_window_t *w,
                       double date,
                       double o, double h, double l, double c, double v) {
    size_t pos = w->write_pos % w->capacity;
    w->r_open[pos]   = o;
    w->r_high[pos]   = h;
    w->r_low[pos]    = l;
    w->r_close[pos]  = c;
    w->r_volume[pos] = v;
    w->r_date[pos]   = date;

    w->write_pos = (w->write_pos + 1) % w->capacity;
    if (w->count < w->capacity) w->count++;
    w->dirty = true;
}

/* =========================================================================
 * Pushing Bars
 * ========================================================================= */

void bar_window_push(bar_window_t *w,
                     double date,
                     double open, double high, double low,
                     double close, double volume) {
    if (!w) return;
    ring_write(w, date, open, high, low, close, volume);
}

void bar_window_push_adjusted(bar_window_t *w,
                               double date,
                               double open, double high, double low,
                               double close, double volume,
                               double adj_factor) {
    if (!w) return;
    if (adj_factor == 0.0) adj_factor = 1.0;
    ring_write(w, date,
               open   * adj_factor,
               high   * adj_factor,
               low    * adj_factor,
               close  * adj_factor,
               volume);  /* volume is not price-adjusted */
}

void bar_window_rescale_prices(bar_window_t *w, double scale) {
    if (!w || w->count == 0 || scale == 1.0 || scale == 0.0) return;

    for (size_t i = 0; i < w->capacity; i++) {
        w->r_open[i] *= scale;
        w->r_high[i] *= scale;
        w->r_low[i] *= scale;
        w->r_close[i] *= scale;
    }

    for (size_t i = 0; i < w->count; i++) {
        w->open[i] *= scale;
        w->high[i] *= scale;
        w->low[i] *= scale;
        w->close[i] *= scale;
    }

    w->dirty = true;
}

/* =========================================================================
 * Accessing Data
 * ========================================================================= */

void bar_window_linearize(bar_window_t *w) {
    if (!w || !w->dirty || w->count == 0) return;

    size_t n    = w->count;
    size_t cap  = w->capacity;

    /* oldest bar is at:
     *   if count < capacity  → index 0 (ring hasn't wrapped yet)
     *   else                 → write_pos (the slot we're about to overwrite next)
     */
    size_t start = (n < cap) ? 0 : w->write_pos;

    for (size_t i = 0; i < n; i++) {
        size_t src = (start + i) % cap;
        w->open[i]   = w->r_open[src];
        w->high[i]   = w->r_high[src];
        w->low[i]    = w->r_low[src];
        w->close[i]  = w->r_close[src];
        w->volume[i] = w->r_volume[src];
        w->date[i]   = w->r_date[src];
    }

    w->dirty = false;
}

double bar_window_last_close(const bar_window_t *w) {
    if (!w || w->count == 0) return 0.0;
    /* Most recently written slot is (write_pos - 1 + capacity) % capacity */
    size_t last = (w->write_pos + w->capacity - 1) % w->capacity;
    return w->r_close[last];
}

double bar_window_last_date(const bar_window_t *w) {
    if (!w || w->count == 0) return 0.0;
    size_t last = (w->write_pos + w->capacity - 1) % w->capacity;
    return w->r_date[last];
}

bool bar_window_ready(const bar_window_t *w, size_t min_bars) {
    return w && w->count >= min_bars;
}
