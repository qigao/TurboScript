/**
 * @file exprtk_ta_math.c
 * @brief Technical Analysis internal math and calculation helpers.
 */

#include "simd_helpers.h"
#include "ta.h"
#include <math.h> 
#include <stdlib.h>
#include <string.h>
#include <turbo_buffer.h>

void ta_sma_calc(const double *src, size_t len, size_t period, double *dst) {
  if (period == 0 || period > len)
    return;
  double sum = simd_sum(src, period);
  dst[period - 1] = sum / (double)period;
  for (size_t i = period; i < len; ++i) {
    sum += src[i] - src[i - period];
    dst[i] = sum / (double)period;
  }
}

void ta_ema_calc(const double *src, size_t len, size_t period, double *dst) {
  if (len == 0 || period == 0 || period > len)
    return;
  if (period == 1) {
    memcpy(dst, src, len * sizeof(double));
    return;
  }

  memset(dst, 0, (period - 1) * sizeof(double));
  double seed = simd_sum(src, period) / (double)period;
  double alpha = 2.0 / (double)(period + 1);
  dst[period - 1] = seed;
  for (size_t i = period; i < len; ++i)
    dst[i] = alpha * src[i] + (1.0 - alpha) * dst[i - 1];
}

void ta_wma_calc(const double *in, size_t n, size_t period, double *out) {
  if (period == 0 || period > n)
    return;
  double weight_sum = (double)(period * (period + 1)) / 2.0;
  double sum_w = 0, sum_v = 0;
  for (size_t i = 0; i < period; ++i) {
    sum_w += in[i] * (double)(i + 1);
    sum_v += in[i];
  }
  for (size_t i = 0; i < period - 1; ++i)
    out[i] = 0;
  out[period - 1] = sum_w / weight_sum;
  for (size_t i = period; i < n; ++i) {
    sum_w = sum_w + (double)period * in[i] - sum_v;
    sum_v = sum_v + in[i] - in[i - period];
    out[i] = sum_w / weight_sum;
  }
}

void ta_wilder_smooth(const double *src, size_t len, size_t period, double *dst) {
  if (period == 0 || period > len)
    return;
  double sum = 0;
  for (size_t i = 0; i < period; ++i)
    sum += src[i];
  dst[period - 1] = sum / (double)period;
  for (size_t i = period; i < len; ++i)
    dst[i] = (dst[i - 1] * (double)(period - 1) + src[i]) / (double)period;
}

void ta_smma(const double *in, size_t n, size_t period, double *out) {
  if (n < period)
    return;
  double sum = 0;
  for (size_t i = 0; i < period; i++)
    sum += in[i];
  out[period - 1] = sum / (double)period;
  for (size_t i = period; i < n; i++) {
    out[i] = (out[i - 1] * (double)(period - 1) + in[i]) / (double)period;
  }
}

double ta_highest(const double *src, size_t idx, size_t period) {
  if (period == 0)
    return 0;
  return simd_max(&src[idx - period + 1], period);
}

double ta_lowest(const double *src, size_t idx, size_t period) {
  if (period == 0)
    return 0;
  return simd_min(&src[idx - period + 1], period);
}

void ta_highest_arr(const double *src, size_t n, size_t period, double *dst, mem_pool_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = MEM_ALLOC_ARRAY(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] >= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (i >= period && deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = src[deque[head]];
  }
}

void ta_lowest_arr(const double *src, size_t n, size_t period, double *dst, mem_pool_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = MEM_ALLOC_ARRAY(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] <= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (i >= period && deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = src[deque[head]];
  }
}

void ta_highest_idx_arr(const double *src, size_t n, size_t period, size_t *dst,
                        mem_pool_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = MEM_ALLOC_ARRAY(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] >= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = deque[head];
  }
}

void ta_lowest_idx_arr(const double *src, size_t n, size_t period, size_t *dst,
                       mem_pool_t *arena) {
  if (period == 0 || period > n)
    return;
  size_t *deque = MEM_ALLOC_ARRAY(arena, size_t, n);
  if (!deque)
    return;
  size_t head = 0, tail = 0;
  for (size_t i = 0; i < n; ++i) {
    while (tail > head && src[i] <= src[deque[tail - 1]])
      tail--;
    deque[tail++] = i;
    if (deque[head] <= i - period)
      head++;
    if (i >= period - 1)
      dst[i] = deque[head];
  }
}

size_t ta_highest_idx(const double *src, size_t idx, size_t period) {
  size_t hi = idx;
  for (size_t j = 1; j < period; ++j)
    if (src[idx - j] > src[hi])
      hi = idx - j;
  return hi;
}

size_t ta_lowest_idx(const double *src, size_t idx, size_t period) {
  size_t lo = idx;
  for (size_t j = 1; j < period; ++j)
    if (src[idx - j] < src[lo])
      lo = idx - j;
  return lo;
}

double ta_true_range(double high, double low, double prev_close) {
  double hl = high - low;
  double hc = fabs(high - prev_close);
  double lc = fabs(low - prev_close);
  return fmax(hl, fmax(hc, lc));
}

void ta_true_range_arr(const double *high, const double *low, const double *close, size_t len,
                       double *dst) {
  if (len == 0)
    return;
  dst[0] = high[0] - low[0];
  for (size_t i = 1; i < len; ++i) {
    double hl = high[i] - low[i];
    double hc = fabs(high[i] - close[i - 1]);
    double lc = fabs(low[i] - close[i - 1]);
    dst[i] = fmax(hl, fmax(hc, lc));
  }
}

void ta_linreg(const double *src, size_t end_idx, size_t period, double *slope, double *intercept) {
  double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
  double n = (double)period;
  for (size_t j = 0; j < period; ++j) {
    double x = (double)j;
    double y = src[end_idx - period + 1 + j];
    sum_x += x;
    sum_y += y;
    sum_xy += x * y;
    sum_x2 += x * x;
  }
  double denom = n * sum_x2 - sum_x * sum_x;
  if (fabs(denom) < 1e-15) {
    *slope = 0;
    *intercept = sum_y / n;
  } else {
    *slope = (n * sum_xy - sum_x * sum_y) / denom;
    *intercept = (sum_y - (*slope) * sum_x) / n;
  }
}

double ta_ncdf(double x) {
  static const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741, a4 = -1.453152027,
                      a5 = 1.061405429, p = 0.3275911;
  double sign = (x < 0) ? -1.0 : 1.0;
  x = fabs(x) / sqrt(2.0);
  double t = 1.0 / (1.0 + p * x);
  double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-x * x);
  return 0.5 * (1.0 + sign * y);
}

double ta_npdf(double x) { return 0.3989422804014327 * exp(-0.5 * x * x); }
