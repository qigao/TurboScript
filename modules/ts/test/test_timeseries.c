/**
 * @file test_timeseries.c
 * @brief Unit tests for timeseries module — time series analysis functions.
 */
#include "ts.h"
#include "tinytest.h"
#include <turbo_buffer.h>
#include <math.h>
#include <stdint.h>

#define EPSILON 0.0001

spec("timeseries") {

    describe("ts_diff - Differencing") {

        it("should calculate first-order difference") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double data[] = {10, 12, 15, 13, 18};
            double out[5] = {0};
            size_t n = 5;

            size_t result = exprtk_ts_diff(data, n, 1, out, &arena);
            check((result) == (n - 1));  // Returns n - order

            // First diff: out[i] = data[i] - data[i-1]
            check(fabs((double)(out[1]) - (double)(2.0)) <= (double)(EPSILON));   // 12 - 10
            check(fabs((double)(out[2]) - (double)(3.0)) <= (double)(EPSILON));   // 15 - 12
            check(fabs((double)(out[3]) - (double)(-2.0)) <= (double)(EPSILON));  // 13 - 15
            check(fabs((double)(out[4]) - (double)(5.0)) <= (double)(EPSILON));   // 18 - 13

            mem_destroy(&arena);
        }
    }

    describe("ts_autocorr - Autocorrelation") {

        it("should calculate autocorrelation function") {
            double data[] = {1, 2, 3, 4, 5, 4, 3, 2, 1};
            double out[4] = {0};
            size_t n = 9;
            size_t max_lag = 3;

            size_t result = exprtk_ts_autocorr(data, n, max_lag, out);
            check((result) == (max_lag + 1));

            // ACF at lag 0 should be 1.0
            check(fabs((double)(out[0]) - (double)(1.0)) <= (double)(EPSILON));

            // ACF should be between -1 and 1
            for (size_t i = 0; i <= max_lag; i++) {
                check(out[i] >= -1.0 && out[i] <= 1.0);
            }
        }
    }

    describe("ts_adf - Augmented Dickey-Fuller") {

        it("should report two output values for valid input") {
            mem_pool_t arena = {0};
            double data[] = {10.0, 10.5, 10.2, 10.8, 10.4, 11.0, 10.7, 11.3};
            double out[2] = {0};

            mem_init(&arena, 4096);
            check((exprtk_ts_adf(data, 8, 1, out, &arena)) == (2));
            check_true(isfinite(out[0]));
            check_true(isfinite(out[1]));
            mem_destroy(&arena);
        }

        it("should reject incomplete inputs") {
            mem_pool_t arena = {0};
            double data[] = {1.0, 2.0, 3.0};
            double out[2] = {0};

            mem_init(&arena, 4096);
            check((exprtk_ts_adf(data, 3, 1, out, &arena)) == (0));
            check((exprtk_ts_adf(NULL, 3, 1, out, &arena)) == (0));
            check((exprtk_ts_adf(data, 3, 1, NULL, &arena)) == (0));
            check((exprtk_ts_adf(data, 3, 1, out, NULL)) == (0));
            check((exprtk_ts_adf(data, 3, SIZE_MAX, out, &arena)) == (0));
            mem_destroy(&arena);
        }
    }

    describe("ts_garch - GARCH volatility") {

        it("should calculate GARCH conditional volatility") {
            double returns[] = {0.01, -0.02, 0.015, -0.01, 0.02, 0.005, -0.015, 0.01};
            double out[8] = {0};
            size_t n = 8;
            double alpha = 0.1;
            double beta = 0.85;

            size_t result = exprtk_ts_garch(returns, n, alpha, beta, out);
            check((result) == (n));

            // Volatility should be positive
            for (size_t i = 0; i < n; i++) {
                check(out[i] > 0.0);
            }
        }
    }

    describe("ts_hurst - Hurst exponent") {

        it("should calculate Hurst exponent for random walk") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double data[100];
            for (size_t i = 0; i < 100; i++) {
                data[i] = (double)i + ((i % 3) - 1) * 0.5;
            }

            double hurst = exprtk_ts_hurst(data, 100, NULL, &arena);

            // Hurst should be between 0 and 1
            check(hurst >= 0.0 && hurst <= 1.0);

            mem_destroy(&arena);
        }
    }

    describe("ts_match - Pattern matching") {

        it("should find pattern matches in time series") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double data[] = {1, 2, 3, 2, 1, 2, 3, 4, 3, 2};
            double pattern[] = {1, 2, 3};
            double out[10] = {0};
            size_t n = 10;
            size_t m = 3;

            size_t result = exprtk_ts_match(data, pattern, n, m, out);
            check((result) == (n - m + 1));  // Returns number of valid windows

            // Should find matches (lower distance = better match)
            // Pattern [1,2,3] appears at index 0 and index 4
            check(out[0] >= 0.0); // Distance metric should be non-negative

            mem_destroy(&arena);
        }
    }

    describe("ts_match_cosine - Cosine similarity matching") {

        it("should calculate cosine similarity for pattern matching") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double data[] = {1, 2, 3, 4, 5, 6, 7, 8};
            double pattern[] = {1, 2, 3};
            double out[8] = {0};
            size_t n = 8;
            size_t m = 3;

            size_t result = exprtk_ts_match_cosine(data, pattern, n, m, out);
            check((result) == (n - m + 1));  // Returns number of valid windows

            // Cosine similarity should be between -1 and 1
            for (size_t i = 0; i < result; i++) {
                check(out[i] >= -1.0 && out[i] <= 1.0);
            }

            mem_destroy(&arena);
        }
    }

    describe("ts_match_dtw - Dynamic Time Warping") {

        it("should calculate DTW distance for pattern matching") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double data[] = {1, 2, 3, 4, 5, 4, 3, 2, 1};
            double pattern[] = {1, 2, 3};
            double out[9] = {0};
            size_t n = 9;
            size_t m = 3;

            size_t result = exprtk_ts_match_dtw(data, pattern, n, m, out, &arena);
            check((result) == (n - m + 1));  // Returns number of valid windows

            // DTW distance should be non-negative
            for (size_t i = 0; i < result; i++) {
                check(out[i] >= 0.0);
            }

            mem_destroy(&arena);
        }
    }

}
