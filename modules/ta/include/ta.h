/**
 * @file ta.h
 * @brief TA module public + shared internal declarations.
 */
#ifndef TA_H
#define TA_H

#include "exprtk.h"

#ifdef __cplusplus
extern "C" {
#endif
 
const exprtk_module_t *exprtk_module_ta(void); 

/* ========================================================================= */
/* TA-Lib Overlap                                                            */
/* ========================================================================= */
size_t exprtk_ta_sma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_ema(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_wma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_dema(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_tema(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_kama(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_t3(const double *in, size_t n, size_t period, double vfactor, double *out,
                    mem_pool_t *arena);
size_t exprtk_ta_trima(const double *in, size_t n, size_t period, double *out,
                       mem_pool_t *arena);
size_t exprtk_ta_bbands(const double *in, size_t n, size_t period, double mult, double *upper,
                        double *middle, double *lower);
size_t exprtk_ta_midpoint(const double *in, size_t n, size_t period, double *out,
                          mem_pool_t *arena);
size_t exprtk_ta_midprice(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          mem_pool_t *arena);
size_t exprtk_ta_sar(const double *hi, const double *lo, size_t n, double accel_init,
                     double accel_max, double *out);
size_t exprtk_ta_savgol(const double *in, size_t n, size_t window, double *out);
size_t exprtk_ta_bbi(const double *in, size_t n, double *out, mem_pool_t *arena);
size_t exprtk_ta_hma(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_supertrend(const double *hi, const double *lo, const double *cl, size_t n,
                            size_t period, double mult, double *trend, double *upper, double *lower,
                            mem_pool_t *arena);

size_t exprtk_ta_vwap(const double *hi, const double *lo, const double *cl, const double *vol,
                      size_t n, double *out);
size_t exprtk_ta_vwap_session(const double *hi, const double *lo, const double *cl,
                              const double *vol, const double *session, size_t n, double *out);
size_t exprtk_ta_donchian(const double *hi, const double *lo, size_t n, size_t period,
                          double *upper, double *lower, double *middle, mem_pool_t *arena);
size_t exprtk_ta_keltner(const double *hi, const double *lo, const double *cl, size_t n,
                         size_t ema_p, size_t atr_p, double mult, double *upper, double *middle,
                         double *lower, mem_pool_t *arena);
size_t exprtk_ta_ichimoku(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t tenkan_p, size_t kijun_p, size_t senkou_p, double *tenkan,
                          double *kijun, double *senkou_a, double *senkou_b, double *chikou,
                          mem_pool_t *arena);

size_t exprtk_ta_pivot_high(const double *hi, size_t n, size_t left, size_t right, double *out);
size_t exprtk_ta_pivot_low(const double *lo, size_t n, size_t left, size_t right, double *out);
size_t exprtk_ta_rma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_zlema(const double *in, size_t n, size_t period, double *out,
                       mem_pool_t *arena);
size_t exprtk_ta_alma(const double *in, size_t n, size_t period, double offset, double sigma,
                      double *out, mem_pool_t *arena);
size_t exprtk_ta_vidya(const double *in, size_t n, size_t cmo_p, size_t ema_p, double *out,
                       mem_pool_t *arena);
size_t exprtk_ta_rvi(const double *in, size_t n, size_t std_p, size_t ema_p, double *out,
                     mem_pool_t *arena);
size_t exprtk_ta_vhf(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_volatility_ratio(const double *hi, const double *lo, const double *cl, size_t n,
                                  size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_er(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_bias(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_psy(const double *cl, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_pressure(const double *hi, const double *lo, const double *cl, const double *vol,
                          size_t n, double *buy_pres, double *sell_pres);
size_t exprtk_ta_kvo(const double *hi, const double *lo, const double *cl, const double *vol,
                     size_t n, size_t fast_p, size_t slow_p, size_t sig_p, double *kvo, double *sig,
                     mem_pool_t *arena);
size_t exprtk_ta_arbr(const double *hi, const double *lo, const double *op, const double *cl,
                      size_t n, size_t period, double *ar, double *br);
size_t exprtk_candle_body_size(const double *O, const double *C, const double *H, const double *L,
                               size_t n, double *out);
size_t exprtk_candle_wick_upper(const double *O, const double *C, const double *H, size_t n,
                                double *out);
size_t exprtk_candle_wick_lower(const double *O, const double *C, const double *L, size_t n,
                                double *out);
size_t exprtk_candle_direction(const double *O, const double *C, size_t n, double *out);
size_t exprtk_candle_fuzzy_bull(const double *O, const double *H, const double *L, const double *C,
                                size_t n, double *out);
size_t exprtk_candle_fuzzy_bear(const double *O, const double *H, const double *L, const double *C,
                                size_t n, double *out);

/* TA-Lib Momentum */
size_t exprtk_ta_rsi(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_macd(const double *in, size_t n, size_t fast_p, size_t slow_p, size_t sig_p,
                      double *line, double *sig, double *hist, mem_pool_t *arena);
size_t exprtk_ta_stoch(const double *hi, const double *lo, const double *cl, size_t n, size_t k_p,
                       size_t d_p, double *out_k, double *out_d, mem_pool_t *arena);
size_t exprtk_ta_stochrsi(const double *in, size_t n, size_t rsi_p, size_t k_p, size_t d_p,
                          double *out_k, double *out_d, mem_pool_t *arena);
size_t exprtk_ta_willr(const double *hi, const double *lo, const double *cl, size_t n,
                       size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_cci(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, mem_pool_t *arena);
size_t exprtk_ta_mom(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_roc(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_apo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out,
                     mem_pool_t *arena);
size_t exprtk_ta_ppo(const double *in, size_t n, size_t fast_p, size_t slow_p, double *out,
                     mem_pool_t *arena);
size_t exprtk_ta_trix(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_ultosc(const double *hi, const double *lo, const double *cl, size_t n, size_t p1,
                        size_t p2, size_t p3, double *out, mem_pool_t *arena);
size_t exprtk_ta_aroon(const double *hi, const double *lo, size_t n, size_t period, double *up,
                       double *dn, mem_pool_t *arena);

size_t exprtk_ta_aroonosc(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          mem_pool_t *arena);
size_t exprtk_ta_cmo(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);

/* TA-Lib Volatility */
size_t exprtk_ta_trange(const double *hi, const double *lo, const double *cl, size_t n,
                        double *out);
size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, mem_pool_t *arena);
size_t exprtk_ta_rsrs(const double *hi, const double *lo, size_t n, size_t n_reg, size_t m_z,
                      double *slope, double *zscore, mem_pool_t *arena);
size_t exprtk_ta_smart_money(const double *p, const double *v, size_t n, size_t period, double *out,
                             mem_pool_t *arena);
size_t exprtk_ta_qrs(const double *slope, size_t n, size_t period, double *out,
                     mem_pool_t *arena);
size_t exprtk_ta_vmacd_mtm(const double *v, size_t n, size_t period, double *out,
                           mem_pool_t *arena);
size_t exprtk_ta_noise_area(const double *op, const double *cl, size_t n, size_t period,
                            double *upper, double *lower, mem_pool_t *arena);
size_t exprtk_ta_w_factor(const double *ret, const double *amt, const double *cnt, size_t n,
                          size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_cpv(const double *ret, const double *vol, size_t n, size_t period, double *out,
                     mem_pool_t *arena);
size_t exprtk_ta_smma(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_alligator(const double *in, size_t n, double *jaw, double *teeth, double *lips,
                           mem_pool_t *arena);
size_t exprtk_ta_natr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                      double *out, mem_pool_t *arena);

/* TA-Lib Volume */
size_t exprtk_ta_obv(const double *cl, const double *vol, size_t n, double *out);
size_t exprtk_ta_ad(const double *hi, const double *lo, const double *cl, const double *vol,
                    size_t n, double *out);
size_t exprtk_ta_shadow(const double *op, const double *hi, const double *lo, const double *cl,
                        size_t n, double *upper, double *lower, mem_pool_t *arena);
size_t exprtk_ta_adosc(const double *hi, const double *lo, const double *cl, const double *vol,
                       size_t n, size_t fast_p, size_t slow_p, double *out, mem_pool_t *arena);
size_t exprtk_ta_mfi(const double *hi, const double *lo, const double *cl, const double *vol,
                     size_t n, size_t period, double *out, mem_pool_t *arena);

/* TA-Lib Trend */
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out,
                         mem_pool_t *arena);
size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          mem_pool_t *arena);
size_t exprtk_ta_plus_di(const double *hi, const double *lo, const double *cl, size_t n,
                         size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_minus_di(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_dx(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                    double *out, mem_pool_t *arena);
size_t exprtk_ta_adx(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, mem_pool_t *arena);
size_t exprtk_ta_adxr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                      double *out, mem_pool_t *arena);

/* TA-Lib Statistics */
size_t exprtk_ta_stddev(const double *in, size_t n, size_t period, double mult, double *out,
                        mem_pool_t *arena);
size_t exprtk_ta_var(const double *in, size_t n, size_t period, double mult, double *out,
                     mem_pool_t *arena);
size_t exprtk_ta_linearreg(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_slope(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_intercept(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_linearreg_angle(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_tsf(const double *in, size_t n, size_t period, double *out);
size_t exprtk_ta_beta(const double *in0, const double *in1, size_t n, size_t period, double *out);
size_t exprtk_ta_correl(const double *in0, const double *in1, size_t n, size_t period, double *out);

/* TA-Lib Price */
size_t exprtk_ta_avgprice(const double *o, const double *hi, const double *lo, const double *cl,
                          size_t n, double *out);
size_t exprtk_ta_medprice(const double *hi, const double *lo, size_t n, double *out);
size_t exprtk_ta_typprice(const double *hi, const double *lo, const double *cl, size_t n,
                          double *out);
size_t exprtk_ta_wclprice(const double *hi, const double *lo, const double *cl, size_t n,
                          double *out);

/* TA-Lib Options */
size_t exprtk_ta_bsm_call(const double *S, const double *K, const double *T, const double *r,
                          const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_put(const double *S, const double *K, const double *T, const double *r,
                         const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_delta_call(const double *S, const double *K, const double *T, const double *r,
                                const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_delta_put(const double *S, const double *K, const double *T, const double *r,
                               const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_gamma(const double *S, const double *K, const double *T, const double *r,
                           const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_theta_call(const double *S, const double *K, const double *T, const double *r,
                                const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_theta_put(const double *S, const double *K, const double *T, const double *r,
                               const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_vega(const double *S, const double *K, const double *T, const double *r,
                          const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_rho_call(const double *S, const double *K, const double *T, const double *r,
                              const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_rho_put(const double *S, const double *K, const double *T, const double *r,
                             const double *sigma, size_t n, double *out);
size_t exprtk_ta_bsm_iv_call(const double *price, const double *S, const double *K, const double *T,
                             const double *r, size_t n, double *out);
size_t exprtk_ta_bsm_iv_put(const double *price, const double *S, const double *K, const double *T,
                            const double *r, size_t n, double *out);
size_t exprtk_ta_opt_binomial(double S, double K, double T, double r, double sigma, size_t steps,
                              int is_call, double *out, mem_pool_t *arena);

/* TA New Indicators */
size_t exprtk_ta_fisher(const double *hi, const double *lo, size_t n, size_t period,
                         double *fisher, double *trigger, mem_pool_t *arena);
size_t exprtk_ta_squeeze(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t bb_p, double bb_m, size_t kc_p, double kc_m,
                          double *squeeze, double *momentum, double *on_off,
                          mem_pool_t *arena);
size_t exprtk_ta_chop(const double *hi, const double *lo, const double *cl, size_t n,
                       size_t period, double *out, mem_pool_t *arena);
size_t exprtk_ta_ehlers_cyber_cycle(const double *in, size_t n, double alpha, double *out);
size_t exprtk_ta_ehlers_itrend(const double *in, size_t n, double alpha, double *out);
size_t exprtk_ta_ehlers_mama(const double *in, size_t n, double fast_limit, double slow_limit,
                              double *mama, double *fama);

/* =========================================================================
 * Shared internal TA helpers (used across TA implementation units)
 * ========================================================================= */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void   ta_sma_calc(const double *src, size_t len, size_t period, double *dst);
void   ta_ema_calc(const double *src, size_t len, size_t period, double *dst);
void   ta_wma_calc(const double *src, size_t len, size_t period, double *dst);
void   ta_wilder_smooth(const double *src, size_t len, size_t period, double *dst);
void   ta_smma(const double *in, size_t n, size_t period, double *out);

double ta_highest(const double *src, size_t idx, size_t period);
double ta_lowest(const double *src, size_t idx, size_t period);
void   ta_highest_arr(const double *src, size_t n, size_t period, double *dst, mem_pool_t *arena);
void   ta_lowest_arr(const double *src, size_t n, size_t period, double *dst, mem_pool_t *arena);
void   ta_highest_idx_arr(const double *src, size_t n, size_t period, size_t *dst, mem_pool_t *arena);
void   ta_lowest_idx_arr(const double *src, size_t n, size_t period, size_t *dst, mem_pool_t *arena);
size_t ta_highest_idx(const double *src, size_t idx, size_t period);
size_t ta_lowest_idx(const double *src, size_t idx, size_t period);

double ta_true_range(double high, double low, double prev_close);
void   ta_true_range_arr(const double *high, const double *low, const double *close, size_t len, double *dst);
void   ta_linreg(const double *src, size_t end_idx, size_t period, double *slope, double *intercept);
double ta_ncdf(double x);
double ta_npdf(double x);

int compare_doubles(const void *a, const void *b);
int compare_rank_items(const void *a, const void *b);
double inv_normal_cdf(double p);

#ifdef __cplusplus
}
#endif

#endif /* TA_H */
