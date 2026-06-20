/**
 * @file provider_csv.c
 * @brief CSV file data provider implementation.
 *
 * Directory layout expected:
 *   <data_dir>/
 *     assets.csv            — asset registry
 *     adjustments.csv       — (optional) price adjustment events
 *     bars/<TICKER>.csv     — OHLCV per asset, preferred naming
 *     bars/<ASSET_ID>.csv   — OHLCV per asset, fallback naming
 *
 * assets.csv format (header required):
 *   id,ticker,exchange,type,start_date,end_date,lot_size,tick_size[,margin_req]
 *   1,AAPL,NYSE,EQUITY,2000-01-03,,1,0.01
 *   2,ENRN,NYSE,EQUITY,2000-01-03,2001-12-02,1,0.01
 *
 * adjustments.csv format:
 *   asset_id,date,type,factor
 *   1,2020-08-31,SPLIT,4.0
 *   1,2024-02-09,DIVIDEND,0.24
 *
 * bars/<TICKER>.csv format:
 *   date,open,high,low,close,volume
 *   2000-01-03,3.35,3.44,3.24,3.35,12300000
 *
 * Dates are accepted as YYYY-MM-DD and converted to epoch days internally.
 */

#include "provider.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

/* =========================================================================
 * Date helpers
 * ========================================================================= */

/** Convert YYYY-MM-DD string to epoch days (days since 1970-01-01). */
static double parse_date(const char *s) {
    if (!s || !*s) return 0.0;

    int y = 0, m = 0, d = 0;
    /* Accept ISO-8601 YYYY-MM-DD */
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) == 3 && y > 0 && m > 0 && d > 0) {
        struct tm t = {0};
        t.tm_year = y - 1900;
        t.tm_mon  = m - 1;
        t.tm_mday = d;
        t.tm_isdst = -1;
        time_t epoch = mktime(&t);
        if (epoch == (time_t)-1) return 0.0;
        return (double)(epoch / 86400);
    }
    /* Accept plain numeric (already epoch days) */
    return atof(s);
}

/** Map type string to UNIVERSE_ASSET_* constant. */
static uint8_t parse_asset_type(const char *s) {
    if (!s) return UNIVERSE_ASSET_EQUITY;
    if (strncmp(s, "EQUITY", 6) == 0) return UNIVERSE_ASSET_EQUITY;
    if (strncmp(s, "ETF",    3) == 0) return UNIVERSE_ASSET_ETF;
    if (strncmp(s, "FUTURE", 6) == 0) return UNIVERSE_ASSET_FUTURE;
    if (strncmp(s, "CRYPTO", 6) == 0) return UNIVERSE_ASSET_CRYPTO;
    if (strncmp(s, "FX",     2) == 0) return UNIVERSE_ASSET_FX;
    if (strncmp(s, "BOND",   4) == 0) return UNIVERSE_ASSET_BOND;
    return UNIVERSE_ASSET_EQUITY;
}

/** Map adjustment type string to UNIVERSE_ADJ_* constant. */
static uint8_t parse_adj_type(const char *s) {
    if (!s) return UNIVERSE_ADJ_SPLIT;
    if (strncmp(s, "SPLIT",     5) == 0) return UNIVERSE_ADJ_SPLIT;
    if (strncmp(s, "DIVIDEND",  8) == 0) return UNIVERSE_ADJ_DIVIDEND;
    if (strncmp(s, "DELIST",    6) == 0) return UNIVERSE_ADJ_DELIST;
    if (strncmp(s, "NAME",      4) == 0) return UNIVERSE_ADJ_NAME;
    return UNIVERSE_ADJ_SPLIT;
}

/* =========================================================================
 * Minimal CSV line parser
 * Returns pointer past the parsed field; advances *out_end past delimiter.
 * ========================================================================= */

static const char *csv_next_field(const char *p, char *buf, size_t buflen) {
    if (!p || !*p) { if (buf && buflen > 0) buf[0] = '\0'; return NULL; }
    size_t i = 0;
    while (*p && *p != ',' && *p != '\n' && *p != '\r') {
        if (buf && i + 1 < buflen) buf[i++] = *p;
        p++;
    }
    if (buf) buf[i] = '\0';
    if (*p == ',') p++;  /* skip delimiter */
    return p;
}

static void strip_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

/* =========================================================================
 * CSV provider state
 * ========================================================================= */

typedef struct {
    char data_dir[512];
} csv_state_t;

/* ─── CSV bar stream ─────────────────────────────────────────────────── */
typedef struct {
    FILE   *file;
    char    ticker[16];
    uint32_t asset_id;
    double   start_date;
    double   end_date;
    bool     header_skipped;
} csv_bar_stream_t;

/* =========================================================================
 * load_assets
 * ========================================================================= */

static int csv_load_assets(provider_t *self, universe_t *u,
                            const char *source, mem_pool_t *arena) {
    (void)arena;
    csv_state_t *st = (csv_state_t *)self->user_data;
    char path[640];

    if (source && source[0] == '/' || (source && source[1] == ':')) {
        /* Absolute path provided */
        snprintf(path, sizeof(path), "%s", source);
    } else {
        snprintf(path, sizeof(path), "%s/%s", st->data_dir,
                 source ? source : "assets.csv");
    }

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[1024];
    bool first = true;

    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);
        if (first) { first = false; continue; } /* skip header */
        if (line[0] == '#' || line[0] == '\0') continue;

        const char *p = line;
        char fid[32], fticker[16], fexchange[8], ftype[16];
        char fstart[16], fend[16], flot[32], ftick[32], fmargin[32];

        p = csv_next_field(p, fid,       sizeof(fid));
        p = csv_next_field(p, fticker,   sizeof(fticker));
        p = csv_next_field(p, fexchange, sizeof(fexchange));
        p = csv_next_field(p, ftype,     sizeof(ftype));
        p = csv_next_field(p, fstart,    sizeof(fstart));
        p = csv_next_field(p, fend,      sizeof(fend));
        p = csv_next_field(p, flot,      sizeof(flot));
        p = csv_next_field(p, ftick,     sizeof(ftick));
        p = csv_next_field(p, fmargin,   sizeof(fmargin));

        if (!fid[0] || !fticker[0]) continue;

        universe_asset_t a = {0};
        a.id         = (uint32_t)atoi(fid);
        a.asset_type = parse_asset_type(ftype);
        a.start_date = parse_date(fstart);
        a.end_date   = parse_date(fend);
        a.lot_size   = flot[0]    ? atof(flot)    : self->market->lot_size_default;
        a.tick_size  = ftick[0]   ? atof(ftick)   : 0.01;  /* tick size is per-asset; default 0.01 */
        a.margin_req = fmargin[0] ? atof(fmargin) : 0.0;
        strncpy(a.ticker,   fticker,   sizeof(a.ticker)   - 1);
        strncpy(a.exchange, fexchange, sizeof(a.exchange) - 1);

        universe_add_asset(u, &a);
    }

    fclose(f);
    return 0;
}

/* =========================================================================
 * load_adjustments
 * ========================================================================= */

static int csv_load_adjustments(provider_t *self, universe_t *u,
                                 const char *source, mem_pool_t *arena) {
    (void)arena;
    csv_state_t *st = (csv_state_t *)self->user_data;
    char path[640];
    bool explicit_source = source && source[0];

    if (source && (source[0] == '/' || source[1] == ':'))
        snprintf(path, sizeof(path), "%s", source);
    else
        snprintf(path, sizeof(path), "%s/%s", st->data_dir,
                 source ? source : "adjustments.csv");

    FILE *f = fopen(path, "r");
    if (!f) return explicit_source ? -1 : 0;  /* default file is optional */

    char line[512];
    bool first = true;

    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);
        if (first) { first = false; continue; }
        if (line[0] == '#' || line[0] == '\0') continue;

        const char *p = line;
        char fid[32], fdate[16], ftype[16], ffactor[32];

        p = csv_next_field(p, fid,     sizeof(fid));
        p = csv_next_field(p, fdate,   sizeof(fdate));
        p = csv_next_field(p, ftype,   sizeof(ftype));
        p = csv_next_field(p, ffactor, sizeof(ffactor));

        if (!fid[0]) continue;

        universe_adj_t adj = {0};
        adj.asset_id = (uint32_t)atoi(fid);
        adj.date     = parse_date(fdate);
        adj.type     = parse_adj_type(ftype);
        adj.factor   = atof(ffactor);

        universe_add_adjustment(u, &adj);
    }

    fclose(f);
    return 0;
}

/* =========================================================================
 * Streaming: single-asset bar stream
 * ========================================================================= */

static void *csv_open_stream(provider_t *self, uint32_t asset_id,
                              double start_date, double end_date,
                              mem_pool_t *arena) {
    (void)arena;
    csv_state_t *st = (csv_state_t *)self->user_data;

    /* Look up ticker from universe (we need to build the filename) */
    /* The universe must already be loaded before streaming begins. */

/* Fallback path for providers that only have bars/<asset_id>.csv.
 * Higher-level helpers prefer ticker-based filenames when the universe knows them. */
    char path[640];
    snprintf(path, sizeof(path), "%s/bars/%u.csv", st->data_dir, asset_id);

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    csv_bar_stream_t *s = (csv_bar_stream_t *)malloc(sizeof(csv_bar_stream_t));
    if (!s) { fclose(f); return NULL; }
    memset(s, 0, sizeof(*s));
    s->file       = f;
    s->asset_id   = asset_id;
    s->start_date = start_date;
    s->end_date   = end_date;
    return s;
}

/** Open stream by ticker name (preferred). */
static void *csv_open_stream_by_ticker(provider_t *self, const char *ticker,
                                        uint32_t asset_id,
                                        double start_date, double end_date,
                                        mem_pool_t *arena) {
    (void)arena;
    csv_state_t *st = (csv_state_t *)self->user_data;
    char path[640];
    snprintf(path, sizeof(path), "%s/bars/%s.csv", st->data_dir, ticker);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Fallback: numeric ID-based filename */
        snprintf(path, sizeof(path), "%s/bars/%u.csv", st->data_dir, asset_id);
        f = fopen(path, "r");
        if (!f) return NULL;
    }

    csv_bar_stream_t *s = (csv_bar_stream_t *)malloc(sizeof(csv_bar_stream_t));
    if (!s) { fclose(f); return NULL; }
    memset(s, 0, sizeof(*s));
    s->file       = f;
    s->asset_id   = asset_id;
    s->start_date = start_date;
    s->end_date   = end_date;
    strncpy(s->ticker, ticker, sizeof(s->ticker) - 1);
    return s;
}

static int csv_next_bar(provider_t *self, void *stream, provider_bar_t *out) {
    (void)self;
    csv_bar_stream_t *s = (csv_bar_stream_t *)stream;
    if (!s || !s->file) return -1;

    char line[256];
    while (fgets(line, sizeof(line), s->file)) {
        strip_newline(line);

        /* Skip header row (first line) */
        if (!s->header_skipped) {
            s->header_skipped = true;
            continue;
        }
        if (line[0] == '#' || line[0] == '\0') continue;

        const char *p = line;
        char fdate[16], fopen[32], fhigh[32], flow[32], fclose[32], fvol[32];

        p = csv_next_field(p, fdate,  sizeof(fdate));
        p = csv_next_field(p, fopen,  sizeof(fopen));
        p = csv_next_field(p, fhigh,  sizeof(fhigh));
        p = csv_next_field(p, flow,   sizeof(flow));
        p = csv_next_field(p, fclose, sizeof(fclose));
        p = csv_next_field(p, fvol,   sizeof(fvol));

        if (!fdate[0] || !fclose[0]) continue;

        double date = parse_date(fdate);
        if (s->start_date > 0.0 && date < s->start_date) continue;
        if (s->end_date   > 0.0 && date > s->end_date)   return 0; /* past range */

        out->date     = date;
        out->open     = atof(fopen);
        out->high     = atof(fhigh);
        out->low      = atof(flow);
        out->close    = atof(fclose);
        out->volume   = atof(fvol);
        out->asset_id = s->asset_id;
        return 1;
    }
    return 0; /* EOF */
}

static void csv_close_stream(provider_t *self, void *stream) {
    (void)self;
    csv_bar_stream_t *s = (csv_bar_stream_t *)stream;
    if (s) {
        if (s->file) fclose(s->file);
        free(s);
    }
}

/* =========================================================================
 * Provider factory
 * ========================================================================= */

provider_t *provider_csv_create(const universe_market_rules_t *market,
                                 const char *data_dir,
                                 mem_pool_t *arena) {
    provider_t *p = (provider_t *)mem_alloc(arena, sizeof(provider_t));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));

    csv_state_t *st = (csv_state_t *)mem_alloc(arena, sizeof(csv_state_t));
    if (!st) return NULL;
    strncpy(st->data_dir, data_dir ? data_dir : ".", sizeof(st->data_dir) - 1);

    p->name              = "csv";
    p->market            = market;
    p->load_assets       = csv_load_assets;
    p->load_adjustments  = csv_load_adjustments;
    p->open_stream       = csv_open_stream;
    p->next_bar          = csv_next_bar;
    p->close_stream      = csv_close_stream;
    p->open_multi_stream = NULL;  /* Phase 3 extension */
    p->next_date         = NULL;
    p->close_multi_stream= NULL;
    p->user_data         = st;

    return p;
}

void provider_free(provider_t *p) {
    /* user_data is arena-allocated; nothing extra to free */
    (void)p;
}

/* =========================================================================
 * Convenience wrappers
 * ========================================================================= */

int provider_load_universe(provider_t *p, universe_t *u,
                            const char *assets_source,
                            const char *adj_source,
                            mem_pool_t *arena) {
    if (!p || !u) return -1;

    int rc = p->load_assets(p, u, assets_source, arena);
    if (rc < 0) return rc;

    if (adj_source && p->load_adjustments) {
        rc = p->load_adjustments(p, u, adj_source, arena);
        if (rc < 0) return rc;
    }

    universe_finalize(u);
    return 0;
}

int provider_load_window(provider_t *p, universe_t *u,
                          uint32_t asset_id,
                          double start_date, double end_date,
                          bar_window_t *window) {
    if (!p || !window || !p->open_stream) return -1;

    /* Try ticker-based open if we know the ticker */
    const universe_asset_t *a = NULL;
    if (u) {
        size_t idx = universe_find_asset(u, asset_id);
        if (idx != SIZE_MAX) a = &u->assets[idx];
    }

    void *stream;
    if (a && a->ticker[0])
        stream = csv_open_stream_by_ticker(p, a->ticker, asset_id,
                                           start_date, end_date, NULL);
    else
        stream = p->open_stream(p, asset_id, start_date, end_date, NULL);

    if (!stream) return -1;

    provider_bar_t bar;
    provider_bar_t prev_bar = {0};
    int count = 0;

    while (p->next_bar(p, stream, &bar) == 1) {
        double prev_adj = universe_adj_factor(u, asset_id);
        if (u) {
            universe_advance(u, bar.date);
            universe_apply_runtime_adjustments(u, asset_id, bar.date, prev_bar.close);
        }
        double adj = universe_adj_factor(u, asset_id);
        if (window && window->count > 0 && prev_adj > 0.0 && adj > 0.0 && prev_adj != adj)
            bar_window_rescale_prices(window, adj / prev_adj);
        bar_window_push_adjusted(window, bar.date,
                                  bar.open, bar.high, bar.low, bar.close, bar.volume,
                                  adj);
        prev_bar = bar;
        count++;
    }
    p->close_stream(p, stream);
    return count;
}
