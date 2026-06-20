/**
 * @file exprtk_ta_others.c
 * @brief Technical Analysis Other indicators and complex ones.
 */

#include "simd_helpers.h"
#include "ta.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_buffer.h>


size_t exprtk_ta_bbi(const double *in, size_t n, double *out, mem_pool_t *arena) {
  if (n < 24)
    return 0;
  double *ma3 = MEM_ALLOC_ARRAY(arena, double, n);
  double *ma6 = MEM_ALLOC_ARRAY(arena, double, n);
  double *ma12 = MEM_ALLOC_ARRAY(arena, double, n);
  double *ma24 = MEM_ALLOC_ARRAY(arena, double, n);
  if (!ma3 || !ma6 || !ma12 || !ma24) {
    return 0;
  }
  ta_sma_calc(in, n, 3, ma3);
  ta_sma_calc(in, n, 6, ma6);
  ta_sma_calc(in, n, 12, ma12);
  ta_sma_calc(in, n, 24, ma24);

  // out = (ma3 + ma6 + ma12 + ma24) * 0.25
  simd_add(ma3, ma6, out, n);
  simd_add(out, ma12, out, n);
  simd_add(out, ma24, out, n);
  simd_scale(out, out, 0.25, n);
  return n;
}

size_t exprtk_ta_hma(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period < 2 || period > n)
    return 0;
  double *wma_half = MEM_ALLOC_ARRAY(arena, double, n);
  double *wma_full = MEM_ALLOC_ARRAY(arena, double, n);
  double *diff = MEM_ALLOC_ARRAY(arena, double, n);
  ta_wma_calc(in, n, period / 2, wma_half);
  ta_wma_calc(in, n, period, wma_full);

  // diff = 2.0 * wma_half - wma_full
  simd_scale(wma_half, diff, 2.0, n);
  simd_sub(diff, wma_full, diff, n);

  size_t sqrt_period = (size_t)sqrt((double)period);
  ta_wma_calc(diff, n, sqrt_period, out);

  return n;
}

size_t exprtk_ta_vwap(const double *hi, const double *lo, const double *cl, const double *vol,
                      size_t n, double *out) {
  if (n == 0)
    return 0;
  double pv_sum = 0, v_sum = 0;
  size_t i = 0;
  simde__m256d v_inv3 = simde_mm256_set1_pd(1.0 / 3.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_tp =
        simde_mm256_mul_pd(simde_mm256_add_pd(simde_mm256_add_pd(simde_mm256_loadu_pd(&hi[i]),
                                                                 simde_mm256_loadu_pd(&lo[i])),
                                              simde_mm256_loadu_pd(&cl[i])),
                           v_inv3);
    simde__m256d v_pv = simde_mm256_mul_pd(v_tp, simde_mm256_loadu_pd(&vol[i]));
    simde__m256d v_v = simde_mm256_loadu_pd(&vol[i]);
    double pvs[4], vs[4];
    simde_mm256_storeu_pd(pvs, v_pv);
    simde_mm256_storeu_pd(vs, v_v);
    for (int j = 0; j < 4; ++j) {
      pv_sum += pvs[j];
      v_sum += vs[j];
      out[i + j] = (v_sum > 1e-15) ? pv_sum / v_sum : 0;
    }
  }
  for (; i < n; ++i) {
    pv_sum += (hi[i] + lo[i] + cl[i]) / 3.0 * vol[i];
    v_sum += vol[i];
    out[i] = (v_sum > 1e-15) ? pv_sum / v_sum : 0;
  }
  return n;
}

size_t exprtk_ta_vwap_session(const double *hi, const double *lo, const double *cl,
                              const double *vol, const double *session, size_t n, double *out) {
  if (n == 0 || !session)
    return 0;

  double pv_sum = 0, v_sum = 0;
  for (size_t i = 0; i < n; ++i) {
    if (i == 0 || session[i] != session[i - 1]) {
      pv_sum = 0;
      v_sum = 0;
    }
    pv_sum += (hi[i] + lo[i] + cl[i]) / 3.0 * vol[i];
    v_sum += vol[i];
    out[i] = (v_sum > 1e-15) ? pv_sum / v_sum : 0;
  }
  return n;
}

size_t exprtk_ta_donchian(const double *hi, const double *lo, size_t n, size_t period,
                          double *upper, double *lower, double *middle, mem_pool_t *arena) {
  if (n < period)
    return 0;
  double *hh = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll = MEM_ALLOC_ARRAY(arena, double, n);
  if (!hh || !ll) {
    return 0;
  }
  ta_highest_arr(hi, n, period, hh, arena);
  ta_lowest_arr(lo, n, period, ll, arena);

  if (upper)
    memcpy(upper, hh, n * sizeof(double));
  if (lower)
    memcpy(lower, ll, n * sizeof(double));
  if (middle) {
    simd_add(hh, ll, middle, n);
    simd_scale(middle, middle, 0.5, n);
  }
  for (size_t i = 0; i < period - 1; ++i) {
    if (upper)
      upper[i] = 0;
    if (lower)
      lower[i] = 0;
    if (middle)
      middle[i] = 0;
  }
  return n;
}

size_t exprtk_ta_keltner(const double *hi, const double *lo, const double *cl, size_t n,
                         size_t ema_p, size_t atr_p, double mult, double *upper, double *middle,
                         double *lower, mem_pool_t *arena) {
  if (ema_p == 0 || atr_p == 0 || n < ema_p || n < atr_p)
    return 0;

  // Middle is EMA
  ta_ema_calc(cl, n, ema_p, middle);

  // Volatility is ATR
  double *atr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, atr_p, atr, arena);

  size_t start = (ema_p > atr_p ? ema_p : atr_p) - 1;
  for (size_t i = 0; i < n; ++i) {
    if (i < start) {
      if (upper)
        upper[i] = 0;
      if (lower)
        lower[i] = 0;
      continue;
    }
    if (upper)
      upper[i] = middle[i] + mult * atr[i];
    if (lower)
      lower[i] = middle[i] - mult * atr[i];
  }

  return n;
}

size_t exprtk_ta_ichimoku(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t tenkan_p, size_t kijun_p, size_t senkou_p, double *tenkan,
                          double *kijun, double *senkou_a, double *senkou_b, double *chikou,
                          mem_pool_t *arena) {
  if (n < tenkan_p || n < kijun_p || n < senkou_p)
    return 0;
  double *hh_tenkan = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll_tenkan = MEM_ALLOC_ARRAY(arena, double, n);
  double *hh_kijun = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll_kijun = MEM_ALLOC_ARRAY(arena, double, n);
  double *hh_senkou = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll_senkou = MEM_ALLOC_ARRAY(arena, double, n);
  if (!hh_tenkan || !ll_tenkan || !hh_kijun || !ll_kijun || !hh_senkou || !ll_senkou) {
    return 0;
  }
  ta_highest_arr(hi, n, tenkan_p, hh_tenkan, arena);
  ta_lowest_arr(lo, n, tenkan_p, ll_tenkan, arena);
  ta_highest_arr(hi, n, kijun_p, hh_kijun, arena);
  ta_lowest_arr(lo, n, kijun_p, ll_kijun, arena);
  ta_highest_arr(hi, n, senkou_p, hh_senkou, arena);
  ta_lowest_arr(lo, n, senkou_p, ll_senkou, arena);

  /* NOTE: ALLOC_DBL / TEMP_ALLOC zero-fills via arena. The first kijun_p
     elements of senkou_a[] and senkou_b[] will read tenkan[i - kijun_p] and
     kijun[i - kijun_p] from the un-computed prefix — these are zeros by
     design, matching TradingView behavior where Senkou Span is undefined
     before enough bars have been processed. */
  size_t i = 0;
  // Compute Tenkan and Kijun
  simd_add(hh_tenkan, ll_tenkan, tenkan, n);
  simd_scale(tenkan, tenkan, 0.5, n);
  simd_add(hh_kijun, ll_kijun, kijun, n);
  simd_scale(kijun, kijun, 0.5, n);

  // Compute Senkou A/B and lagged chikou without exposing future values at index i.
  for (i = 0; i < n; i++) {
    if (i >= kijun_p) {
      senkou_a[i] = (tenkan[i - kijun_p] + kijun[i - kijun_p]) / 2.0;
      senkou_b[i] = (hh_senkou[i - kijun_p] + ll_senkou[i - kijun_p]) / 2.0;
    } else {
      senkou_a[i] = 0;
      senkou_b[i] = 0;
    }
    chikou[i] = 0;
    if (i >= kijun_p)
      chikou[i] = cl[i - kijun_p];
  }
  return n;
}

size_t exprtk_ta_supertrend(const double *hi, const double *lo, const double *cl, size_t n,
                            size_t period, double mult, double *trend, double *upper, double *lower,
                            mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *atr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);

  double *f_up = upper;
  double *f_lo = lower;
  int cur_trend = 1;

  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      f_up[i] = f_lo[i] = trend[i] = 0;
      continue;
    }
    double med = (hi[i] + lo[i]) / 2.0;
    double b_up = med + mult * atr[i];
    double b_lo = med - mult * atr[i];

    if (i == period - 1) {
      f_up[i] = b_up;
      f_lo[i] = b_lo;
      trend[i] = (cl[i] > med) ? 1 : -1;
      cur_trend = (int)trend[i];
    } else {
      f_up[i] = (b_up < f_up[i - 1] || cl[i - 1] > f_up[i - 1]) ? b_up : f_up[i - 1];
      f_lo[i] = (b_lo > f_lo[i - 1] || cl[i - 1] < f_lo[i - 1]) ? b_lo : f_lo[i - 1];

      if (cur_trend == 1 && cl[i] < f_lo[i])
        cur_trend = -1;
      else if (cur_trend == -1 && cl[i] > f_up[i])
        cur_trend = 1;
      trend[i] = (double)cur_trend;
    }
  }

  return n;
}

size_t exprtk_ta_rma(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n == 0)
    return 0;
  double alpha = 1.0 / (double)period;
  double val = in[0];
  for (size_t i = 1; i < (n < period ? n : period); ++i)
    val += in[i];
  val /= (size_t)(n < period ? n : period);
  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      out[i] = 0;
      continue;
    }
    if (i == period - 1) {
      out[i] = val;
    } else {
      val = alpha * in[i] + (1.0 - alpha) * val;
      out[i] = val;
    }
  }
  return n;
}

size_t exprtk_ta_zlema(const double *in, size_t n, size_t period, double *out,
                       mem_pool_t *arena) {
  if (period < 2 || n < period)
    return 0;
  size_t lag = (period - 1) / 2;
  double *src = MEM_ALLOC_ARRAY(arena, double, n);
  if (!src)
    return 0;
  for (size_t i = 0; i < n; ++i) {
    if (i < lag)
      src[i] = in[i];
    else
      src[i] = in[i] + (in[i] - in[i - lag]);
  }
  ta_ema_calc(src, n, period, out);
  return n;
}

size_t exprtk_ta_alma(const double *in, size_t n, size_t period, double offset, double sigma,
                      double *out, mem_pool_t *arena) {
  if (period < 2 || n < period)
    return 0;
  double *w = MEM_ALLOC_ARRAY(arena, double, period);
  if (!w)
    return 0;
  double m = offset * (double)(period - 1);
  double s = (double)period / sigma;
  double w_sum = 0;
  for (size_t i = 0; i < period; ++i) {
    double d = (double)i - m;
    w[i] = exp(-(d * d) / (2.0 * s * s));
    w_sum += w[i];
  }
  for (size_t i = 0; i < period; ++i)
    w[i] /= w_sum;

  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      out[i] = 0;
      continue;
    }
    double val = 0;
    size_t j = 0;
    if (period >= 4) {
      simde__m256d v_val = simde_mm256_setzero_pd();
      for (; j + 4 <= period; j += 4) {
        simde__m256d v_in = simde_mm256_loadu_pd(&in[i - (period - 1) + j]);
        simde__m256d v_w = simde_mm256_loadu_pd(&w[j]);
        v_val = simde_mm256_add_pd(v_val, simde_mm256_mul_pd(v_in, v_w));
      }
      double tmp[4];
      simde_mm256_storeu_pd(tmp, v_val);
      val = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    }
    for (; j < period; ++j) {
      val += in[i - (period - 1 - j)] * w[j];
    }
    out[i] = val;
  }

  return n;
}

size_t exprtk_ta_vidya(const double *in, size_t n, size_t cmo_p, size_t ema_p, double *out,
                       mem_pool_t *arena) {
  if (n < cmo_p || n < ema_p)
    return 0;
  double *cmo = MEM_ALLOC_ARRAY(arena, double, n);
  if (!cmo)
    return 0;
  exprtk_ta_cmo(in, n, cmo_p, cmo, arena);
  double alpha = 2.0 / (double)(ema_p + 1);
  double val = in[cmo_p - 1];
  for (size_t i = 0; i < n; ++i) {
    if (i < cmo_p - 1) {
      out[i] = 0;
      continue;
    }
    double v = fabs(cmo[i]) / 100.0;
    val = (alpha * v) * in[i] + (1.0 - alpha * v) * val;
    out[i] = val;
  }
  return n;
}

size_t exprtk_ta_rvi(const double *in, size_t n, size_t std_p, size_t ema_p, double *out,
                     mem_pool_t *arena) {
  if (n < std_p + ema_p)
    return 0;
  double *std = MEM_ALLOC_ARRAY(arena, double, n);
  if (!std)
    return 0;
  exprtk_ta_stddev(in, n, std_p, 1.0, std, arena);
  size_t res = exprtk_ta_rsi(std, n, ema_p, out, arena);
  return res;
}

size_t exprtk_ta_vhf(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (n < period)
    return 0;
  double *hh = MEM_ALLOC_ARRAY(arena, double, n);
  double *ll = MEM_ALLOC_ARRAY(arena, double, n);
  if (!hh || !ll) {
    return 0;
  }
  ta_highest_arr(in, n, period, hh, arena);
  ta_lowest_arr(in, n, period, ll, arena);

  double sum_chg = 0;
  for (size_t j = 1; j < period; ++j)
    sum_chg += fabs(in[j] - in[j - 1]);

  for (size_t i = 0; i < n; ++i) {
    if (i < period - 1) {
      out[i] = 0;
      continue;
    }
    if (i >= period)
      sum_chg += fabs(in[i] - in[i - 1]) - fabs(in[i - period + 1] - in[i - period]);
    out[i] = (sum_chg > 1e-15) ? (hh[i] - ll[i]) / sum_chg : 0;
  }
  return n;
}

size_t exprtk_ta_volatility_ratio(const double *hi, const double *lo, const double *cl, size_t n,
                                  size_t period, double *out, mem_pool_t *arena) {
  if (n < period)
    return 0;
  double *atr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = 1;
  for (; i < n; ++i) {
    double tr1 = hi[i] - lo[i];
    double tr2 = fabs(hi[i] - cl[i - 1]);
    double tr3 = fabs(lo[i] - cl[i - 1]);
    double tr = fmax(tr1, fmax(tr2, tr3));
    out[i] = (atr[i] > 1e-15) ? tr / atr[i] : 0;
  }
  return n;
}

size_t exprtk_ta_arbr(const double *hi, const double *lo, const double *op, const double *cl,
                      size_t n, size_t period, double *ar, double *br) {
  if (period == 0 || n < period + 1)
    return 0;
  double sum_ho = 0, sum_ol = 0, sum_hpc = 0, sum_pcl = 0;
  for (size_t i = 0; i < period; i++) {
    sum_ho += (hi[i] - op[i]);
    sum_ol += (op[i] - lo[i]);
    if (i > 0) {
      double pc = cl[i - 1];
      sum_hpc += (hi[i] > pc) ? (hi[i] - pc) : 0;
      sum_pcl += (pc > lo[i]) ? (pc - lo[i]) : 0;
    }
  }
  for (size_t i = 0; i < period - 1; i++) {
    ar[i] = 0;
    br[i] = 0;
  }
  ar[period - 1] = (fabs(sum_ol) > 1e-15) ? (sum_ho / sum_ol) * 100.0 : 0;
  br[period - 1] = (fabs(sum_pcl) > 1e-15) ? (sum_hpc / sum_pcl) * 100.0 : 0;

  /* Sliding window: update AR (H-O, O-L) and BR (H-PC, PC-L).
     BR uses cl[i-1] as "previous close", so removing the oldest bar
     requires cl[i - period - 1]. Safe only when i >= period + 1. */
  for (size_t i = period; i < n; i++) {
    sum_ho += (hi[i] - op[i]) - (hi[i - period] - op[i - period]);
    sum_ol += (op[i] - lo[i]) - (op[i - period] - lo[i - period]);
    double pc = cl[i - 1];
    sum_hpc += (hi[i] > pc) ? (hi[i] - pc) : 0;
    sum_pcl += (pc > lo[i]) ? (pc - lo[i]) : 0;
    if (i >= period + 1) {
      double ppc = cl[i - period - 1];
      sum_hpc -= (hi[i - period] > ppc) ? (hi[i - period] - ppc) : 0;
      sum_pcl -= (ppc > lo[i - period]) ? (ppc - lo[i - period]) : 0;
    }
    ar[i] = (fabs(sum_ol) > 1e-15) ? (sum_ho / sum_ol) * 100.0 : 0;
    br[i] = (fabs(sum_pcl) > 1e-15) ? (sum_hpc / sum_pcl) * 100.0 : 0;
  }
  return n;
}

size_t exprtk_ta_rsrs(const double *hi, const double *lo, size_t n, size_t n_reg, size_t m_z,
                      double *slope, double *zscore, mem_pool_t *arena) {
  if (n < n_reg)
    return 0;
  double *slopes = MEM_ALLOC_ARRAY(arena, double, n);
  if (!slopes)
    return 0;
  memset(slopes, 0, n * sizeof(double));
  for (size_t i = n_reg - 1; i < n; i++) {
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
    for (size_t j = 0; j < n_reg; j++) {
      double x = lo[i - n_reg + 1 + j];
      double y = hi[i - n_reg + 1 + j];
      sum_x += x;
      sum_y += y;
      sum_xx += x * x;
      sum_xy += x * y;
    }
    double den = (double)n_reg * sum_xx - sum_x * sum_x;
    slopes[i] = (fabs(den) > 1e-15) ? ((double)n_reg * sum_xy - sum_x * sum_y) / den : 1.0;
    slope[i] = slopes[i];
  }
  if (n < n_reg + m_z - 1)
    return n;
  for (size_t i = n_reg + m_z - 2; i < n; i++) {
    double sum = 0, sum_sq = 0;
    for (size_t j = 0; j < m_z; j++) {
      double s = slopes[i - m_z + 1 + j];
      sum += s;
      sum_sq += s * s;
    }
    double mean = sum / (double)m_z;
    double var = (sum_sq / (double)m_z) - (mean * mean);
    double std = (var > 1e-15) ? sqrt(var) : 0;
    zscore[i] = (std > 1e-15) ? (slopes[i] - mean) / std : 0;
  }
  return n;
}

size_t exprtk_ta_smart_money(const double *p, const double *v, size_t n, size_t period, double *out,
                             mem_pool_t *arena) {
  if (n < period)
    return 0;
  for (size_t i = period - 1; i < n; i++) {
    const double *sub_p = &p[i - period + 1];
    const double *sub_v = &v[i - period + 1];
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0;
    for (size_t j = 1; j < period; j++) {
      double r = fabs(sub_p[j] / sub_p[j - 1] - 1.0);
      double vol = sub_v[j];
      sum_x += r;
      sum_y += vol;
      sum_xx += r * r;
      sum_yy += vol * vol;
      sum_xy += r * vol;
    }
    size_t m = period - 1;
    double den = sqrt(((double)m * sum_xx - sum_x * sum_x) * ((double)m * sum_yy - sum_y * sum_y));
    out[i] = (fabs(den) > 1e-15) ? ((double)m * sum_xy - sum_x * sum_y) / den : 0;
  }
  return n;
}

size_t exprtk_ta_vmacd_mtm(const double *v, size_t n, size_t period, double *out,
                           mem_pool_t *arena) {
  if (n < 60)
    return 0;
  double *macd = MEM_ALLOC_ARRAY(arena, double, n);
  double *sig = MEM_ALLOC_ARRAY(arena, double, n);
  double *hist = MEM_ALLOC_ARRAY(arena, double, n);
  if (!macd || !sig || !hist)
    return 0;
  exprtk_ta_macd(v, n, 12, 26, 9, macd, sig, hist, arena);
  double *zscore = MEM_ALLOC_ARRAY(arena, double, n);
  if (!zscore)
    return 0;
  memset(zscore, 0, n * sizeof(double));
  for (size_t i = period - 1; i < n; i++) {
    double sum = 0, sum_sq = 0;
    for (size_t j = 0; j < period; j++) {
      double h = hist[i - period + 1 + j];
      sum += h;
      sum_sq += h * h;
    }
    double mean = sum / (double)period;
    double var = (sum_sq / (double)period) - (mean * mean);
    double std = (var > 1e-15) ? sqrt(var) : 1e-15;
    zscore[i] = (hist[i] - mean) / std;
  }
  for (size_t i = period; i < n; i++) {
    if (i >= 2 * period - 1) {
      double roll_sum = 0;
      for (size_t j = 0; j < period; j++) {
        double d = zscore[i - period + 1 + j] - zscore[i - period + j];
        roll_sum += d;
      }
      out[i] = roll_sum;
    }
  }
  return n;
}

size_t exprtk_ta_noise_area(const double *op, const double *cl, size_t n, size_t period,
                            double *upper, double *lower, mem_pool_t *arena) {
  if (n < period)
    return 0;
  double *dist = MEM_ALLOC_ARRAY(arena, double, n);
  if (!dist)
    return 0;
  for (size_t i = 0; i < n; i++)
    dist[i] = (fabs(op[i]) > 1e-15) ? fabs(cl[i] / op[i] - 1.0) : 0;
  for (size_t i = period - 1; i < n; i++) {
    double sum = 0;
    for (size_t j = 0; j < period; j++)
      sum += dist[i - period + 1 + j];
    double sigma = sum / (double)period;
    double ref_o = op[i], ref_pc = (i > 0) ? cl[i - 1] : op[i];
    double u_thr = (ref_o > ref_pc) ? ref_o : ref_pc;
    double l_thr = (ref_o < ref_pc) ? ref_o : ref_pc;
    if (upper)
      upper[i] = u_thr * (1.0 + sigma);
    if (lower)
      lower[i] = l_thr * (1.0 - sigma);
  }
  return n;
}

#ifdef _MSC_VER
  #define WF_THREAD_LOCAL __declspec(thread)
#else
  #define WF_THREAD_LOCAL __thread
#endif

static WF_THREAD_LOCAL const double *s_w_factor_avg_size;

static int w_factor_cmp(const void *a, const void *b) {
  const double *avg_size = s_w_factor_avg_size;
  double va = avg_size[*(const size_t *)a];
  double vb = avg_size[*(const size_t *)b];
  return (va > vb) - (va < vb);
}

size_t exprtk_ta_w_factor(const double *ret, const double *amt, const double *cnt, size_t n,
                          size_t period, double *out, mem_pool_t *arena) {
  if (n < period)
    return 0;
  double *avg_size = MEM_ALLOC_ARRAY(arena, double, n);
  if (!avg_size)
    return 0;
  for (size_t i = 0; i < n; i++)
    avg_size[i] = (cnt[i] > 1e-15) ? amt[i] / cnt[i] : 0;
  size_t *indices = MEM_ALLOC_ARRAY(arena, size_t, period);
  if (!indices)
    return 0;
  for (size_t i = period - 1; i < n; i++) {
    for (size_t j = 0; j < period; j++)
      indices[j] = i - period + 1 + j;
    s_w_factor_avg_size = avg_size;
    qsort(indices, period, sizeof(size_t), w_factor_cmp);
    double m_high = 0, m_low = 0;
    size_t half = period / 2;
    for (size_t j = 0; j < half; j++) {
      m_low += ret[indices[j]];
      m_high += ret[indices[period - 1 - j]];
    }
    out[i] = m_high - m_low;
  }
  return n;
}

size_t exprtk_ta_cpv(const double *ret, const double *vol, size_t n, size_t period, double *out,
                     mem_pool_t *arena) {
  if (n < period * 2)
    return 0;
  double *corrs = MEM_ALLOC_ARRAY(arena, double, n);
  if (!corrs)
    return 0;
  memset(corrs, 0, n * sizeof(double));
  for (size_t i = period - 1; i < n; i++) {
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_yy = 0, sum_xy = 0;
    for (size_t j = 0; j < period; j++) {
      double r = ret[i - period + 1 + j];
      double v = vol[i - period + 1 + j];
      sum_x += r;
      sum_y += v;
      sum_xx += r * r;
      sum_yy += v * v;
      sum_xy += r * v;
    }
    double den =
        sqrt(((double)period * sum_xx - sum_x * sum_x) * ((double)period * sum_yy - sum_y * sum_y));
    corrs[i] = (fabs(den) > 1e-15) ? ((double)period * sum_xy - sum_x * sum_y) / den : 0;
  }
  for (size_t i = 2 * period - 2; i < n; i++) {
    double sum = 0;
    for (size_t j = 0; j < period; j++)
      sum += corrs[i - period + 1 + j];
    out[i] = sum / (double)period;
  }
  return n;
}

size_t exprtk_vec_quantile(const double *in, size_t n, size_t period, double *out,
                           mem_pool_t *arena) {
  (void)arena;
  if (n < period)
    return 0;
  for (size_t i = period - 1; i < n; i++) {
    double current = in[i];
    size_t count = 0;
    for (size_t j = 0; j < period; j++) {
      if (in[i - period + 1 + j] < current)
        count++;
    }
    out[i] = (double)count / (double)period;
  }
  return n;
}

size_t exprtk_ta_qrs(const double *slope, size_t n, size_t period, double *out,
                     mem_pool_t *arena) {
  (void)arena;
  return exprtk_vec_quantile(slope, n, period, out, arena);
}

size_t exprtk_ta_alligator(const double *in, size_t n, double *jaw, double *teeth, double *lips,
                           mem_pool_t *arena) {
  if (n < 21)
    return 0;
  double *tmp_j = MEM_ALLOC_ARRAY(arena, double, n);
  double *tmp_t = MEM_ALLOC_ARRAY(arena, double, n);
  double *tmp_l = MEM_ALLOC_ARRAY(arena, double, n);
  if (!tmp_j || !tmp_t || !tmp_l)
    return 0;
  exprtk_ta_smma(in, n, 13, tmp_j);
  exprtk_ta_smma(in, n, 8, tmp_t);
  exprtk_ta_smma(in, n, 5, tmp_l);
  memset(jaw, 0, n * sizeof(double));
  memset(teeth, 0, n * sizeof(double));
  memset(lips, 0, n * sizeof(double));
  for (size_t i = 8; i < n; i++)
    jaw[i] = tmp_j[i - 8];
  for (size_t i = 5; i < n; i++)
    teeth[i] = tmp_t[i - 5];
  for (size_t i = 3; i < n; i++)
    lips[i] = tmp_l[i - 3];
  return n;
}

size_t exprtk_ta_shadow(const double *op, const double *hi, const double *lo, const double *cl,
                        size_t n, double *upper, double *lower, mem_pool_t *arena) {
  (void)arena;
  for (size_t i = 0; i < n; i++) {
    double real_top = fmax(op[i], cl[i]);
    double real_bot = fmin(op[i], cl[i]);
    if (upper)
      upper[i] = (hi[i] > real_top) ? (hi[i] - real_top) : 0;
    if (lower)
      lower[i] = (real_bot > lo[i]) ? (real_bot - lo[i]) : 0;
  }
  return n;
}
