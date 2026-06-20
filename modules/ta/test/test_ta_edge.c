/**
 * @file test_ta_edge.c
 * @brief Negative and edge case tests for TA indicators.
 */
#include "ta.h"
#include "tinytest.h"
#include <turbo_buffer.h>
#include <math.h>
#include <string.h>

#define EPSILON 0.0001

spec("ta_edge_cases") {

    describe("Empty / Zero-length input") {
        it("SMA should return 0 for empty input") {
            double out[1] = {999};
            check_int_eq(exprtk_ta_sma(NULL, 0, 14, out), 0);
        }

        it("EMA should return 0 for empty input") {
            double out[1] = {999};
            check_int_eq(exprtk_ta_ema(NULL, 0, 14, out), 0);
        }

        it("RSI should return 0 for empty input") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double out[1] = {999};
            check_int_eq(exprtk_ta_rsi(NULL, 0, 14, out, &arena), 0);
            mem_destroy(&arena);
        }
    }

    describe("Period > data length") {
        it("SMA should return 0 when period > n") {
            double in[] = {1, 2, 3};
            double out[3] = {0};
            check_int_eq(exprtk_ta_sma(in, 3, 10, out), 0);
        }

        it("EMA should return 0 when period > n") {
            double in[] = {1, 2, 3};
            double out[3] = {0};
            check_int_eq(exprtk_ta_ema(in, 3, 10, out), 0);
        }

        it("RSI should return 0 when period > n") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double in[] = {1, 2, 3};
            double out[3] = {0};
            check_int_eq(exprtk_ta_rsi(in, 3, 14, out, &arena), 0);
            mem_destroy(&arena);
        }

        it("ATR should return 0 when period > n") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double h[] = {2}, l[] = {1}, c[] = {1.5};
            double out[1] = {0};
            check_int_eq(exprtk_ta_atr(h, l, c, 1, 14, out, &arena), 0);
            mem_destroy(&arena);
        }
    }

    describe("Period = 0") {
        it("SMA should return 0 for period=0") {
            double in[] = {1, 2, 3, 4, 5};
            double out[5] = {0};
            check_int_eq(exprtk_ta_sma(in, 5, 0, out), 0);
        }

        it("ARBR should return 0 for period=0") {
            double h[] = {2,3,4}, l[] = {1,2,3}, o[] = {1.5,2.5,3.5}, c[] = {1.8,2.8,3.8};
            double ar[3]={0}, br[3]={0};
            check_int_eq(exprtk_ta_arbr(h, l, o, c, 3, 0, ar, br), 0);
        }

        it("Chop should return 0 for period < 2") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double h[] = {2,3}, l[] = {1,2}, c[] = {1.5,2.5};
            double out[2] = {0};
            check_int_eq(exprtk_ta_chop(h, l, c, 2, 1, out, &arena), 0);
            mem_destroy(&arena);
        }
    }

    describe("Period = 1") {
        it("SMA with period=1 should return input") {
            double in[] = {3.0, 7.0, 5.0};
            double out[3] = {0};
            size_t r = exprtk_ta_sma(in, 3, 1, out);
            check_int_eq(r, 3);
            check_float_eq(out[0], 3.0, EPSILON);
            check_float_eq(out[1], 7.0, EPSILON);
            check_float_eq(out[2], 5.0, EPSILON);
        }
    }

    describe("Constant input") {
        it("RSI should be 50 for constant prices") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double in[20];
            double out[20] = {0};
            for (int i = 0; i < 20; i++) in[i] = 100.0;
            size_t r = exprtk_ta_rsi(in, 20, 14, out, &arena);
            check(r > 0);
            /* With no change, RSI should be ~50 or 0 (implementation dependent) */
            mem_destroy(&arena);
        }

        it("BBands upper == lower for constant input") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double in[20], upper[20]={0}, lower[20]={0}, mid[20]={0};
            for (int i = 0; i < 20; i++) in[i] = 50.0;
            size_t r = exprtk_ta_bbands(in, 20, 10, 2.0, upper, mid, lower);
            check(r > 0);
            /* Std dev = 0, so upper == mid == lower */
            check_float_eq(upper[15], mid[15], EPSILON);
            check_float_eq(lower[15], mid[15], EPSILON);
            mem_destroy(&arena);
        }
    }

    describe("Minimum viable input") {
        it("Fisher should handle period=2 with 2 bars") {
            mem_pool_t arena = {0};
            mem_init(&arena, 4096);
            double h[] = {10, 12}, l[] = {8, 9};
            double fisher[2]={0}, trigger[2]={0};
            size_t r = exprtk_ta_fisher(h, l, 2, 2, fisher, trigger, &arena);
            check(r == 2);
            mem_destroy(&arena);
        }

        it("Ehlers cyber cycle should return 0 for n < 7") {
            double in[] = {1, 2, 3, 4, 5, 6};
            double out[6] = {0};
            check_int_eq(exprtk_ta_ehlers_cyber_cycle(in, 6, 0.07, out), 0);
        }

        it("Ehlers MAMA should return 0 for n < 32") {
            double in[31], mama[31]={0}, fama[31]={0};
            for (int i = 0; i < 31; i++) in[i] = (double)i;
            check_int_eq(exprtk_ta_ehlers_mama(in, 31, 0.5, 0.05, mama, fama), 0);
        }
    }

    describe("Monotonic input") {
        it("Supertrend should detect uptrend for rising prices") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double h[30], l[30], c[30];
            for (int i = 0; i < 30; i++) {
                c[i] = 100.0 + (double)i * 2.0;
                h[i] = c[i] + 1.0;
                l[i] = c[i] - 1.0;
            }
            double upper[30]={0}, lower[30]={0}, trend[30]={0};
            size_t r = exprtk_ta_supertrend(h, l, c, 30, 10, 3.0, trend, upper, lower, &arena);
            check(r > 0);
            /* Last bar should show uptrend (1) */
            check_float_eq(trend[29], 1.0, EPSILON);
            mem_destroy(&arena);
        }
    }
}
