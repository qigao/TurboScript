/**
 * @file test_ts_rolling.c
 * @brief Unit tests for rolling, expanding, EWM, and return functions.
 */
#include "ts.h"
#include "tinytest.h"
#include <turbo_buffer.h>
#include <math.h>
#include <string.h>

#define EPSILON 0.001

spec("ts_rolling") {

    describe("rolling_mean") {
        it("should compute rolling average") {
            double in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
            double out[10] = {0};
            size_t r = exprtk_ts_rolling_mean(in, 10, 3, out);
            check((r) == (10));
            check(fabs((double)(out[2]) - (double)(2.0)) <= (double)(EPSILON));   /* (1+2+3)/3 */
            check(fabs((double)(out[3]) - (double)(3.0)) <= (double)(EPSILON));   /* (2+3+4)/3 */
            check(fabs((double)(out[9]) - (double)(9.0)) <= (double)(EPSILON));   /* (8+9+10)/3 */
        }

        it("should return 0 when period > n") {
            double in[] = {1, 2};
            double out[2] = {0};
            check((exprtk_ts_rolling_mean(in, 2, 5, out)) == (0));
        }
    }

    describe("rolling_std") {
        it("should compute rolling standard deviation") {
            double in[] = {2, 4, 4, 4, 5, 5, 7, 9};
            double out[8] = {0};
            size_t r = exprtk_ts_rolling_std(in, 8, 4, out);
            check((r) == (8));
            /* First 3 should be 0 */
            check(fabs((double)(out[0]) - (double)(0.0)) <= (double)(EPSILON));
            /* std of {2,4,4,4} = ~0.9574 (sample std) */
            check(out[3] > 0.5 && out[3] < 1.5);
        }

        it("should return 0 for constant input") {
            double in[] = {5, 5, 5, 5, 5};
            double out[5] = {0};
            exprtk_ts_rolling_std(in, 5, 3, out);
            check(fabs((double)(out[4]) - (double)(0.0)) <= (double)(EPSILON));
        }
    }

    describe("rolling_skew") {
        it("should compute rolling skewness") {
            double in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
            double out[10] = {0};
            size_t r = exprtk_ts_rolling_skew(in, 10, 5, out);
            check((r) == (10));
            /* Symmetric data should have ~0 skewness */
            check(fabs(out[6]) < 0.5);
        }
    }

    describe("rolling_kurt") {
        it("should compute rolling kurtosis") {
            double in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
            double out[10] = {0};
            size_t r = exprtk_ts_rolling_kurt(in, 10, 5, out);
            check((r) == (10));
            /* Uniform-ish data: excess kurtosis should be negative */
            check(out[6] < 0.5);
        }
    }

    describe("rolling_corr") {
        it("should return 1.0 for perfectly correlated series") {
            double a[] = {1, 2, 3, 4, 5, 6, 7, 8};
            double b[] = {2, 4, 6, 8, 10, 12, 14, 16};
            double out[8] = {0};
            size_t r = exprtk_ts_rolling_corr(a, b, 8, 4, out);
            check((r) == (8));
            check(fabs((double)(out[7]) - (double)(1.0)) <= (double)(EPSILON));
        }

        it("should return -1.0 for inversely correlated series") {
            double a[] = {1, 2, 3, 4, 5, 6, 7, 8};
            double b[] = {16, 14, 12, 10, 8, 6, 4, 2};
            double out[8] = {0};
            exprtk_ts_rolling_corr(a, b, 8, 4, out);
            check(fabs((double)(out[7]) - (double)(-1.0)) <= (double)(EPSILON));
        }
    }

    describe("rolling_beta") {
        it("should compute regression beta") {
            double y[] = {2, 4, 6, 8, 10, 12, 14, 16};
            double x[] = {1, 2, 3, 4, 5, 6, 7, 8};
            double out[8] = {0};
            size_t r = exprtk_ts_rolling_beta(y, x, 8, 4, out);
            check((r) == (8));
            /* y = 2*x, so beta should be ~2.0 */
            check(fabs((double)(out[7]) - (double)(2.0)) <= (double)(EPSILON));
        }
    }

    describe("expanding_mean") {
        it("should compute cumulative mean") {
            double in[] = {2, 4, 6, 8, 10};
            double out[5] = {0};
            size_t r = exprtk_ts_expanding_mean(in, 5, out);
            check((r) == (5));
            check(fabs((double)(out[0]) - (double)(2.0)) <= (double)(EPSILON));
            check(fabs((double)(out[1]) - (double)(3.0)) <= (double)(EPSILON));   /* (2+4)/2 */
            check(fabs((double)(out[4]) - (double)(6.0)) <= (double)(EPSILON));   /* (2+4+6+8+10)/5 */
        }
    }

    describe("expanding_std") {
        it("should compute cumulative std") {
            double in[] = {2, 4, 6, 8, 10};
            double out[5] = {0};
            size_t r = exprtk_ts_expanding_std(in, 5, out);
            check((r) == (5));
            check(fabs((double)(out[0]) - (double)(0.0)) <= (double)(EPSILON));
            check(out[4] > 0);
        }
    }

    describe("ewm_mean") {
        it("should compute exponentially weighted mean") {
            double in[] = {1, 2, 3, 4, 5};
            double out[5] = {0};
            size_t r = exprtk_ts_ewm_mean(in, 5, 3, out);
            check((r) == (5));
            check(fabs((double)(out[0]) - (double)(1.0)) <= (double)(EPSILON));
            /* EWM should be between min and max */
            check(out[4] > 1.0 && out[4] < 5.0);
        }
    }

    describe("ewm_std") {
        it("should compute exponentially weighted std") {
            double in[] = {1, 3, 1, 3, 1, 3, 1, 3};
            double out[8] = {0};
            size_t r = exprtk_ts_ewm_std(in, 8, 3, out);
            check((r) == (8));
            check(fabs((double)(out[0]) - (double)(0.0)) <= (double)(EPSILON));
            check(out[7] > 0);
        }
    }

    describe("pct_change") {
        it("should compute percentage change") {
            double in[] = {100, 110, 121, 100};
            double out[4] = {0};
            size_t r = exprtk_ts_pct_change(in, 4, 1, out);
            check((r) == (4));
            check(fabs((double)(out[0]) - (double)(0.0)) <= (double)(EPSILON));
            check(fabs((double)(out[1]) - (double)(0.1)) <= (double)(EPSILON));    /* 110/100 - 1 */
            check(fabs((double)(out[2]) - (double)(0.1)) <= (double)(EPSILON));    /* 121/110 - 1 */
        }

        it("should handle multi-period pct_change") {
            double in[] = {100, 110, 121, 133.1};
            double out[4] = {0};
            exprtk_ts_pct_change(in, 4, 2, out);
            check(fabs((double)(out[2]) - (double)(0.21)) <= (double)(EPSILON));   /* 121/100 - 1 */
        }
    }

    describe("log_return") {
        it("should compute log returns") {
            double in[] = {100, 110, 121};
            double out[3] = {0};
            size_t r = exprtk_ts_log_return(in, 3, out);
            check((r) == (3));
            check(fabs((double)(out[0]) - (double)(0.0)) <= (double)(EPSILON));
            check(fabs((double)(out[1]) - (double)(log(1.1))) <= (double)(EPSILON));
            check(fabs((double)(out[2]) - (double)(log(1.1))) <= (double)(EPSILON));
        }
    }

    describe("cum_return") {
        it("should compute cumulative return") {
            double in[] = {0.1, 0.05, -0.02};
            double out[3] = {0};
            size_t r = exprtk_ts_cum_return(in, 3, out);
            check((r) == (3));
            check(fabs((double)(out[0]) - (double)(1.1)) <= (double)(EPSILON));
            check(fabs((double)(out[1]) - (double)(1.1 * 1.05)) <= (double)(EPSILON));
            check(fabs((double)(out[2]) - (double)(1.1 * 1.05 * 0.98)) <= (double)(EPSILON));
        }
    }

    describe("Cointegration") {
        it("should detect cointegrated series") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double x[100], y[100], out[3] = {0};
            for (int i = 0; i < 100; i++) {
                x[i] = (double)i + sin((double)i * 0.1);
                y[i] = 2.0 * x[i] + 5.0 + sin((double)i * 0.05) * 0.5;
            }
            size_t r = exprtk_ts_coint(x, y, 100, out, &arena);
            check((r) == (3));
            /* Hedge ratio should be close to 2.0 */
            check(fabs(out[2] - 2.0) < 0.5);
            /* Approximate p-value must be normalized, not a raw slope. */
            check(out[1] >= 0.0 && out[1] <= 1.0);
            mem_destroy(&arena);
        }
    }

    describe("Spread") {
        it("should compute regression spread") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double x[] = {1, 2, 3, 4, 5};
            double y[] = {2, 4, 6, 8, 10};
            double out[5] = {0};
            size_t r = exprtk_ts_spread(x, y, 5, out, &arena);
            check((r) == (5));
            /* y = 2*x exactly, spread should be ~0 */
            for (int i = 0; i < 5; i++)
                check(fabs(out[i]) < EPSILON);
            mem_destroy(&arena);
        }
    }
}
