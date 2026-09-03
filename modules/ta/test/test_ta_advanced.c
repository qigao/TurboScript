/**
 * @file test_ta_advanced.c
 * @brief Unit tests for advanced TA indicators: ichimoku, keltner, alligator,
 *        rsrs, arbr, shadow, fisher, squeeze, chop, ehlers.
 */
#include "ta.h"
#include "tinytest.h"
#include <salts_buffer.h>
#include <math.h>
#include <string.h>

#define EPSILON 0.01
#define N 60

static double hi[N], lo[N], cl[N], op[N], vol[N];

static void gen_ohlcv(void) {
    for (size_t i = 0; i < N; i++) {
        double base = 100.0 + (double)i * 0.5;
        op[i]  = base;
        hi[i]  = base + 2.0;
        lo[i]  = base - 1.0;
        cl[i]  = base + 1.0;
        vol[i] = 1000.0 + (double)i * 10.0;
    }
}

spec("ta_advanced") {

    before_each() { gen_ohlcv(); }

    describe("Ichimoku") {
        it("should produce 5 output vectors") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double tenkan[N]={0}, kijun[N]={0}, sa[N]={0}, sb[N]={0}, chikou[N]={0};
            size_t r = exprtk_ta_ichimoku(hi, lo, cl, N, 9, 26, 52, tenkan, kijun, sa, sb, chikou, &arena);
            check(r > 0);
            /* Tenkan should be midpoint of 9-period high/low */
            check(tenkan[9] > 0);
            check(kijun[25] > 0);
            mem_destroy(&arena);
        }

        it("should keep chikou lagged instead of reading future closes at the same index") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double tenkan[N]={0}, kijun[N]={0}, sa[N]={0}, sb[N]={0}, chikou[N]={0};
            size_t kijun_period = 26;

            size_t r = exprtk_ta_ichimoku(hi, lo, cl, N, 9, kijun_period, 52,
                                          tenkan, kijun, sa, sb, chikou, &arena);
            check(r > 0);
            check(fabs((double)(chikou[0]) - (double)(0.0)) <= (double)(EPSILON));
            check(fabs((double)(chikou[kijun_period]) - (double)(cl[0])) <= (double)(EPSILON));
            check(fabs((double)(chikou[kijun_period + 5]) - (double)(cl[5])) <= (double)(EPSILON));
            mem_destroy(&arena);
        }
    }

    describe("Keltner Channels") {
        it("should produce upper > mid > lower") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double upper[N]={0}, mid[N]={0}, lower[N]={0};
            size_t r = exprtk_ta_keltner(hi, lo, cl, N, 20, 10, 1.5, upper, mid, lower, &arena);
            check(r > 0);
            for (size_t i = 20; i < N; i++) {
                check(upper[i] >= mid[i]);
                check(mid[i] >= lower[i]);
            }
            mem_destroy(&arena);
        }
    }

    describe("Alligator") {
        it("should produce jaw/teeth/lips") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double jaw[N]={0}, teeth[N]={0}, lips[N]={0};
            size_t r = exprtk_ta_alligator(cl, N, jaw, teeth, lips, &arena);
            check(r > 0);
            /* Jaw is slowest (13-period SMMA), should be non-zero after warmup */
            check(jaw[20] > 0);
            check(teeth[20] > 0);
            check(lips[20] > 0);
            mem_destroy(&arena);
        }
    }

    describe("ARBR") {
        it("should produce AR and BR values") {
            double ar[N]={0}, br[N]={0};
            size_t r = exprtk_ta_arbr(hi, lo, op, cl, N, 10, ar, br);
            check(r > 0);
            /* First period-1 should be zero */
            check(fabs((double)(ar[0]) - (double)(0.0)) <= (double)(EPSILON));
            /* AR at period-1 should be positive for uptrending data */
            check(ar[9] > 0);
        }

        it("should not read out of bounds at i=period") {
            /* This was the F.1 bug fix — just verify no crash */
            double ar[N]={0}, br[N]={0};
            size_t r = exprtk_ta_arbr(hi, lo, op, cl, N, 10, ar, br);
            check(r == N);
            check(ar[10] > 0);
        }
    }

    describe("Shadow") {
        it("should produce upper and lower shadow ratios") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double upper[N]={0}, lower[N]={0};
            size_t r = exprtk_ta_shadow(op, hi, lo, cl, N, upper, lower, &arena);
            check(r > 0);
            for (size_t i = 0; i < N; i++) {
                check(upper[i] >= 0.0);
                check(lower[i] >= 0.0);
            }
            mem_destroy(&arena);
        }
    }

    describe("Fisher Transform") {
        it("should produce fisher and trigger lines") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double fisher[N]={0}, trigger[N]={0};
            size_t r = exprtk_ta_fisher(hi, lo, N, 10, fisher, trigger, &arena);
            check(r == N);
            /* Fisher values should be non-zero after warmup */
            check(fisher[15] != 0.0);
            /* Trigger is previous fisher */
            check(fabs((double)(trigger[16]) - (double)(fisher[15])) <= (double)(EPSILON));
            mem_destroy(&arena);
        }
    }

    describe("Squeeze Momentum") {
        it("should detect squeeze on/off") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double sq[N]={0}, mom[N]={0}, on[N]={0};
            size_t r = exprtk_ta_squeeze(hi, lo, cl, N, 20, 2.0, 20, 1.5, sq, mom, on, &arena);
            check(r > 0);
            /* on_off should be 0 or 1 */
            for (size_t i = 20; i < N; i++) {
                check(on[i] == 0.0 || on[i] == 1.0);
            }
            mem_destroy(&arena);
        }
    }

    describe("Choppiness Index") {
        it("should produce values in 0-100 range") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double out[N]={0};
            size_t r = exprtk_ta_chop(hi, lo, cl, N, 14, out, &arena);
            check(r > 0);
            for (size_t i = 14; i < N; i++) {
                check(out[i] >= 0.0 && out[i] <= 100.0);
            }
            mem_destroy(&arena);
        }
    }

    describe("Ehlers Cyber Cycle") {
        it("should produce output without crash") {
            double out[N]={0};
            size_t r = exprtk_ta_ehlers_cyber_cycle(cl, N, 0.07, out);
            check(r == N);
        }
    }

    describe("Ehlers Instantaneous Trendline") {
        it("should track price trend") {
            double out[N]={0};
            size_t r = exprtk_ta_ehlers_itrend(cl, N, 0.07, out);
            check(r == N);
            /* For uptrending data, itrend should follow */
            check(out[N-1] > out[10]);
        }
    }

    describe("Ehlers MAMA/FAMA") {
        it("should produce mama and fama") {
            double mama[64]={0}, fama[64]={0};
            /* Need more data for MAMA */
            double prices[64];
            for (size_t i = 0; i < 64; i++)
                prices[i] = 100.0 + sin((double)i * 0.3) * 5.0;
            size_t r = exprtk_ta_ehlers_mama(prices, 64, 0.5, 0.05, mama, fama);
            check(r == 64);
            /* MAMA should be closer to price than FAMA */
            check(mama[63] != 0.0);
            check(fama[63] != 0.0);
        }
    }

    describe("RSRS") {
        it("should produce slope and zscore") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double slope[N]={0}, zscore[N]={0};
            size_t r = exprtk_ta_rsrs(hi, lo, N, 10, 10, slope, zscore, &arena);
            check(r > 0);
            mem_destroy(&arena);
        }
    }

    describe("Smart Money") {
        it("should produce correlation values") {
            mem_pool_t arena = {0};
            mem_init(&arena, 65536);
            double out[N]={0};
            size_t r = exprtk_ta_smart_money(cl, vol, N, 10, out, &arena);
            check(r > 0);
            /* Correlation should be in [-1, 1] */
            for (size_t i = 10; i < N; i++) {
                check(out[i] >= -1.01 && out[i] <= 1.01);
            }
            mem_destroy(&arena);
        }
    }
}
