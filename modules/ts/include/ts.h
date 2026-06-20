/**
 * @file fin.h
 * @brief Finance, TA, and Timeseries module public API.
 *
 * Provides per-env module descriptors (ta, finance, timeseries) and
 * the underlying C-level indicator / backtest / risk functions.
 */
#ifndef FIN_H
#define FIN_H

#include "exprtk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Static module accessors — mount via exprtk_env_add_module() */
const exprtk_module_t *exprtk_module_timeseries(void);

/* Timeseries */
size_t exprtk_ts_diff(const double *data, size_t n, size_t order, double *out,
                      mem_pool_t *arena);
size_t exprtk_ts_autocorr(const double *data, size_t n, size_t max_lag, double *out);
size_t exprtk_ts_pacf(const double *data, size_t n, size_t max_lag, double *out,
                      mem_pool_t *arena);
int exprtk_ts_adf(const double *data, size_t n, size_t p, double *out, mem_pool_t *arena);
size_t exprtk_ts_garch(const double *returns, size_t n, double alpha, double beta, double *out);
size_t exprtk_ts_match(const double *data, const double *pattern, size_t n, size_t m, double *out);
size_t exprtk_ts_match_cosine(const double *data, const double *pattern, size_t n, size_t m,
                              double *out);
size_t exprtk_ts_match_normalized(const double *data, const double *pattern, size_t n, size_t m,
                                  double *out, mem_pool_t *arena);
size_t exprtk_ts_match_correl(const double *data, const double *pattern, size_t n, size_t m,
                              double *out, mem_pool_t *arena);
size_t exprtk_ts_match_returns(const double *data, const double *pattern, size_t n, size_t m,
                               double *out, mem_pool_t *arena);
size_t exprtk_ts_match_candle(const double *O, const double *H, const double *L, const double *C,
                              const double *pO, const double *pH, const double *pL,
                              const double *pC, size_t n, size_t m, double *out,
                              mem_pool_t *arena);
size_t exprtk_ts_match_dtw(const double *data, const double *pattern, size_t n, size_t m,
                           double *out, mem_pool_t *arena);
size_t exprtk_ts_dwt(const double *data, size_t n, size_t levels, double *approx, double *detail,
                     mem_pool_t *arena);
size_t exprtk_ts_emd(const double *data, size_t n, size_t max_imfs, double *imfs,
                     mem_pool_t *arena);
double exprtk_ts_hurst(const double *data, size_t n, double *out, mem_pool_t *arena);

/* Cointegration & Spread */
size_t exprtk_ts_coint(const double *x, const double *y, size_t n, double *out,
                       mem_pool_t *arena);
size_t exprtk_ts_spread(const double *x, const double *y, size_t n, double *out,
                        mem_pool_t *arena);

/* Rolling / Expanding Window Functions */
size_t exprtk_ts_rolling_mean(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ts_rolling_std(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ts_rolling_skew(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ts_rolling_kurt(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ts_rolling_corr(const double *a, const double *b, size_t n, size_t period,
                              double *out);
size_t exprtk_ts_rolling_beta(const double *y, const double *x, size_t n, size_t period,
                              double *out);
size_t exprtk_ts_expanding_mean(const double *in, size_t n, double *out);
size_t exprtk_ts_expanding_std(const double *in, size_t n, double *out);
size_t exprtk_ts_ewm_mean(const double *in, size_t n, size_t span, double *out);
size_t exprtk_ts_ewm_std(const double *in, size_t n, size_t span, double *out);
size_t exprtk_ts_pct_change(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ts_log_return(const double *in, size_t n, double *out);
size_t exprtk_ts_cum_return(const double *in, size_t n, double *out);

#ifdef __cplusplus
}
#endif

#endif /* FIN_H */
