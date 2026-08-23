/**
 * @file test_ta.c
 * @brief Unit tests for TA module — technical analysis indicators.
 */
#include "ta.h"
#include "tinytest.h"
#include <turbo_buffer.h>
#include <math.h>
#include <string.h>

#define EPSILON 0.0001

spec("ta_indicators") {

    describe("SMA - Simple Moving Average") {

        it("should calculate SMA correctly") {
            double in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
            double out[10] = {0};
            size_t n = 10;
            size_t period = 3;

            size_t result = exprtk_ta_sma(in, n, period, out);
            check((result) == (n));

            // SMA(3) at index 2: (1+2+3)/3 = 2.0
            check(fabs((double)(out[2]) - (double)(2.0)) <= (double)(EPSILON));
            // SMA(3) at index 3: (2+3+4)/3 = 3.0
            check(fabs((double)(out[3]) - (double)(3.0)) <= (double)(EPSILON));
            // SMA(3) at index 9: (8+9+10)/3 = 9.0
            check(fabs((double)(out[9]) - (double)(9.0)) <= (double)(EPSILON));
        }

        it("should handle period equal to length") {
            double in[] = {2, 4, 6};
            double out[3] = {0};

            size_t result = exprtk_ta_sma(in, 3, 3, out);
            check((result) == (3));
            check(fabs((double)(out[2]) - (double)(4.0)) <= (double)(EPSILON)); // (2+4+6)/3 = 4.0
        }
    }

    describe("EMA - Exponential Moving Average") {

        it("should calculate EMA correctly") {
            double in[] = {10, 11, 12, 13, 14, 15};
            double out[6] = {0};
            size_t n = 6;
            size_t period = 3;

            size_t result = exprtk_ta_ema(in, n, period, out);
            check((result) == (n));

            // Warmup is undefined until period-1
            check(fabs((double)(out[0]) - (double)(0.0)) <= (double)(EPSILON));
            check(fabs((double)(out[1]) - (double)(0.0)) <= (double)(EPSILON));
            // Seed from SMA(10,11,12)=11
            check(fabs((double)(out[2]) - (double)(11.0)) <= (double)(EPSILON));
            check(out[5] > out[2]);
        }
    }

    describe("VWAP - Anchored and Session") {
        it("should reset session VWAP when session id changes") {
            double hi[] = {11, 13, 21, 23};
            double lo[] = {9, 11, 19, 21};
            double cl[] = {10, 12, 20, 22};
            double vol[] = {100, 100, 100, 100};
            double session[] = {1, 1, 2, 2};
            double anchored[4] = {0};
            double session_vwap[4] = {0};

            check((exprtk_ta_vwap(hi, lo, cl, vol, 4, anchored)) == (4));
            check((exprtk_ta_vwap_session(hi, lo, cl, vol, session, 4, session_vwap)) == (4));

            check(fabs((double)(anchored[0]) - (double)(10.0)) <= (double)(EPSILON));
            check(fabs((double)(anchored[3]) - (double)(16.0)) <= (double)(EPSILON));
            check(fabs((double)(session_vwap[0]) - (double)(10.0)) <= (double)(EPSILON));
            check(fabs((double)(session_vwap[1]) - (double)(11.0)) <= (double)(EPSILON));
            check(fabs((double)(session_vwap[2]) - (double)(20.0)) <= (double)(EPSILON));
            check(fabs((double)(session_vwap[3]) - (double)(21.0)) <= (double)(EPSILON));
        }
    }

    describe("RSI - Relative Strength Index") {

        it("should calculate RSI for trending data") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double in[] = {44, 44.34, 44.09, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84, 46.08,
                          45.89, 46.03, 45.61, 46.28, 46.28, 46.00, 46.03, 46.41, 46.22, 45.64};
            double out[20] = {0};
            size_t n = 20;
            size_t period = 14;

            size_t result = exprtk_ta_rsi(in, n, period, out, &arena);
            check((result) == (n));

            // RSI should be between 0 and 100
            for (size_t i = period; i < n; i++) {
                check(out[i] >= 0.0 && out[i] <= 100.0);
            }

            mem_destroy(&arena);
        }
    }

    describe("MACD - Moving Average Convergence Divergence") {

        it("should calculate MACD line, signal, and histogram") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            // Need enough data for MACD (slow period is 26)
            double in[50];
            for (size_t i = 0; i < 50; i++) {
                in[i] = 100.0 + (double)i * 0.5;
            }

            double line[50] = {0};
            double sig[50] = {0};
            double hist[50] = {0};

            size_t result = exprtk_ta_macd(in, 50, 12, 26, 9, line, sig, hist, &arena);
            check((result) == (50));

            // MACD line should have values after slow period
            // Signal line needs additional 9 periods
            // Check that histogram = line - signal for later indices
            for (size_t i = 35; i < 50; i++) {
                if (line[i] != 0.0 || sig[i] != 0.0) {
                    check(fabs((double)(hist[i]) - (double)(line[i] - sig[i])) <= (double)(EPSILON));
                }
            }

            mem_destroy(&arena);
        }
    }

    describe("ATR - Average True Range") {

        it("should calculate ATR from high/low/close") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);

            double hi[] = {48, 49, 50, 51, 52};
            double lo[] = {46, 47, 48, 49, 50};
            double cl[] = {47, 48, 49, 50, 51};
            double out[5] = {0};
            size_t n = 5;
            size_t period = 3;

            size_t result = exprtk_ta_atr(hi, lo, cl, n, period, out, &arena);
            check((result) == (n));

            // ATR should be positive
            for (size_t i = period - 1; i < n; i++) {
                check(out[i] > 0.0);
            }

            mem_destroy(&arena);
        }
    }

    describe("Bollinger Bands") {

        it("should calculate upper, middle, and lower bands") {
            double in[] = {10, 11, 12, 11, 10, 9, 10, 11, 12, 13};
            double upper[10] = {0};
            double middle[10] = {0};
            double lower[10] = {0};
            size_t n = 10;
            size_t period = 5;
            double mult = 2.0;

            size_t result = exprtk_ta_bbands(in, n, period, mult, upper, middle, lower);
            check((result) == (n));

            // Middle band should equal SMA
            // Upper should be above middle, lower should be below
            for (size_t i = period - 1; i < n; i++) {
                check(upper[i] >= middle[i]);
                check(middle[i] >= lower[i]);
            }
        }
    }

    describe("Momentum Indicators") {

        it("should calculate MOM (momentum)") {
            double in[] = {100, 102, 101, 103, 105, 104, 106, 108};
            double out[8] = {0};
            size_t n = 8;
            size_t period = 3;

            size_t result = exprtk_ta_mom(in, n, period, out);
            check((result) == (n));

            // MOM at index 3: in[3] - in[0] = 103 - 100 = 3
            check(fabs((double)(out[3]) - (double)(3.0)) <= (double)(EPSILON));
        }

        it("should calculate ROC (rate of change)") {
            double in[] = {100, 102, 104, 106, 108};
            double out[5] = {0};
            size_t n = 5;
            size_t period = 2;

            size_t result = exprtk_ta_roc(in, n, period, out);
            check((result) == (n));

            // ROC at index 2: ((104 - 100) / 100) * 100 = 4.0
            check(fabs((double)(out[2]) - (double)(4.0)) <= (double)(EPSILON));
        }
    }

    describe("Black-Scholes-Merton Options") {

        it("should calculate call option price") {
            double S[] = {100};
            double K[] = {100};
            double T[] = {1.0};
            double r[] = {0.05};
            double sigma[] = {0.2};
            double out[1] = {0};

            size_t result = exprtk_ta_bsm_call(S, K, T, r, sigma, 1, out);
            check((result) == (1));

            // ATM call with 1 year to expiry should have positive value
            check(out[0] > 0.0);
            check(out[0] < S[0]); // Should be less than spot price
        }

        it("should calculate put option price") {
            double S[] = {100};
            double K[] = {100};
            double T[] = {1.0};
            double r[] = {0.05};
            double sigma[] = {0.2};
            double out[1] = {0};

            size_t result = exprtk_ta_bsm_put(S, K, T, r, sigma, 1, out);
            check((result) == (1));

            // ATM put with 1 year to expiry should have positive value
            check(out[0] > 0.0);
        }

        it("should calculate delta for call") {
            double S[] = {100};
            double K[] = {100};
            double T[] = {1.0};
            double r[] = {0.05};
            double sigma[] = {0.2};
            double out[1] = {0};

            size_t result = exprtk_ta_bsm_delta_call(S, K, T, r, sigma, 1, out);
            check((result) == (1));

            // Delta should be between 0 and 1 for calls
            check(out[0] > 0.0 && out[0] < 1.0);
        }
    }

    describe("Volume Indicators") {

        it("should calculate OBV (On-Balance Volume)") {
            double cl[] = {10, 11, 10, 12, 11};
            double vol[] = {1000, 1500, 1200, 1800, 1300};
            double out[5] = {0};
            size_t n = 5;

            size_t result = exprtk_ta_obv(cl, vol, n, out);
            check((result) == (n));

            // OBV starts with first volume, then accumulates based on price direction
            check(fabs((double)(out[0]) - (double)(1000.0)) <= (double)(EPSILON));  // Initial volume
            check(fabs((double)(out[1]) - (double)(2500.0)) <= (double)(EPSILON));  // price up: 1000 + 1500
            check(fabs((double)(out[2]) - (double)(1300.0)) <= (double)(EPSILON));  // price down: 2500 - 1200
            check(fabs((double)(out[3]) - (double)(3100.0)) <= (double)(EPSILON));  // price up: 1300 + 1800
            check(fabs((double)(out[4]) - (double)(1800.0)) <= (double)(EPSILON));  // price down: 3100 - 1300
        }
    }
}
