/**
 * @file exprtk_ta_options.c
 * @brief Technical Analysis Options Pricing Indicators.
 */

#include "ta.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <stdlib.h>
#include <string.h>

// TA-Lib Options Pricing
size_t exprtk_ta_bsm_call(const double *S, const double *K, const double *T, const double *r,
                          const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = fmax(0, S[i] - K[i]);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    out[i] = S[i] * ta_ncdf(d1) - K[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
  }
  return n;
}

size_t exprtk_ta_bsm_put(const double *S, const double *K, const double *T, const double *r,
                         const double *sigma, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = fmax(0, K[i] - S[i]);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    out[i] = K[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2) - S[i] * ta_ncdf(-d1);
  }
  return n;
}

size_t exprtk_ta_bsm_delta_call(const double *S, const double *K, const double *T, const double *r,
                                const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = (S[i] > K[i] ? 1.0 : 0.0);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    out[i] = ta_ncdf(d1);
  }
  return n;
}

size_t exprtk_ta_bsm_delta_put(const double *S, const double *K, const double *T, const double *r,
                               const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = (S_arr[j] < K_arr[j] ? -1.0 : 0.0);
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        out_arr[j] = ta_ncdf(d1_arr[j]) - 1.0;
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = (S[i] < K[i] ? -1.0 : 0.0);
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    out[i] = ta_ncdf(d1) - 1.0;
  }
  return n;
}

size_t exprtk_ta_bsm_gamma(const double *S, const double *K, const double *T, const double *r,
                           const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        out_arr[j] = ta_npdf(d1_arr[j]) / (S_arr[j] * vol_sqrt_t_arr[j]);
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    out[i] = ta_npdf(d1) / (S[i] * vol_sqrt_t);
  }
  return n;
}

size_t exprtk_ta_bsm_theta_call(const double *S, const double *K, const double *T, const double *r,
                                const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4], d2_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        d2_arr[j] = d1_arr[j] - vol_sqrt_t_arr[j];
        double term1 = -(S_arr[j] * ta_npdf(d1_arr[j]) * sigma_arr[j]) / (2.0 * sqrt(T_arr[j]));
        double term2 = r_arr[j] * K_arr[j] * exp(-r_arr[j] * T_arr[j]) * ta_ncdf(d2_arr[j]);
        out_arr[j] = term1 - term2;
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    double term1 = -(S[i] * ta_npdf(d1) * sigma[i]) / (2.0 * sqrt(T[i]));
    double term2 = r[i] * K[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
    out[i] = term1 - term2;
  }
  return n;
}

size_t exprtk_ta_bsm_theta_put(const double *S, const double *K, const double *T, const double *r,
                               const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double vol_sqrt_t_arr[4], d1_arr[4], d2_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        vol_sqrt_t_arr[j] = sigma_arr[j] * sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            vol_sqrt_t_arr[j];
        d2_arr[j] = d1_arr[j] - vol_sqrt_t_arr[j];
        double term1 = -(S_arr[j] * ta_npdf(d1_arr[j]) * sigma_arr[j]) / (2.0 * sqrt(T_arr[j]));
        double term2 = r_arr[j] * K_arr[j] * exp(-r_arr[j] * T_arr[j]) * ta_ncdf(-d2_arr[j]);
        out_arr[j] = term1 + term2;
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double vol_sqrt_t = sigma[i] * sqrt(T[i]);
    double d1 = (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / vol_sqrt_t;
    double d2 = d1 - vol_sqrt_t;
    double term1 = -(S[i] * ta_npdf(d1) * sigma[i]) / (2.0 * sqrt(T[i]));
    double term2 = r[i] * K[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2);
    out[i] = term1 + term2;
  }
  return n;
}

size_t exprtk_ta_bsm_vega(const double *S, const double *K, const double *T, const double *r,
                          const double *sigma, size_t n, double *out) {
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    simde__m256d S_v = simde_mm256_loadu_pd(&S[i]);
    simde__m256d K_v = simde_mm256_loadu_pd(&K[i]);
    simde__m256d T_v = simde_mm256_loadu_pd(&T[i]);
    simde__m256d r_v = simde_mm256_loadu_pd(&r[i]);
    simde__m256d sigma_v = simde_mm256_loadu_pd(&sigma[i]);

    double S_arr[4], K_arr[4], T_arr[4], r_arr[4], sigma_arr[4];
    double sqrt_t_arr[4], d1_arr[4];
    double out_arr[4];

    simde_mm256_storeu_pd(S_arr, S_v);
    simde_mm256_storeu_pd(K_arr, K_v);
    simde_mm256_storeu_pd(T_arr, T_v);
    simde_mm256_storeu_pd(r_arr, r_v);
    simde_mm256_storeu_pd(sigma_arr, sigma_v);

    for (int j = 0; j < 4; ++j) {
      if (T_arr[j] <= 0 || sigma_arr[j] <= 0) {
        out_arr[j] = 0;
      } else {
        sqrt_t_arr[j] = sqrt(T_arr[j]);
        d1_arr[j] =
            (log(S_arr[j] / K_arr[j]) + (r_arr[j] + 0.5 * sigma_arr[j] * sigma_arr[j]) * T_arr[j]) /
            (sigma_arr[j] * sqrt_t_arr[j]);
        out_arr[j] = S_arr[j] * sqrt_t_arr[j] * ta_npdf(d1_arr[j]);
      }
    }
    simde_mm256_storeu_pd(&out[i], simde_mm256_loadu_pd(out_arr));
  }
  for (; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double sqrt_t = sqrt(T[i]);
    double d1 =
        (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt_t);
    out[i] = S[i] * sqrt_t * ta_npdf(d1);
  }
  return n;
}

size_t exprtk_ta_bsm_rho_call(const double *S, const double *K, const double *T, const double *r,
                              const double *sigma, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double d1 =
        (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
    double d2 = d1 - sigma[i] * sqrt(T[i]);
    out[i] = K[i] * T[i] * exp(-r[i] * T[i]) * ta_ncdf(d2);
  }
  return n;
}

size_t exprtk_ta_bsm_rho_put(const double *S, const double *K, const double *T, const double *r,
                             const double *sigma, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    if (T[i] <= 0 || sigma[i] <= 0) {
      out[i] = 0;
      continue;
    }
    double d1 =
        (log(S[i] / K[i]) + (r[i] + 0.5 * sigma[i] * sigma[i]) * T[i]) / (sigma[i] * sqrt(T[i]));
    double d2 = d1 - sigma[i] * sqrt(T[i]);
    out[i] = -K[i] * T[i] * exp(-r[i] * T[i]) * ta_ncdf(-d2);
  }
  return n;
}

size_t exprtk_ta_bsm_iv_call(const double *price, const double *S, const double *K, const double *T,
                             const double *r, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    double v = 0.5;
    for (int iter = 0; iter < 100; ++iter) {
      double c, vega;
      exprtk_ta_bsm_call(&S[i], &K[i], &T[i], &r[i], &v, 1, &c);
      exprtk_ta_bsm_vega(&S[i], &K[i], &T[i], &r[i], &v, 1, &vega);
      double diff = c - price[i];
      if (fabs(diff) < 1e-8 || fabs(vega) < 1e-12)
        break;
      v -= diff / vega;
      if (v < 0)
        v = 1e-6;
    }
    out[i] = v;
  }
  return n;
}

size_t exprtk_ta_bsm_iv_put(const double *price, const double *S, const double *K, const double *T,
                            const double *r, size_t n, double *out) {
  for (size_t i = 0; i < n; ++i) {
    double v = 0.5;
    for (int iter = 0; iter < 100; ++iter) {
      double p, vega;
      exprtk_ta_bsm_put(&S[i], &K[i], &T[i], &r[i], &v, 1, &p);
      exprtk_ta_bsm_vega(&S[i], &K[i], &T[i], &r[i], &v, 1, &vega);
      double diff = p - price[i];
      if (fabs(diff) < 1e-8 || fabs(vega) < 1e-12)
        break;
      v -= diff / vega;
      if (v < 0)
        v = 1e-6;
    }
    out[i] = v;
  }
  return n;
}

size_t exprtk_ta_opt_binomial(double S, double K, double T, double r, double sigma, size_t steps,
                              int is_call, double *out, mem_pool_t *arena) {
  if (steps == 0 || T <= 0 || sigma <= 0) {
    *out = is_call ? fmax(0, S - K) : fmax(0, K - S);
    return 1;
  }
  double dt = T / (double)steps;
  double u = exp(sigma * sqrt(dt)), d = 1.0 / u;
  double p = (exp(r * dt) - d) / (u - d);
  double *V = MEM_ALLOC_ARRAY(arena, double, steps + 1);
  if (!V)
    return 0;
  for (size_t i = 0; i <= steps; ++i) {
    double St = S * pow(u, (double)(steps - i)) * pow(d, (double)i);
    V[i] = is_call ? fmax(0, St - K) : fmax(0, K - St);
  }
  for (size_t j = steps; j > 0; --j) {
    for (size_t i = 0; i < j; ++i)
      V[i] = exp(-r * dt) * (p * V[i] + (1.0 - p) * V[i + 1]);
  }
  *out = V[0];
  return 1;
}
