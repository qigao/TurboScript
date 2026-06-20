/**
 * @file exprtk_ta_indicators_2.c
 * @brief Technical Analysis Volatility, Volume, Trend, Statistics, and Price Transform Indicators.
 */

#include "ta.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <stdlib.h>
#include <string.h>

// TA-Lib Volatility Indicators

size_t exprtk_ta_trange(const double *hi, const double *lo, const double *cl, size_t n,
                        double *out) {
  if (n == 0)
    return 0;
  ta_true_range_arr(hi, lo, cl, n, out);
  return n;
}

size_t exprtk_ta_atr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *tr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!tr)
    return 0;
  ta_true_range_arr(hi, lo, cl, n, tr);
  ta_wilder_smooth(tr, n, period, out);
  return n;
}

size_t exprtk_ta_natr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                      double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *atr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!atr)
    return 0;
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_set1_pd(1e-15); // Use 1e-15 for comparison
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_atr_val = simde_mm256_loadu_pd(&atr[i]);
    simde__m256d v_cl_val = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_cl_val, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_div_pd(simde_mm256_mul_pd(v_atr_val, v_100), v_cl_val);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (cl[i] > 1e-15) ? 100.0 * atr[i] / cl[i] : 0;
  return n;
}

size_t exprtk_ta_bop(const double *o, const double *h, const double *l, const double *c, size_t n,
                     double *out) {
  size_t i = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&h[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&l[i]);
    simde__m256d v_diff = simde_mm256_sub_pd(v_h, v_l);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_diff, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_div_pd(
        simde_mm256_sub_pd(simde_mm256_loadu_pd(&c[i]), simde_mm256_loadu_pd(&o[i])), v_diff);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double diff = h[i] - l[i];
    out[i] = (diff > 0) ? (c[i] - o[i]) / diff : 0;
  }
  return n;
}

// TA-Lib Volume Indicators
size_t exprtk_ta_obv(const double *cl, const double *vol, size_t n, double *out) {
  if (n == 0)
    return 0;
  double obv = 0;
  out[0] = vol[0];
  obv = vol[0];
  size_t i = 1;

  for (; i + 4 <= n; i += 4) {
    simde__m256d v_c1 = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_c0 = simde_mm256_loadu_pd(&cl[i - 1]);
    simde__m256d v_v = simde_mm256_loadu_pd(&vol[i]);

    simde__m256d v_gt = simde_mm256_cmp_pd(v_c1, v_c0, SIMDE_CMP_GT_OQ);
    simde__m256d v_lt = simde_mm256_cmp_pd(v_c1, v_c0, SIMDE_CMP_LT_OQ);

    // sign * vol
    simde__m256d v_inc = simde_mm256_and_pd(v_v, v_gt);               // add vol if gt
    v_inc = simde_mm256_sub_pd(v_inc, simde_mm256_and_pd(v_v, v_lt)); // sub vol if lt

    double incs[4];
    simde_mm256_storeu_pd(incs, v_inc);
    for (int j = 0; j < 4; ++j) {
      obv += incs[j];
      out[i + j] = obv;
    }
  }
  for (; i < n; ++i) {
    if (cl[i] > cl[i - 1])
      obv += vol[i];
    else if (cl[i] < cl[i - 1])
      obv -= vol[i];
    out[i] = obv;
  }
  return n;
}

size_t exprtk_ta_ad(const double *hi, const double *lo, const double *cl, const double *vol,
                    size_t n, double *out) {
  if (n == 0)
    return 0;
  double ad = 0;
  size_t i = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_hi = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_lo = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_cl_val = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_vol_val = simde_mm256_loadu_pd(&vol[i]);

    simde__m256d v_range = simde_mm256_sub_pd(v_hi, v_lo);
    // mask = range > 0
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);

    // mfv = ((cl - lo) - (hi - cl)) / range => (2*cl - lo - hi) / range
    simde__m256d v_num = simde_mm256_sub_pd(
        simde_mm256_sub_pd(simde_mm256_mul_pd(simde_mm256_set1_pd(2.0), v_cl_val), v_lo), v_hi);
    simde__m256d v_mfv = simde_mm256_div_pd(v_num, v_range);
    v_mfv = simde_mm256_and_pd(v_mfv, v_mask); // zero out if range <= 0

    simde__m256d v_inc = simde_mm256_mul_pd(v_mfv, v_vol_val);
    double incs[4];
    simde_mm256_storeu_pd(incs, v_inc);
    for (int j = 0; j < 4; ++j) {
      ad += incs[j];
      out[i + j] = ad;
    }
  }
  for (; i < n; ++i) {
    double range = hi[i] - lo[i];
    double mfv = (range > 0) ? ((cl[i] - lo[i]) - (hi[i] - cl[i])) / range : 0;
    ad += mfv * vol[i];
    out[i] = ad;
  }
  return n;
}

size_t exprtk_ta_adosc(const double *hi, const double *lo, const double *cl, const double *vol,
                       size_t n, size_t fast_p, size_t slow_p, double *out, mem_pool_t *arena) {
  if (fast_p == 0 || slow_p == 0 || n < slow_p)
    return 0;
  double *ad = MEM_ALLOC_ARRAY(arena, double, n);
  if (!ad)
    return 0;
  exprtk_ta_ad(hi, lo, cl, vol, n, ad);
  double *f_ema = MEM_ALLOC_ARRAY(arena, double, n);
  double *s_ema = MEM_ALLOC_ARRAY(arena, double, n);
  if (!f_ema || !s_ema) {
          return 0;
  }
  ta_ema_calc(ad, n, fast_p, f_ema);
  ta_ema_calc(ad, n, slow_p, s_ema);
  size_t i = (size_t)fmax(fast_p, slow_p) - 1;
  for (; i + 4 <= n; i += 4) {
    simde_mm256_storeu_pd(&out[i], simde_mm256_sub_pd(simde_mm256_loadu_pd(&f_ema[i]),
                                                      simde_mm256_loadu_pd(&s_ema[i])));
  }
  for (; i < n; ++i)
    out[i] = f_ema[i] - s_ema[i];
  return n;
}

size_t exprtk_ta_cmf(const double *hi, const double *lo, const double *cl, const double *vol,
                     size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *mfv = MEM_ALLOC_ARRAY(arena, double, n);
  if (!mfv)
    return 0;
  size_t i = 0;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  simde__m256d v_two = simde_mm256_set1_pd(2.0);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_l = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_c = simde_mm256_loadu_pd(&cl[i]);
    simde__m256d v_v = simde_mm256_loadu_pd(&vol[i]);
    simde__m256d v_range = simde_mm256_sub_pd(v_h, v_l);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_range, v_zero, SIMDE_CMP_GT_OQ);
    // ((cl - lo) - (hi - cl)) = 2*cl - hi - lo
    simde__m256d v_num =
        simde_mm256_sub_pd(simde_mm256_sub_pd(simde_mm256_mul_pd(v_two, v_c), v_h), v_l);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_num, v_range), v_v);
    simde_mm256_storeu_pd(&mfv[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double range = hi[i] - lo[i];
    mfv[i] = (range > 0) ? ((cl[i] - lo[i]) - (hi[i] - cl[i])) / range * vol[i] : 0;
  }

  double sum_mfv = 0, sum_vol = 0;
  for (size_t j = 0; j < period; ++j) {
    sum_mfv += mfv[j];
    sum_vol += vol[j];
  }
  out[period - 1] = (sum_vol > 1e-15) ? sum_mfv / sum_vol : 0;

  for (size_t k = period; k < n; ++k) {
    sum_mfv += mfv[k] - mfv[k - period];
    sum_vol += vol[k] - vol[k - period];
    out[k] = (sum_vol > 1e-15) ? sum_mfv / sum_vol : 0;
  }
  return n;
}

size_t exprtk_ta_mfi(const double *hi, const double *lo, const double *cl, const double *vol,
                     size_t n, size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < period + 1)
    return 0;
  double *tp = MEM_ALLOC_ARRAY(arena, double, n);
  if (!tp)
    return 0;
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

  double *pos_mf_inc = MEM_ALLOC_ARRAY(arena, double, n);
  double *neg_mf_inc = MEM_ALLOC_ARRAY(arena, double, n);
  if (!pos_mf_inc || !neg_mf_inc) {
          return 0;
  }

  pos_mf_inc[0] = 0;
  neg_mf_inc[0] = 0;
  i = 1;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_tp_curr = simde_mm256_loadu_pd(&tp[i]);
    simde__m256d v_tp_prev = simde_mm256_loadu_pd(&tp[i - 1]);
    simde__m256d v_vol_val = simde_mm256_loadu_pd(&vol[i]);
    simde__m256d v_mf = simde_mm256_mul_pd(v_tp_curr, v_vol_val);
    simde__m256d v_gt = simde_mm256_cmp_pd(v_tp_curr, v_tp_prev, SIMDE_CMP_GT_OQ);
    simde__m256d v_lt = simde_mm256_cmp_pd(v_tp_curr, v_tp_prev, SIMDE_CMP_LT_OQ);
    simde_mm256_storeu_pd(&pos_mf_inc[i], simde_mm256_and_pd(v_mf, v_gt));
    simde_mm256_storeu_pd(&neg_mf_inc[i], simde_mm256_and_pd(v_mf, v_lt));
  }
  for (; i < n; ++i) {
    double mf = tp[i] * vol[i];
    pos_mf_inc[i] = (tp[i] > tp[i - 1]) ? mf : 0;
    neg_mf_inc[i] = (tp[i] < tp[i - 1]) ? mf : 0;
  }

  double pos_mf = 0, neg_mf = 0;
  for (i = 1; i <= period; ++i) {
    pos_mf += pos_mf_inc[i];
    neg_mf += neg_mf_inc[i];
  }
  out[period] = (pos_mf + neg_mf > 1e-15) ? 100.0 * pos_mf / (pos_mf + neg_mf) : 50.0;

  for (i = period + 1; i < n; ++i) {
    pos_mf += pos_mf_inc[i] - pos_mf_inc[i - period];
    neg_mf += neg_mf_inc[i] - neg_mf_inc[i - period];
    out[i] = (pos_mf + neg_mf > 1e-15) ? 100.0 * pos_mf / (pos_mf + neg_mf) : 50.0;
  }

  return n;
}

// TA-Lib Trend Indicators
size_t exprtk_ta_plus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out,
                         mem_pool_t *arena) {
  if (period == 0 || n < 1)
    return 0;
  double *pdm = MEM_ALLOC_ARRAY(arena, double, n);
  if (!pdm)
    return 0;
  pdm[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h1 = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_h0 = simde_mm256_loadu_pd(&hi[i - 1]);
    simde__m256d v_l1 = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_l0 = simde_mm256_loadu_pd(&lo[i - 1]);

    simde__m256d v_up = simde_mm256_sub_pd(v_h1, v_h0);
    simde__m256d v_dn = simde_mm256_sub_pd(v_l0, v_l1);

    simde__m256d v_mask = simde_mm256_and_pd(simde_mm256_cmp_pd(v_up, v_dn, SIMDE_CMP_GT_OQ),
                                             simde_mm256_cmp_pd(v_up, v_zero, SIMDE_CMP_GT_OQ));
    simde_mm256_storeu_pd(&pdm[i], simde_mm256_and_pd(v_up, v_mask));
  }
  for (; i < n; ++i) {
    double up = hi[i] - hi[i - 1], dn = lo[i - 1] - lo[i];
    pdm[i] = (up > dn && up > 0) ? up : 0;
  }
  ta_wilder_smooth(pdm + 1, n - 1, period, out + 1);
  return n;
}

size_t exprtk_ta_minus_dm(const double *hi, const double *lo, size_t n, size_t period, double *out,
                          mem_pool_t *arena) {
  if (period == 0 || n < 1)
    return 0;
  double *mdm = MEM_ALLOC_ARRAY(arena, double, n);
  if (!mdm)
    return 0;
  mdm[0] = 0;
  size_t i = 1;
  simde__m256d v_zero = simde_mm256_setzero_pd();
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_h1 = simde_mm256_loadu_pd(&hi[i]);
    simde__m256d v_h0 = simde_mm256_loadu_pd(&hi[i - 1]);
    simde__m256d v_l1 = simde_mm256_loadu_pd(&lo[i]);
    simde__m256d v_l0 = simde_mm256_loadu_pd(&lo[i - 1]);

    simde__m256d v_up = simde_mm256_sub_pd(v_h1, v_h0);
    simde__m256d v_dn = simde_mm256_sub_pd(v_l0, v_l1);

    simde__m256d v_mask = simde_mm256_and_pd(simde_mm256_cmp_pd(v_dn, v_up, SIMDE_CMP_GT_OQ),
                                             simde_mm256_cmp_pd(v_dn, v_zero, SIMDE_CMP_GT_OQ));
    simde_mm256_storeu_pd(&mdm[i], simde_mm256_and_pd(v_dn, v_mask));
  }
  for (; i < n; ++i) {
    double up = hi[i] - hi[i - 1], dn = lo[i - 1] - lo[i];
    mdm[i] = (dn > up && dn > 0) ? dn : 0;
  }
  ta_wilder_smooth(mdm + 1, n - 1, period, out + 1);
  return n;
}

size_t exprtk_ta_plus_di(const double *hi, const double *lo, const double *cl, size_t n,
                         size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *pdm = MEM_ALLOC_ARRAY(arena, double, n);
  double *atr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!pdm || !atr) {
        return 0;
  }
  exprtk_ta_plus_dm(hi, lo, n, period, pdm, arena);
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_atr_val = simde_mm256_loadu_pd(&atr[i]);
    simde__m256d v_pdm_val = simde_mm256_loadu_pd(&pdm[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_atr_val, v_epsilon, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_pdm_val, v_atr_val), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (atr[i] > 1e-15) ? 100.0 * pdm[i] / atr[i] : 0;
  return n;
}

size_t exprtk_ta_minus_di(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t period, double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *mdm = MEM_ALLOC_ARRAY(arena, double, n);
  double *atr = MEM_ALLOC_ARRAY(arena, double, n);
  if (!mdm || !atr) {
        return 0;
  }
  exprtk_ta_minus_dm(hi, lo, n, period, mdm, arena);
  exprtk_ta_atr(hi, lo, cl, n, period, atr, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_epsilon = simde_mm256_set1_pd(1e-15);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_atr_val = simde_mm256_loadu_pd(&atr[i]);
    simde__m256d v_mdm_val = simde_mm256_loadu_pd(&mdm[i]);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_atr_val, v_epsilon, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_mdm_val, v_atr_val), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i)
    out[i] = (atr[i] > 1e-15) ? 100.0 * mdm[i] / atr[i] : 0;
  return n;
}

size_t exprtk_ta_dx(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                    double *out, mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *p_di = MEM_ALLOC_ARRAY(arena, double, n);
  double *m_di = MEM_ALLOC_ARRAY(arena, double, n);
  if (!p_di || !m_di) {
    return 0;
  }
  exprtk_ta_plus_di(hi, lo, cl, n, period, p_di, arena);
  exprtk_ta_minus_di(hi, lo, cl, n, period, m_di, arena);
  size_t i = period - 1;
  simde__m256d v_100 = simde_mm256_set1_pd(100.0);
  simde__m256d v_zero = simde_mm256_setzero_pd();
  simde__m256d v_sign_mask = simde_mm256_set1_pd(-0.0); // Mask for clearing sign bit (abs)
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_p_di = simde_mm256_loadu_pd(&p_di[i]);
    simde__m256d v_m_di = simde_mm256_loadu_pd(&m_di[i]);
    simde__m256d v_diff = simde_mm256_andnot_pd(v_sign_mask, simde_mm256_sub_pd(v_p_di, v_m_di));
    simde__m256d v_sum = simde_mm256_add_pd(v_p_di, v_m_di);
    simde__m256d v_mask = simde_mm256_cmp_pd(v_sum, v_zero, SIMDE_CMP_GT_OQ);
    simde__m256d v_res = simde_mm256_mul_pd(simde_mm256_div_pd(v_diff, v_sum), v_100);
    simde_mm256_storeu_pd(&out[i], simde_mm256_and_pd(v_res, v_mask));
  }
  for (; i < n; ++i) {
    double diff = fabs(p_di[i] - m_di[i]), sum = p_di[i] + m_di[i];
    out[i] = (sum > 0) ? 100.0 * diff / sum : 0;
  }
  return n;
}

size_t exprtk_ta_adx(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                     double *out, mem_pool_t *arena) {
  if (period == 0 || n < 2 * period)
    return 0;
  double *dx = MEM_ALLOC_ARRAY(arena, double, n);
  if (!dx)
    return 0;
  exprtk_ta_dx(hi, lo, cl, n, period, dx, arena);
  ta_wilder_smooth(dx + period - 1, n - (period - 1), period, out + period - 1);
  return n;
}

size_t exprtk_ta_adxr(const double *hi, const double *lo, const double *cl, size_t n, size_t period,
                      double *out, mem_pool_t *arena) {
  if (period == 0 || n < 2 * period)
    return 0; // Changed condition to 2*period as per ADX requirement
  exprtk_ta_adx(hi, lo, cl, n, period, out, arena);
  size_t i = 2 * period - 2; // First valid index for ADXR
  simde__m256d v_05 = simde_mm256_set1_pd(0.5);
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_curr = simde_mm256_loadu_pd(&out[i]);
    simde__m256d v_prev = simde_mm256_loadu_pd(&out[i - (period - 1)]);
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(simde_mm256_add_pd(v_curr, v_prev), v_05));
  }
  for (; i < n; ++i) {
    out[i] = (out[i] + out[i - (period - 1)]) / 2.0;
  }
  return n;
}

// TA-Lib Statistics Indicators
size_t exprtk_ta_stddev(const double *in, size_t n, size_t period, double mult, double *out,
                        mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *s1 = MEM_ALLOC_ARRAY(arena, double, n + 1);
  double *s2 = MEM_ALLOC_ARRAY(arena, double, n + 1);
  if (!s1 || !s2) {
    return 0;
  }

  s1[0] = 0;
  s2[0] = 0;
  for (size_t i = 0; i < n; ++i) {
    s1[i + 1] = s1[i] + in[i];
    s2[i + 1] = s2[i] + in[i] * in[i];
  }

  double inv_p = 1.0 / (double)period;
  simde__m256d v_inv_p = simde_mm256_set1_pd(inv_p);
  simde__m256d v_mult = simde_mm256_set1_pd(mult);
  simde__m256d v_zero = simde_mm256_setzero_pd();

  size_t i = period - 1;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_s1_curr = simde_mm256_loadu_pd(&s1[i + 1]);
    simde__m256d v_s1_prev = simde_mm256_loadu_pd(&s1[i + 1 - period]);
    simde__m256d v_s2_curr = simde_mm256_loadu_pd(&s2[i + 1]);
    simde__m256d v_s2_prev = simde_mm256_loadu_pd(&s2[i + 1 - period]);

    simde__m256d v_sum = simde_mm256_sub_pd(v_s1_curr, v_s1_prev);
    simde__m256d v_sum2 = simde_mm256_sub_pd(v_s2_curr, v_s2_prev);

    simde__m256d v_var = simde_mm256_mul_pd(
        simde_mm256_sub_pd(v_sum2, simde_mm256_mul_pd(simde_mm256_mul_pd(v_sum, v_sum), v_inv_p)),
        v_inv_p);
    simde__m256d v_res =
        simde_mm256_mul_pd(v_mult, simde_mm256_sqrt_pd(simde_mm256_max_pd(v_zero, v_var)));
    simde_mm256_storeu_pd(&out[i], v_res);
  }
  for (; i < n; ++i) {
    double sum = s1[i + 1] - s1[i + 1 - period];
    double sum2 = s2[i + 1] - s2[i + 1 - period];
    double var = (sum2 - (sum * sum) * inv_p) * inv_p;
    out[i] = mult * sqrt(fmax(0, var));
  }

  return n;
}

size_t exprtk_ta_var(const double *in, size_t n, size_t period, double mult, double *out,
                     mem_pool_t *arena) {
  if (period == 0 || n < period)
    return 0;
  double *s1 = MEM_ALLOC_ARRAY(arena, double, n + 1);
  double *s2 = MEM_ALLOC_ARRAY(arena, double, n + 1);
  if (!s1 || !s2) {
    return 0;
  }

  s1[0] = 0;
  s2[0] = 0;
  for (size_t i = 0; i < n; ++i) {
    s1[i + 1] = s1[i] + in[i];
    s2[i + 1] = s2[i] + in[i] * in[i];
  }

  double inv_p = 1.0 / (double)period;
  simde__m256d v_inv_p = simde_mm256_set1_pd(inv_p);
  simde__m256d v_mult = simde_mm256_set1_pd(mult);
  simde__m256d v_zero = simde_mm256_setzero_pd();

  size_t i = period - 1;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v_s1_curr = simde_mm256_loadu_pd(&s1[i + 1]);
    simde__m256d v_s1_prev = simde_mm256_loadu_pd(&s1[i + 1 - period]);
    simde__m256d v_s2_curr = simde_mm256_loadu_pd(&s2[i + 1]);
    simde__m256d v_s2_prev = simde_mm256_loadu_pd(&s2[i + 1 - period]);

    simde__m256d v_sum = simde_mm256_sub_pd(v_s1_curr, v_s1_prev);
    simde__m256d v_sum2 = simde_mm256_sub_pd(v_s2_curr, v_s2_prev);

    simde__m256d v_var = simde_mm256_mul_pd(
        simde_mm256_sub_pd(v_sum2, simde_mm256_mul_pd(simde_mm256_mul_pd(v_sum, v_sum), v_inv_p)),
        v_inv_p);
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v_mult, simde_mm256_max_pd(v_zero, v_var)));
  }
  for (; i < n; ++i) {
    double sum = s1[i + 1] - s1[i + 1 - period];
    double sum2 = s2[i + 1] - s2[i + 1 - period];
    double var = (sum2 - (sum * sum) * inv_p) * inv_p;
    out[i] = mult * fmax(0, var);
  }

  return n;
}

size_t exprtk_ta_linearreg(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  double m = ((double)period * sum_xy - sum_x * sum_y) / denom;
  double b = (sum_y - m * sum_x) / (double)period;
  out[period - 1] = m * (double)(period - 1) + b;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    m = ((double)period * sum_xy - sum_x * sum_y) / denom;
    b = (sum_y - m * sum_x) / (double)period;
    out[i] = m * (double)(period - 1) + b;
  }
  return n;
}

size_t exprtk_ta_linearreg_slope(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  out[period - 1] = ((double)period * sum_xy - sum_x * sum_y) / denom;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    out[i] = ((double)period * sum_xy - sum_x * sum_y) / denom;
  }
  return n;
}

size_t exprtk_ta_linearreg_intercept(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  double m = ((double)period * sum_xy - sum_x * sum_y) / denom;
  out[period - 1] = (sum_y - m * sum_x) / (double)period;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    m = ((double)period * sum_xy - sum_x * sum_y) / denom;
    out[i] = (sum_y - m * sum_x) / (double)period;
  }
  return n;
}

size_t exprtk_ta_linearreg_angle(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || n < period)
    return 0;
  exprtk_ta_linearreg_slope(in, n, period, out);
  size_t i = period - 1;
  double scale = 180.0 / M_PI;
  for (; i < n; ++i) {
    out[i] = atan(out[i]) * scale;
  }
  return n;
}

size_t exprtk_ta_tsf(const double *in, size_t n, size_t period, double *out) {
  if (period < 2 || n < period)
    return 0;
  double sum_y = 0, sum_xy = 0;
  double sum_x = (double)period * (period - 1) / 2.0;
  double sum_x2 = (double)(period - 1) * period * (2 * period - 1) / 6.0;
  double denom = (double)period * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15)
    return 0;

  for (size_t i = 0; i < period; ++i) {
    sum_y += in[i];
    sum_xy += (double)i * in[i];
  }
  double m = ((double)period * sum_xy - sum_x * sum_y) / denom;
  double b = (sum_y - m * sum_x) / (double)period;
  out[period - 1] = m * (double)period + b;

  for (size_t i = period; i < n; ++i) {
    double old_val = in[i - period];
    sum_xy = sum_xy - sum_y + old_val + (double)(period - 1) * in[i];
    sum_y = sum_y - old_val + in[i];
    m = ((double)period * sum_xy - sum_x * sum_y) / denom;
    b = (sum_y - m * sum_x) / (double)period;
    out[i] = m * (double)period + b;
  }
  return n;
}

size_t exprtk_ta_beta(const double *in0, const double *in1, size_t n, size_t period, double *out) {
  if (period == 0 || n < period)
    return 0;
  double sum0 = 0, sum1 = 0, sum11 = 0, sum01 = 0;
  for (size_t i = 0; i < period; ++i) {
    sum0 += in0[i];
    sum1 += in1[i];
    sum11 += in1[i] * in1[i];
    sum01 += in0[i] * in1[i];
  }
  double inv_p = 1.0 / (double)period;
  double s11 = sum11 - (sum1 * sum1) * inv_p;
  double s01 = sum01 - (sum0 * sum1) * inv_p;
  out[period - 1] = (s11 > 1e-15) ? s01 / s11 : 0;

  for (size_t i = period; i < n; ++i) {
    double o0 = in0[i - period], o1 = in1[i - period];
    sum0 += in0[i] - o0;
    sum1 += in1[i] - o1;
    sum11 += in1[i] * in1[i] - o1 * o1;
    sum01 += in0[i] * in1[i] - o0 * o1;
    s11 = sum11 - (sum1 * sum1) * inv_p;
    s01 = sum01 - (sum0 * sum1) * inv_p;
    out[i] = (s11 > 1e-15) ? s01 / s11 : 0;
  }
  return n;
}

size_t exprtk_ta_correl(const double *in0, const double *in1, size_t n, size_t period,
                        double *out) {
  if (period == 0 || n < period)
    return 0;
  double sum0 = 0, sum1 = 0, sum00 = 0, sum11 = 0, sum01 = 0;
  for (size_t i = 0; i < period; ++i) {
    sum0 += in0[i];
    sum1 += in1[i];
    sum00 += in0[i] * in0[i];
    sum11 += in1[i] * in1[i];
    sum01 += in0[i] * in1[i];
  }
  double inv_p = 1.0 / (double)period;
  double s00 = sum00 - (sum0 * sum0) * inv_p;
  double s11 = sum11 - (sum1 * sum1) * inv_p;
  double s01 = sum01 - (sum0 * sum1) * inv_p;
  double den = sqrt(fmax(0, s00 * s11));
  out[period - 1] = (den > 1e-15) ? s01 / den : 0;

  for (size_t i = period; i < n; ++i) {
    double o0 = in0[i - period], o1 = in1[i - period];
    sum0 += in0[i] - o0;
    sum1 += in1[i] - o1;
    sum00 += in0[i] * in0[i] - o0 * o0;
    sum11 += in1[i] * in1[i] - o1 * o1;
    sum01 += in0[i] * in1[i] - o0 * o1;
    s00 = sum00 - (sum0 * sum0) * inv_p;
    s11 = sum11 - (sum1 * sum1) * inv_p;
    s01 = sum01 - (sum0 * sum1) * inv_p;
    den = sqrt(fmax(0, s00 * s11));
    out[i] = (den > 1e-15) ? s01 / den : 0;
  }
  return n;
}

// TA-Lib Price Transform Indicators
size_t exprtk_ta_avgprice(const double *o, const double *h, const double *l, const double *c,
                          size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&o[i]), simde_mm256_loadu_pd(&h[i]));
    v = simde_mm256_add_pd(v, simde_mm256_loadu_pd(&l[i]));
    v = simde_mm256_add_pd(v, simde_mm256_loadu_pd(&c[i]));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(0.25)));
  }
  for (; i < n; ++i)
    out[i] = (o[i] + h[i] + l[i] + c[i]) * 0.25;
  return n;
}

size_t exprtk_ta_medprice(const double *h, const double *l, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&h[i]), simde_mm256_loadu_pd(&l[i]));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(0.5)));
  }
  for (; i < n; ++i)
    out[i] = (h[i] + l[i]) * 0.5;
  return n;
}

size_t exprtk_ta_typprice(const double *h, const double *l, const double *c, size_t n,
                          double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&h[i]), simde_mm256_loadu_pd(&l[i]));
    v = simde_mm256_add_pd(v, simde_mm256_loadu_pd(&c[i]));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(1.0 / 3.0)));
  }
  for (; i < n; ++i)
    out[i] = (h[i] + l[i] + c[i]) / 3.0;
  return n;
}

size_t exprtk_ta_wclprice(const double *h, const double *l, const double *c, size_t n,
                          double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d v = simde_mm256_add_pd(simde_mm256_loadu_pd(&h[i]), simde_mm256_loadu_pd(&l[i]));
    v = simde_mm256_add_pd(
        v, simde_mm256_mul_pd(simde_mm256_loadu_pd(&c[i]), simde_mm256_set1_pd(2.0)));
    simde_mm256_storeu_pd(&out[i], simde_mm256_mul_pd(v, simde_mm256_set1_pd(0.25)));
  }
  for (; i < n; ++i)
    out[i] = (h[i] + l[i] + 2.0 * c[i]) / 4.0;
  return n;
}
