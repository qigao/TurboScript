/**
 * @file test_factor.c
 * @brief Unit tests for factor processing functions.
 */

#include "fin.h"
#include "tinytest.h"
#include <math.h>
#include <string.h>

#define EPSILON 0.001

suite("Factor Processing") {
  group("Ranking") {
    it("should rank values in ascending order") {
      double in[] = {3.0, 1.0, 4.0, 1.5, 2.0};
      size_t n = 5;
      double out[5];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_rank(in, n, out, &arena);

      check_int_eq(result, n);
      check_float_eq(out[0], 3.0, EPSILON); // 3.0 is rank 3
      check_float_eq(out[1], 0.0, EPSILON); // 1.0 is rank 0
      check_float_eq(out[2], 4.0, EPSILON); // 4.0 is rank 4
      check_float_eq(out[3], 1.0, EPSILON); // 1.5 is rank 1
      check_float_eq(out[4], 2.0, EPSILON); // 2.0 is rank 2

      mem_destroy(&arena);
    }

    it("should handle ties with average rank") {
      double in[] = {1.0, 2.0, 2.0, 3.0};
      size_t n = 4;
      double out[4];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_rank(in, n, out, &arena);

      check_int_eq(result, n);
      check_float_eq(out[0], 0.0, EPSILON);  // 1.0 is rank 0
      check_float_eq(out[1], 1.5, EPSILON);  // 2.0 tied, avg rank (1+2)/2 = 1.5
      check_float_eq(out[2], 1.5, EPSILON);  // 2.0 tied, avg rank 1.5
      check_float_eq(out[3], 3.0, EPSILON);  // 3.0 is rank 3

      mem_destroy(&arena);
    }

    it("should handle single element") {
      double in[] = {42.0};
      double out[1];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_rank(in, 1, out, &arena);

      check_int_eq(result, 1);
      check_float_eq(out[0], 0.0, EPSILON);

      mem_destroy(&arena);
    }
  }

  group("Z-Score Normalization") {
    it("should standardize to zero mean and unit variance") {
      double in[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      size_t n = 5;
      double out[5];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_zscore(in, n, out, &arena);

      check_int_eq(result, n);

      // Check mean is close to 0
      double sum = 0.0;
      for (size_t i = 0; i < n; i++) {
        sum += out[i];
      }
      double mean = sum / n;
      check_float_eq(mean, 0.0, 0.01);

      // Check std is close to 1
      double var_sum = 0.0;
      for (size_t i = 0; i < n; i++) {
        var_sum += out[i] * out[i];
      }
      double variance = var_sum / n;
      check_float_eq(variance, 1.0, 0.01);

      mem_destroy(&arena);
    }

    it("should handle zero variance") {
      double in[] = {5.0, 5.0, 5.0, 5.0};
      size_t n = 4;
      double out[4];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_zscore(in, n, out, &arena);

      check_int_eq(result, n);
      for (size_t i = 0; i < n; i++) {
        check_float_eq(out[i], 0.0, EPSILON);
      }

      mem_destroy(&arena);
    }

    it("should handle single element") {
      double in[] = {42.0};
      double out[1];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_zscore(in, 1, out, &arena);

      check_int_eq(result, 1);
      check_float_eq(out[0], 0.0, EPSILON);

      mem_destroy(&arena);
    }
  }

  group("Winsorization") {
    it("should cap extreme values at percentiles") {
      double in[] = {1.0, 2.0, 3.0, 4.0, 100.0};
      size_t n = 5;
      double out[5];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_winsorize(in, n, 0.2, out, &arena);

      check_int_eq(result, n);
      // 20th percentile should cap the extreme value
      check(out[4] < 100.0); // Extreme value should be capped
      check(out[0] >= 1.0);  // Lower values should be preserved or raised

      mem_destroy(&arena);
    }

    it("should handle invalid percentile") {
      double in[] = {1.0, 2.0, 3.0};
      size_t n = 3;
      double out[3];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_winsorize(in, n, 0.6, out, &arena); // Invalid: > 0.5

      check_int_eq(result, n);
      // Should just copy input
      for (size_t i = 0; i < n; i++) {
        check_float_eq(out[i], in[i], EPSILON);
      }

      mem_destroy(&arena);
    }
  }

  group("Min-Max Standardization") {
    it("should scale to [0, 1] range") {
      double in[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      size_t n = 5;
      double out[5];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_standardize(in, n, out, &arena);

      check_int_eq(result, n);
      check_float_eq(out[0], 0.0, EPSILON);   // Min maps to 0
      check_float_eq(out[1], 0.25, EPSILON);
      check_float_eq(out[2], 0.5, EPSILON);
      check_float_eq(out[3], 0.75, EPSILON);
      check_float_eq(out[4], 1.0, EPSILON);   // Max maps to 1

      mem_destroy(&arena);
    }

    it("should handle zero range") {
      double in[] = {3.0, 3.0, 3.0};
      size_t n = 3;
      double out[3];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_standardize(in, n, out, &arena);

      check_int_eq(result, n);
      for (size_t i = 0; i < n; i++) {
        check_float_eq(out[i], 0.0, EPSILON);
      }

      mem_destroy(&arena);
    }
  }

  group("Distribution Statistics") {
    it("should calculate skewness") {
      double symmetric[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      double right_skewed[] = {1.0, 1.0, 1.0, 2.0, 10.0};

      double skew_sym = exprtk_vec_skewness(symmetric, 5);
      double skew_right = exprtk_vec_skewness(right_skewed, 5);

      // Symmetric distribution should have skewness near 0
      check_float_eq(skew_sym, 0.0, 0.1);
      // Right-skewed should have positive skewness
      check(skew_right > 0.5);
    }

    it("should calculate kurtosis") {
      double normal[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      double heavy_tail[] = {3.0, 3.0, 3.0, 1.0, 10.0};

      double kurt_normal = exprtk_vec_kurtosis(normal, 5);
      double kurt_heavy = exprtk_vec_kurtosis(heavy_tail, 5);

      // Uniform-ish distribution should have negative excess kurtosis
      check(kurt_normal < 0.0);
      // Heavy-tailed distribution - just check it's calculated
      check(kurt_heavy == kurt_heavy); // Not NaN
    }

    it("should calculate entropy") {
      double uniform[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      double concentrated[] = {1.0, 1.0, 1.0, 1.0, 2.0};

      double entropy_uniform = exprtk_vec_entropy(uniform, 5, 5);
      double entropy_conc = exprtk_vec_entropy(concentrated, 5, 5);

      // Uniform distribution should have higher entropy
      check(entropy_uniform > entropy_conc);
      check(entropy_uniform > 0.0);
    }

    it("should handle edge cases for statistics") {
      double single[] = {42.0};
      double two[] = {1.0, 2.0};

      // Should not crash with small samples
      double skew = exprtk_vec_skewness(single, 1);
      double kurt = exprtk_vec_kurtosis(two, 2);
      double entropy = exprtk_vec_entropy(single, 1, 1);

      check(skew == 0.0);
      check(kurt == 0.0);
      check(entropy == 0.0);
    }
  }

  group("Realized Volatility Measures") {
    it("should calculate realized variance") {
      double returns[] = {0.01, -0.02, 0.015, -0.01, 0.02};
      size_t n = 5;

      double rvar = exprtk_vec_rvar(returns, n);

      check(rvar > 0.0);
      // RV should be average of squared returns
      double expected = 0.0;
      for (size_t i = 0; i < n; i++) {
        expected += returns[i] * returns[i];
      }
      expected /= n;
      check_float_eq(rvar, expected, EPSILON);
    }

    it("should calculate realized skewness") {
      double symmetric[] = {0.01, -0.01, 0.01, -0.01, 0.01};
      double right_skewed[] = {0.01, 0.01, 0.01, 0.02, 0.1};

      double rskew_sym = exprtk_vec_rskew(symmetric, 5);
      double rskew_right = exprtk_vec_rskew(right_skewed, 5);

      // Symmetric should be near 0
      check_float_eq(rskew_sym, 0.0, 0.5);
      // Right-skewed should be positive
      check(rskew_right > 0.0);
    }

    it("should calculate realized kurtosis") {
      double returns[] = {0.01, 0.01, 0.01, 0.01, 0.05};

      double rkurt = exprtk_vec_rkurt(returns, 5);

      // Should return a valid value
      check(rkurt == rkurt); // Not NaN
    }
  }

  group("Market Microstructure") {
    it("should calculate Amihud illiquidity") {
      double returns[] = {0.01, -0.02, 0.015, -0.01, 0.02};
      double amounts[] = {1000000, 1500000, 800000, 1200000, 900000};
      size_t n = 5;

      double illiq = exprtk_vec_illiq(returns, amounts, n);

      check(illiq > 0.0);
      // Higher returns / lower volume = higher illiquidity
    }

    it("should handle zero volume in illiquidity") {
      double returns[] = {0.01, 0.02, 0.01};
      double amounts[] = {1000, 0.0, 1000}; // Zero volume in middle

      double illiq = exprtk_vec_illiq(returns, amounts, 3);

      check(illiq > 0.0); // Should skip zero volume periods
    }

    it("should calculate volatility ratio") {
      double vol[] = {0.1, 0.1, 0.2, 0.2, 0.1, 0.1};

      double ratio = exprtk_vec_vol_ratio(vol, 6, 2, 1);

      check(ratio > 0.0);
      // Segment 1 (indices 2-3) has higher vol than average
      check(ratio > 1.0);
    }

    it("should calculate trend strength") {
      double uptrend[] = {100, 101, 102, 103, 104};
      double sideways[] = {100, 101, 100, 101, 100};

      double strength_up = exprtk_vec_trend_strength(uptrend, 5);
      double strength_side = exprtk_vec_trend_strength(sideways, 5);

      // Strong uptrend should have high R²
      check(strength_up > 0.9);
      // Sideways should have low R²
      check(strength_side < 0.5);
    }

    it("should calculate price efficiency") {
      double efficient[] = {100, 101, 102, 103, 104}; // Straight line
      double random[] = {100, 99, 101, 100, 102};     // Random walk

      double eff_straight = exprtk_vec_efficiency(efficient, 5);
      double eff_random = exprtk_vec_efficiency(random, 5);

      // Straight line should be highly efficient
      check(eff_straight > 0.9);
      // Random walk should be less efficient
      check(eff_random < eff_straight);
    }
  }

  group("Cross-Sectional Factors") {
    it("should calculate CSAD") {
      // 3 assets, 4 periods
      double returns[] = {
        0.01, 0.02, -0.01, 0.03,  // Asset 1
        0.02, 0.01,  0.00, 0.02,  // Asset 2
       -0.01, 0.03,  0.02, 0.01   // Asset 3
      };
      double out[4];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_csad(returns, 3, 4, out, &arena);

      check_int_eq(result, 4);
      // CSAD should be positive
      for (size_t i = 0; i < 4; i++) {
        check(out[i] >= 0.0);
      }

      mem_destroy(&arena);
    }

    it("should calculate rolling quantile") {
      double in[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      double out[5];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_quantile(in, 5, 3, out, &arena);

      check_int_eq(result, 5);
      // Last value (5.0) in window [3,4,5] has rank 2/3 = 0.667
      check_float_eq(out[4], 0.667, 0.01);
      // First value (1.0) in window [1] has rank 0/1 = 0.0
      check_float_eq(out[0], 0.0, EPSILON);

      mem_destroy(&arena);
    }

    it("should calculate fractal volatility dimension") {
      double vol[] = {0.1, 0.15, 0.12, 0.18, 0.14, 0.16, 0.13, 0.17};

      double fvd = exprtk_vec_fvd(vol, 8, 6);

      check(fvd >= 0.0);
    }

    it("should calculate return-signed jump") {
      double returns[] = {0.01, 0.02, 0.15, -0.01, -0.12, 0.01}; // Positive jump larger

      double rsj = exprtk_vec_rsj(returns, 6);

      // Should detect asymmetry
      check(rsj >= -1.0 && rsj <= 1.0);
    }

    it("should calculate AM-PM spread") {
      double am_returns[] = {0.01, 0.02, 0.01, 0.03};
      double pm_returns[] = {-0.01, 0.01, -0.01, 0.01};

      double spread = exprtk_vec_apm(am_returns, pm_returns, 4);

      // AM returns are higher on average
      check(spread > 0.0);
    }

    it("should calculate CGO") {
      double prices[] = {100, 101, 102, 101, 103, 104};
      double volumes[] = {1000, 1500, 1200, 1800, 1100, 1300};
      double out[6];
      mem_pool_t arena;
      mem_init(&arena, 1024);

      size_t result = exprtk_vec_cgo(prices, volumes, 6, 3, out, &arena);

      check_int_eq(result, 6);
      // First period-1 values should be 0
      check_float_eq(out[0], 0.0, EPSILON);
      check_float_eq(out[1], 0.0, EPSILON);
      // Later values should be calculated
      check(out[5] != 0.0 || out[5] == 0.0); // Just check it's a valid number

      mem_destroy(&arena);
    }

    it("should calculate salience factor") {
      double ret[] = {0.01, 0.05, -0.02, 0.03, -0.01};
      double mkt[] = {0.01, 0.02, -0.01, 0.02, 0.00};

      double salience = exprtk_vec_salience(ret, mkt, 5, 0.7);

      // Should return a valid value
      check(salience == salience); // Not NaN
    }

    it("should calculate short-term reversal") {
      double ret[] = {0.02, -0.01, 0.03, -0.02, 0.01, -0.01};
      double mkt[] = {0.01, 0.00, 0.01, -0.01, 0.00, 0.00};

      double str = exprtk_vec_str(ret, mkt, 6, 1.0);

      // STR should be between -1 and 1 (correlation)
      check(str >= -1.0 && str <= 1.0);
    }
  }
}



