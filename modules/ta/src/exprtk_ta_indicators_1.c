/**
 * @file exprtk_ta_indicators_1.c
 * @brief Technical Analysis Overlap and Momentum Indicators.
 */

#include "simd_helpers.h"
#include "ta.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <stdlib.h>
#include <string.h>

// TA-Lib Overlap Indicators

size_t exprtk_ta_sma(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return 0;
  ta_sma_calc(in, n, period, out);
  return n;
}

size_t exprtk_ta_ema(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return 0;
  ta_ema_calc(in, n, period, out);
  return n;
}

size_t exprtk_ta_wma(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return 0;
  ta_wma_calc(in, n, period, out);
  return n;
}

size_t exprtk_ta_dema(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *e1 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e2 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!e1 || !e2) {
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);

  // out = 2.0 * e1 - e2
  simd_scale(e1, out, 2.0, n);
  simd_sub(out, e2, out, n);

  return n;
}

size_t exprtk_ta_tema(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *e1 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e2 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e3 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!e1 || !e2 || !e3) {
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  ta_ema_calc(e2, n, period, e3);

  // out = 3.0 * (e1 - e2) + e3
  simd_sub(e1, e2, out, n);
  simd_scale(out, out, 3.0, n);
  simd_add(out, e3, out, n);

  return n;
}

size_t exprtk_ta_kama(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period >= n)
    return 0;
  double kama = in[period - 1];
  out[period - 1] = kama;
  const double fast_sc = 2.0 / 3.0, slow_sc = 2.0 / 31.0;
  double volatility = 0;
  for (size_t j = 1; j <= period; ++j)
    volatility += fabs(in[j] - in[j - 1]);
  for (size_t i = period; i < n; ++i) {
    double change = fabs(in[i] - in[i - period]);
    if (i > period) {
      volatility = volatility + fabs(in[i] - in[i - 1]) - fabs(in[i - period] - in[i - period - 1]);
    }
    double er = (volatility > 0) ? change / volatility : 0;
    double sc = pow(er * (fast_sc - slow_sc) + slow_sc, 2.0);
    kama = kama + sc * (in[i] - kama);
    out[i] = kama;
  }
  return n;
}

size_t exprtk_ta_t3(const double *in, size_t n, size_t period, double vfactor, double *out,
                    mem_pool_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *e1 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e2 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e3 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e4 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e5 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e6 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!e1 || !e2 || !e3 || !e4 || !e5 || !e6) {
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  ta_ema_calc(e2, n, period, e3);
  ta_ema_calc(e3, n, period, e4);
  ta_ema_calc(e4, n, period, e5);
  ta_ema_calc(e5, n, period, e6);
  double c1 = -vfactor * vfactor * vfactor;
  double c2 = 3.0 * vfactor * vfactor + 3.0 * vfactor * vfactor * vfactor;
  double c3 = -6.0 * vfactor * vfactor - 3.0 * vfactor - 3.0 * vfactor * vfactor * vfactor;
  double c4 = 1.0 + 3.0 * vfactor + vfactor * vfactor * vfactor + 3.0 * vfactor * vfactor;
  size_t i = (period > 0 ? 6 * period - 6 : 0);
  simde__m256d v_c1 = simde_mm256_set1_pd(c1);
  simde__m256d v_c2 = simde_mm256_set1_pd(c2);
  simde__m256d v_c3 = simde_mm256_set1_pd(c3);
  simde__m256d v_c4 = simde_mm256_set1_pd(c4);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e6 = simde_mm256_loadu_pd(&e6[i]);
    simde__m256d v_e5 = simde_mm256_loadu_pd(&e5[i]);
    simde__m256d v_e4 = simde_mm256_loadu_pd(&e4[i]);
    simde__m256d v_e3 = simde_mm256_loadu_pd(&e3[i]);
    simde__m256d v_res = simde_mm256_add_pd(
        simde_mm256_add_pd(simde_mm256_mul_pd(v_c1, v_e6), simde_mm256_mul_pd(v_c2, v_e5)),
        simde_mm256_add_pd(simde_mm256_mul_pd(v_c3, v_e4), simde_mm256_mul_pd(v_c4, v_e3)));
    simde_mm256_storeu_pd(&out[i], v_res);
  }
  for (; i < n; ++i) {
    out[i] = c1 * e6[i] + c2 * e5[i] + c3 * e4[i] + c4 * e3[i];
  }
  return n;
}

size_t exprtk_ta_trima(const double *in, size_t n, size_t period, double *out,
                       mem_pool_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *tmp = MEM_ALLOC_ARRAY(arena, double, n);
  if (!tmp)
    return 0;
  size_t p1 = (period + 1) / 2;
  size_t p2 = (period % 2 == 0) ? p1 + 1 : p1;
  ta_sma_calc(in, n, p1, tmp);
  ta_sma_calc(tmp, n, p2, out);
  return n;
}

size_t exprtk_ta_bbands(const double *in, size_t n, size_t period, double mult, double *upper,
                        double *middle, double *lower) {
  if (period == 0 || period > n)
    return 0;
  double sum = 0, sum2 = 0;
  for (size_t i = 0; i < period; ++i) {
    sum += in[i];
    sum2 += in[i] * in[i];
  }
  double inv_p = 1.0 / (double)period;
  double mean = sum * inv_p;
  double var = (sum2 - (sum * sum) * inv_p) * inv_p;
  double dev = mult * sqrt(fmax(0, var));
  middle[period - 1] = mean;
  upper[period - 1] = mean + dev;
  lower[period - 1] = mean - dev;

  for (size_t i = period; i < n; ++i) {
    double old = in[i - period];
    sum += in[i] - old;
    sum2 += in[i] * in[i] - old * old;
    double cur_mean = sum * inv_p;
    double cur_var = (sum2 - (sum * sum) * inv_p) * inv_p;
    double cur_dev = mult * sqrt(fmax(0, cur_var));
    middle[i] = cur_mean;
    upper[i] = cur_mean + cur_dev;
    lower[i] = cur_mean - cur_dev;
  }
  return n;
}

size_t exprtk_ta_midpoint(const double *in, size_t n, size_t period, double *out,
                          mem_pool_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *hh = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll = MEM_ALLOC_ARRAY(arena, double, n);
  if (!hh || !ll) {
    return 0;
  }
  ta_highest_arr(in, n, period, hh, arena);
  ta_lowest_arr(in, n, period, ll, arena);
  size_t i = period - 1;
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(
        &out[i],
        simde_mm256_mul_pd(
            simde_mm256_add_pd(simde_mm256_loadu_pd(&hh[i]), simde_mm256_loadu_pd(&ll[i])), v_05));
  }
  for (; i < n; ++i)
    out[i] = (hh[i] + ll[i]) * 0.5;
  return n;
}

size_t exprtk_ta_midprice(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          mem_pool_t *arena) {
  if (period == 0 || period > n)
    return 0;
  double *upper = MEM_ALLOC_ARRAY(arena, double, n);
  double *lower = MEM_ALLOC_ARRAY(arena, double, n);
  if (!upper || !lower) {
    return 0;
  }
  ta_highest_arr(hi, n, period, upper, arena);
  ta_lowest_arr(lo, n, period, lower, arena);
  size_t i = period - 1;
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&out[i],
                          simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_loadu_pd(&upper[i]),
                                                                simde_mm256_loadu_pd(&lower[i])),
                                             v_05));
  }
  for (; i < n; ++i)
    out[i] = (upper[i] + lower[i]) * 0.5;
  return n;
}

size_t exprtk_ta_sar(const double *hi, const double *lo, size_t n, double accel_init,
                     double accel_max, double *out) {
  if (n < 2)
    return 0;
  int is_long = (hi[1] > hi[0] || lo[1] > lo[0]);
  double sar = is_long ? lo[0] : hi[0];
  double ep = is_long ? hi[1] : lo[1];
  double af = accel_init;
  out[0] = sar;
  for (size_t i = 1; i < n; ++i) {
    double next_sar = sar + af * (ep - sar);
    if (is_long) {
      if (i >= 2)
        next_sar = fmin(next_sar, fmin(lo[i - 1], lo[i - 2]));
      else
        next_sar = fmin(next_sar, lo[i - 1]);
      if (lo[i] < next_sar) {
        is_long = 0;
        sar = ep;
        ep = lo[i];
        af = accel_init;
      } else {
        sar = next_sar;
        if (hi[i] > ep) {
          ep = hi[i];
          af = fmin(af + accel_init, accel_max);
        }
      }
    } else {
      if (i >= 2)
        next_sar = fmax(next_sar, fmax(hi[i - 1], hi[i - 2]));
      else
        next_sar = fmax(next_sar, hi[i - 1]);
      if (hi[i] > next_sar) {
        is_long = 1;
        sar = ep;
        ep = hi[i];
        af = accel_init;
      } else {
        sar = next_sar;
        if (lo[i] < ep) {
          ep = lo[i];
          af = fmin(af + accel_init, accel_max);
        }
      }
    }
    out[i] = sar;
  }
  return n;
}

size_t exprtk_ta_savgol(const double *in, size_t n, size_t window, double *out) {
  if (window < 3 || window > n || window % 2 == 0)
    return 0;
  if (window == 5) {
    size_t i = 2;
    simde__m256d v_3 = simde_mm256_set1_pd(-3.0);
    simde__m256d v_12 = simde_mm256_set1_pd(12.0);
    simde__m256d v_17 = simde_mm256_set1_pd(17.0);
    simde__m256d v_inv35 = simde_mm256_set1_pd(1.0 / 35.0);
    for (; i + 4 <= n - 2; i += 4) {
      simde__m256d v_im2 = simde_mm256_loadu_pd(&in[i - 2]);
      simde__m256d v_im1 = simde_mm256_loadu_pd(&in[i - 1]);
      simde__m256d v_i0 = simde_mm256_loadu_pd(&in[i]);
      simde__m256d v_ip1 = simde_mm256_loadu_pd(&in[i + 1]);
      simde__m256d v_ip2 = simde_mm256_loadu_pd(&in[i + 2]);
      simde__m256d v_res = simde_mm256_add_pd(
          simde_mm256_add_pd(simde_mm256_mul_pd(v_im2, v_3), simde_mm256_mul_pd(v_im1, v_12)),
          simde_mm256_add_pd(
              simde_mm256_mul_pd(v_i0, v_17),
              simde_mm256_add_pd(simde_mm256_mul_pd(v_ip1, v_12), simde_mm256_mul_pd(v_ip2, v_3))));
      simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v_res, v_inv35));
    }
    for (; i < n - 2; ++i)
      out[i] = (-3.0 * in[i - 2] + 12.0 * in[i - 1] + 17.0 * in[i] + 12.0 * in[i + 1] -
                3.0 * in[i + 2]) /
               35.0;

  } else {
    ta_sma_calc(in, n, window, out);
  }
  return n;
}

// TA-Lib Momentum Indicators
size_t exprtk_ta_rsi(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || period >= n) {
    memset(out, 0, n * sizeof(double));
    return 0;
  }
  double *g = MEM_ALLOC_ARRAY(arena, double, n);
  double *l = MEM_ALLOC_ARRAY(arena, double, n);
  if (!g || !l) {
    return 0;
  }

  g[0] = 0;
  l[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_c = simde_mm256_loadu_pd(&in[i]);
    simde__m256d v_p = simde_mm256_loadu_pd(&in[i - 1]);
    simde__m256d v_d = simde_mm256_sub_pd(v_c, v_p);
    simde_mm256_storeu_pd(&g[i], simde_mm256_max_pd(v_d, v_zero));
    simde_mm256_storeu_pd(&l[i], simde_mm256_max_pd(simde_mm256_sub_pd(v_zero, v_d), v_zero));
  }
  for (; i < n; ++i) {
    double d = in[i] - in[i - 1];
    g[i] = d > 0 ? d : 0;
    l[i] = d < 0 ? -d : 0;
  }

  double *ag = MEM_ALLOC_ARRAY(arena, double, n);
  double *al = MEM_ALLOC_ARRAY(arena, double, n);
  if (!ag || !al) {
    return 0;
  }

  ta_wilder_smooth(g + 1, n - 1, period, ag + 1);
  ta_wilder_smooth(l + 1, n - 1, period, al + 1);

  memset(out, 0, (period + 1) * sizeof(double));
  for (size_t k = period; k < n; ++k) {
    if (al[k] < 1e-15)
      out[k] = (ag[k] < 1e-15) ? 50.0 : 100.0;
    else {
      double rs = ag[k] / al[k];
      out[k] = 100.0 - (100.0 / (1.0 + rs));
    }
  }
  return n;
}

size_t exprtk_ta_macd(const double *in, size_t n, size_t fast, size_t slow, size_t signal,
                      double *macd, double *sig, double *hist, mem_pool_t *arena) {
  if (fast == 0 || slow == 0 || signal == 0 || n < slow)
    return 0;
  exprtk_ta_apo(in, n, fast, slow, macd, arena);
  ta_ema_calc(macd + slow - 1, n - (slow - 1), signal, sig + slow - 1);
  size_t i = slow + signal - 2;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&hist[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&macd[i]),
                                                       simde_mm256_loadu_pd(&sig[i])));
  }
  for (; i < n; ++i)
    hist[i] = macd[i] - sig[i];
  return n;
}

size_t exprtk_ta_stoch(const double *hi, const double *lo, const double *cl, size_t n, size_t k_p,
                       size_t d_p, double *out_k, double *out_d, mem_pool_t *arena) {
  if (k_p == 0 || d_p == 0 || n < k_p)
    return 0;
  double *fast_k = MEM_ALLOC_ARRAY(arena, double, n);
  double *hh = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll = MEM_ALLOC_ARRAY(arena, double, n);
  if (!fast_k || !hh || !ll) {
    return 0;
  }

  ta_highest_arr(hi, n, k_p, hh, arena);
  ta_lowest_arr(lo, n, k_p, ll, arena);

  size_t i = k_p - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hh = simde_mm256_loadu_pd(&hh[i]);
    simde__m256d v_ll = simde_mm256_loadu_pd(&ll[i]);
    simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_range = simde_mm256_sub_pd(v_hh, v_ll);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_cl, v_ll), v_range), v_100);
    simde_mm256_storeu_pd(&fast_k[i], simde_mm256_and_pd(v_res, v_mask)); // Store to fast_k
  }
  for (; i < n; ++i) {
    double range = hh[i] - ll[i];
    fast_k[i] = (range > 0) ? 100.0 * (cl[i] - ll[i]) / range : 0;
  }
  ta_sma_calc(fast_k + k_p - 1, n - (k_p - 1), 3, out_k + k_p - 1);
  ta_sma_calc(out_k + k_p - 1, n - (k_p - 1), d_p, out_d + k_p - 1);
  return n;
}

size_t exprtk_ta_stochrsi(const double *in, size_t n, size_t rsi_p, size_t k_p, size_t d_p,
                          double *out_k, double *out_d, mem_pool_t *arena) {
  if (rsi_p == 0 || n < rsi_p + k_p)
    return 0;
  double *rsi = MEM_ALLOC_ARRAY(arena, double, n);
  if (!rsi)
    return 0;
  exprtk_ta_rsi(in, n, rsi_p, rsi, arena);
  double *fast_k = MEM_ALLOC_ARRAY(arena, double, n);
  double *hh = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll = MEM_ALLOC_ARRAY(arena, double, n);
  if (!fast_k || !hh || !ll) {
    return 0;
  }

  ta_highest_arr(rsi, n, k_p, hh, arena);
  ta_lowest_arr(rsi, n, k_p, ll, arena);

  for (size_t i = rsi_p + k_p - 1; i < n; ++i) {
    fast_k[i] = (hh[i] > ll[i]) ? 100.0 * (rsi[i] - ll[i]) / (hh[i] - ll[i]) : 100.0;
  }
  ta_sma_calc(fast_k + rsi_p + k_p - 1, n - (rsi_p + k_p - 1), 3, out_k + rsi_p + k_p - 1);
  ta_sma_calc(out_k + rsi_p + k_p - 1, n - (rsi_p + k_p - 1), d_p, out_d + rsi_p + k_p - 1);
  return n;
}

size_t exprtk_ta_willr(const double *hi, const double *lo, const double *cl, size_t n,
                       size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *hh = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll = MEM_ALLOC_ARRAY(arena, double, n);
  if (!hh || !ll) {
    return 0;
  }
  ta_highest_arr(hi, n, period, hh, arena);
  ta_lowest_arr(lo, n, period, ll, arena);
  size_t i = period - 1;
  simde__m256d v_m100 = simde_mm256_set1_pd(-100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hh = simde_mm256_loadu_pd(&hh[i]);
    simde__m256d v_ll = simde_mm256_loadu_pd(&ll[i]);
    simde__m256d v_cl = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_range = simde_mm256_sub_pd(v_hh, v_ll);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_hh, v_cl), v_range), v_m100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double range = hh[i] - ll[i];
    out[i] = (range > 0) ? -100.0 * (hh[i] - cl[i]) / range : 0;
  }
  return n;
}

size_t exprtk_ta_cci(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *tp = MEM_ALLOC_ARRAY(arena, double, n);
  double *sma_tp = MEM_ALLOC_ARRAY(arena, double, n);
  if (!tp || !sma_tp) {
    return 0;
  }
  size_t i = 0;
  simde__m256d v_inv3 = simde_mm256_set1_pd(1.0 / 3.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c = simde_mm256_loadu_pd(&cl[i]);
    simde_mm256_storeu_pd(
        &tp[i], simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_add_pd(v_h, v_l), v_c), v_inv3));
  }
  for (; i < n; ++i)
    tp[i] = (hi[i] + lo[i] + cl[i]) / 3.0;
  ta_sma_calc(tp, n, period, sma_tp);

  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
  for (i = period - 1; i < n; ++i) {

    double md = 0;
    double mean = sma_tp[i];
    size_t j = 0;
    simde__m256d v_sma = simde_mm256_set1_pd(sma_tp[i]);
    simde__m256d v_sum_md = simde_mm256_setzero_pd();

    j = 0;
    for (; j + 4 <= period; j += 4) {
      simde__m256d v_tp_val = simde_mm256_loadu_pd(&tp[i - j - 3]);
      simde__m256d v_diff = simde_mm256_sub_pd(v_tp_val, v_sma);
      v_sum_md = simde_mm256_add_pd(v_sum_md, simde_mm256_andnot_pd(v_sign_mask, v_diff));
    }
    double tmp_md[4];
    simde_mm256_storeu_pd(tmp_md, v_sum_md);
    md = tmp_md[0] + tmp_md[1] + tmp_md[2] + tmp_md[3];

    for (; j < period; ++j)
      md += fabs(tp[i - j] - mean);
    md /= (double)period;
    out[i] = (md > 1e-15) ? (tp[i] - mean) / (0.015 * md) : 0;
  }
  return n;
}

size_t exprtk_ta_mom(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n <= period)
    return 0;
  size_t i = period;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&in[i]),
                                                      simde_mm256_loadu_pd(&in[i - period])));
  }
  for (; i < n; ++i)
    out[i] = in[i] - in[i - period];
  return n;
}

size_t exprtk_ta_roc(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n <= period)
    return 0;
  size_t i = period;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_curr = simde_mm256_loadu_pd(&in[i]);
    simde__m256d v_prev = simde_mm256_loadu_pd(&in[i - period]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_prev, v_zero, SIMDE_CMP_NEQ_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_curr, v_prev), v_prev), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (in[i - period] != 0) ? 100.0 * (in[i] - in[i - period]) / in[i - period] : 0;
  return n;
}

size_t exprtk_ta_apo(const double *in, size_t n, size_t fast, size_t slow, double *out,
                     mem_pool_t *arena) {
  if (fast == 0 || slow == 0 || n < slow)
    return 0;
  double *e1 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e2 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!e1 || !e2) {
    return 0;
  }
  ta_ema_calc(in, n, fast, e1);
  ta_ema_calc(in, n, slow, e2);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(
        &out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&e1[i]), simde_mm256_loadu_pd(&e2[i])));
  }
  for (; i < n; ++i)
    out[i] = e1[i] - e2[i];
  return n;
}

size_t exprtk_ta_ppo(const double *in, size_t n, size_t fast, size_t slow, double *out,
                     mem_pool_t *arena) {
  if (fast == 0 || slow == 0 || n < slow)
    return 0;
  double *e1 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e2 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!e1 || !e2) {
    return 0;
  }
  ta_ema_calc(in, n, fast, e1);
  ta_ema_calc(in, n, slow, e2);
  size_t i = 0;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e1 = simde_mm256_loadu_pd(&e1[i]);
    simde__m256d v_e2 = simde_mm256_loadu_pd(&e2[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_e2, v_zero, SIMDE_CMP_NEQ_OQ);
    simde__m256d v_res =
        simde_mm256_mul_pd(simde_mm256_div_pd(simde_mm256_sub_pd(v_e1, v_e2), v_e2), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (e2[i] != 0) ? 100.0 * (e1[i] - e2[i]) / e2[i] : 0;
  return n;
}

size_t exprtk_ta_trix(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < 3 * period)
    return 0;
  double *e1 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e2 = MEM_ALLOC_ARRAY(arena, double, n);
  double *e3 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!e1 || !e2 || !e3) {
    return 0;
  }
  ta_ema_calc(in, n, period, e1);
  ta_ema_calc(e1, n, period, e2);
  ta_ema_calc(e2, n, period, e3);
  size_t i = 3 * period - 2; // Correct starting index for TRIX calculation
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_e3_curr = simde_mm256_loadu_pd(&e3[i]);
    simde__m256d v_e3_prev = simde_mm256_loadu_pd(&e3[i - 1]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_e3_prev, v_epsilon, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(
        simde_mm256_div_pd(simde_mm256_sub_pd(v_e3_curr, v_e3_prev), v_e3_prev), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (e3[i - 1] > 1e-15) ? (e3[i] - e3[i - 1]) / e3[i - 1] * 100.0 : 0;
  return n;
}

size_t exprtk_ta_ultosc(const double *hi, const double *lo, const double *cl, size_t n, size_t p1,
                        size_t p2, size_t p3, double *out, mem_pool_t *arena) {
  if (p1 == 0 || p2 == 0 || p3 == 0 || n < p1 || n < p2 || n < p3)
    return 0;
  double *bp = MEM_ALLOC_ARRAY(arena, double, n);
  double *tr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!bp || !tr) {
    return 0;
  }

  // Initial BP and TR calculation - Vectorized
  size_t i = 1;
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hi = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_lo = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c_prev = simde_mm256_loadu_pd(&cl[i - 1]);
    simde__m256d v_c_curr = simde_mm256_loadu_pd(&cl[i]);

    simde__m256d v_low_min = simde_mm256_min_pd(v_lo, v_c_prev);
    simde__m256d v_bp = simde_mm256_sub_pd(v_c_curr, v_low_min);
    simde__m256d v_tr = simde_mm256_max_pd(
        simde_mm256_sub_pd(v_hi, v_low_min),
        simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_c_prev, v_low_min)));
    v_tr = simde_mm256_max_pd(v_tr, v_epsilon);

    simde_mm256_storeu_pd(&bp[i], v_bp);
    simde_mm256_storeu_pd(&tr[i], v_tr);
  }
  for (; i < n; ++i) {
    double low_min = fmin(lo[i], cl[i - 1]);
    bp[i] = cl[i] - low_min;
    double tr_val = fmax(hi[i], cl[i - 1]) - low_min;
    tr[i] = (tr_val < 1e-15) ? 1e-15 : tr_val;
  }

  double s1_bp = 0, s1_tr = 0, s2_bp = 0, s2_tr = 0, s3_bp = 0, s3_tr = 0;
  size_t max_p = (size_t)fmax(p1, fmax(p2, p3));

  for (i = 1; i < n; ++i) {

    s1_bp += bp[i];
    s1_tr += tr[i];
    s2_bp += bp[i];
    s2_tr += tr[i];
    s3_bp += bp[i];
    s3_tr += tr[i];

    if (i > p1) {
      s1_bp -= bp[i - p1];
      s1_tr -= tr[i - p1];
    }
    if (i > p2) {
      s2_bp -= bp[i - p2];
      s2_tr -= tr[i - p2];
    }
    if (i > p3) {
      s3_bp -= bp[i - p3];
      s3_tr -= tr[i - p3];
    }

    if (i >= max_p) {
      double a1 = (s1_tr > 0) ? s1_bp / s1_tr : 0;
      double a2 = (s2_tr > 0) ? s2_bp / s2_tr : 0;
      double a3 = (s3_tr > 0) ? s3_bp / s3_tr : 0;
      out[i] = 100.0 * (4.0 * a1 + 2.0 * a2 + a3) / 7.0;
    }
  }

  return n;
}

size_t exprtk_ta_aroon(const double *hi, const double *lo, size_t n, size_t period, double *up,
                       double *dn, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  size_t *hi_idx = MEM_ALLOC_ARRAY(arena, size_t, n);
  size_t *lo_idx = MEM_ALLOC_ARRAY(arena, size_t, n);
  if (!hi_idx || !lo_idx)
    return 0;
  ta_highest_idx_arr(hi, n, period + 1, hi_idx, arena);
  ta_lowest_idx_arr(lo, n, period + 1, lo_idx, arena);

  size_t i = period;
  double inv_p = 100.0 / (double)period;
  for (; i < n; ++i) {
    up[i] = (double)(period - (i - hi_idx[i])) * inv_p;
    dn[i] = (double)(period - (i - lo_idx[i])) * inv_p;
  }

  return n;
}

size_t exprtk_ta_aroonosc(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *up = MEM_ALLOC_ARRAY(arena, double, n);
  double *dn = MEM_ALLOC_ARRAY(arena, double, n);
  if (!up || !dn) {
    return 0;
  }
  exprtk_ta_aroon(hi, lo, n, period, up, dn, arena);
  size_t i = period;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(
        &out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&up[i]), simde_mm256_loadu_pd(&dn[i])));
  }
  for (; i < n; ++i)
    out[i] = up[i] - dn[i];
  return n;
}

size_t exprtk_ta_cmo(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < period + 1)
    return 0;
  double *up = MEM_ALLOC_ARRAY(arena, double, n);
  double *dn = MEM_ALLOC_ARRAY(arena, double, n);
  if (!up || !dn) {
    return 0;
  }

  up[0] = 0;
  dn[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_c = simde_mm256_loadu_pd(&in[i]);
    simde__m256d v_p = simde_mm256_loadu_pd(&in[i - 1]);
    simde__m256d v_d = simde_mm256_sub_pd(v_c, v_p);
    simde_mm256_storeu_pd(&up[i], simde_mm256_max_pd(v_d, v_zero));
    simde_mm256_storeu_pd(&dn[i], simde_mm256_max_pd(simde_mm256_sub_pd(v_zero, v_d), v_zero));
  }
  for (; i < n; ++i) {
    double d = in[i] - in[i - 1];
    up[i] = d > 0 ? d : 0;
    dn[i] = d < 0 ? -d : 0;
  }

  double sum_up = 0, sum_dn = 0;
  for (i = 1; i <= period; ++i) {
    sum_up += up[i];
    sum_dn += dn[i];
  }
  out[period] = (sum_up + sum_dn > 1e-15) ? 100.0 * (sum_up - sum_dn) / (sum_up + sum_dn) : 0;

  for (i = period + 1; i < n; ++i) {
    sum_up += up[i] - up[i - period];
    sum_dn += dn[i] - dn[i - period];
    out[i] = (sum_up + sum_dn > 1e-15) ? 100.0 * (sum_up - sum_dn) / (sum_up + sum_dn) : 0;
  }

  return n;
}

size_t exprtk_ta_smma(const double *in, size_t n, size_t period, double *out) {
  if (n < period)
    return 0;
  ta_smma(in, n, period, out);
  return n;
}
