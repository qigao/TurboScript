/**
 * @file exprtk_ta_indicators_3.c
 * @brief Fisher Transform, Squeeze Momentum, Choppiness Index, Ehlers indicators.
 */
#include "ta.h"
#include "ta.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif



/* ========================================================================= */
/* Fisher Transform                                                          */
/* ========================================================================= */

size_t exprtk_ta_fisher(const double *hi, const double *lo, size_t n, size_t period,
                         double *fisher, double *trigger, mem_pool_t *arena) {
    (void)arena;
    if (period < 2 || n < period) return 0;

    double val = 0, prev_fish = 0;
    double hh = hi[0], ll = lo[0];

    for (size_t i = 0; i < n; i++) {
        /* Rolling highest high / lowest low */
        if (i < period) {
            for (size_t j = 0; j <= i; j++) {
                if (hi[j] > hh) hh = hi[j];
                if (lo[j] < ll) ll = lo[j];
            }
        } else {
            hh = hi[i]; ll = lo[i];
            for (size_t j = i - period + 1; j <= i; j++) {
                if (hi[j] > hh) hh = hi[j];
                if (lo[j] < ll) ll = lo[j];
            }
        }

        double mid = (hi[i] + lo[i]) * 0.5;
        double range = hh - ll;
        double raw = (range > 1e-15) ? (mid - ll) / range : 0.0;

        /* Normalize to [-1, 1] range, clamp */
        raw = 2.0 * raw - 1.0;
        /* Smooth */
        val = 0.33 * raw + 0.67 * val;
        if (val > 0.999) val = 0.999;
        if (val < -0.999) val = -0.999;

        double fish = 0.5 * log((1.0 + val) / (1.0 - val));
        trigger[i] = prev_fish;
        fisher[i] = fish;
        prev_fish = fish;
    }
    return n;
}

/* ========================================================================= */
/* Squeeze Momentum (Bollinger + Keltner)                                    */
/* ========================================================================= */

size_t exprtk_ta_squeeze(const double *hi, const double *lo, const double *cl, size_t n,
                          size_t bb_p, double bb_m, size_t kc_p, double kc_m,
                          double *squeeze, double *momentum, double *on_off,
                          mem_pool_t *arena) {
    size_t min_p = (bb_p > kc_p) ? bb_p : kc_p;
    if (n < min_p) return 0;

    double *bb_upper = MEM_ALLOC_ARRAY(arena, double, n);
    double *bb_lower = MEM_ALLOC_ARRAY(arena, double, n);
    double *bb_mid   = MEM_ALLOC_ARRAY(arena, double, n);
    double *kc_upper = MEM_ALLOC_ARRAY(arena, double, n);
    double *kc_lower = MEM_ALLOC_ARRAY(arena, double, n);
    double *kc_mid   = MEM_ALLOC_ARRAY(arena, double, n);
    if (!bb_upper || !bb_lower || !bb_mid || !kc_upper || !kc_lower || !kc_mid)
        return 0;

    exprtk_ta_bbands(cl, n, bb_p, bb_m, bb_upper, bb_mid, bb_lower);
    exprtk_ta_keltner(hi, lo, cl, n, kc_p, kc_p, kc_m, kc_upper, kc_mid, kc_lower, arena);

    /* Linear regression of (close - midline) for momentum */
    size_t lr_period = bb_p;

    for (size_t i = 0; i < n; i++) {
        /* Squeeze on/off: BB inside KC = squeeze on */
        on_off[i] = (bb_lower[i] > kc_lower[i] && bb_upper[i] < kc_upper[i]) ? 1.0 : 0.0;

        /* Momentum: linear regression value of (close - avg(bb_mid, kc_mid)) */
        double delta = cl[i] - (bb_mid[i] + kc_mid[i]) * 0.5;
        squeeze[i] = delta;

        if (i >= lr_period - 1) {
            /* Simple linear regression over lr_period */
            double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
            for (size_t j = 0; j < lr_period; j++) {
                double x = (double)j;
                double y = cl[i - lr_period + 1 + j] - (bb_mid[i - lr_period + 1 + j] + kc_mid[i - lr_period + 1 + j]) * 0.5;
                sum_x += x; sum_y += y; sum_xy += x * y; sum_xx += x * x;
            }
            double denom = (double)lr_period * sum_xx - sum_x * sum_x;
            if (fabs(denom) > 1e-15) {
                double slope = ((double)lr_period * sum_xy - sum_x * sum_y) / denom;
                double intercept = (sum_y - slope * sum_x) / (double)lr_period;
                momentum[i] = intercept + slope * (double)(lr_period - 1);
            } else {
                momentum[i] = delta;
            }
        } else {
            momentum[i] = delta;
        }
    }
    return n;
}

/* ========================================================================= */
/* Choppiness Index                                                          */
/* ========================================================================= */

size_t exprtk_ta_chop(const double *hi, const double *lo, const double *cl, size_t n,
                       size_t period, double *out, mem_pool_t *arena) {
    if (period < 2 || n < period + 1) return 0;

    double *atr_arr = MEM_ALLOC_ARRAY(arena, double, n);
    if (!atr_arr) return 0;
    exprtk_ta_atr(hi, lo, cl, n, 1, atr_arr, arena);

    for (size_t i = 0; i < period; i++)
        out[i] = 0;

    for (size_t i = period; i < n; i++) {
        /* Sum of ATR(1) over period */
        double atr_sum = 0;
        for (size_t j = i - period + 1; j <= i; j++)
            atr_sum += atr_arr[j];

        /* Highest high - lowest low over period */
        double hh = hi[i - period + 1], ll = lo[i - period + 1];
        for (size_t j = i - period + 2; j <= i; j++) {
            if (hi[j] > hh) hh = hi[j];
            if (lo[j] < ll) ll = lo[j];
        }

        double range = hh - ll;
        if (range > 1e-15)
            out[i] = 100.0 * log10(atr_sum / range) / log10((double)period);
        else
            out[i] = 0;
    }
    return n;
}

/* ========================================================================= */
/* Ehlers Cyber Cycle                                                        */
/* ========================================================================= */

size_t exprtk_ta_ehlers_cyber_cycle(const double *in, size_t n, double alpha, double *out) {
    if (n < 7) return 0;
    memset(out, 0, n * sizeof(double));

    /* Smooth price */
    double smooth[4] = {0};
    for (size_t i = 0; i < n; i++) {
        double s = (in[i] + 2.0 * (i >= 1 ? in[i-1] : in[i]) +
                    2.0 * (i >= 2 ? in[i-2] : in[i]) +
                    (i >= 3 ? in[i-3] : in[i])) / 6.0;

        if (i >= 6) {
            out[i] = (1.0 - 0.5 * alpha) * (1.0 - 0.5 * alpha) * (s - 2.0 * smooth[(i-1) % 4] + smooth[(i-2) % 4])
                     + 2.0 * (1.0 - alpha) * out[i-1]
                     - (1.0 - alpha) * (1.0 - alpha) * out[i-2];
        }
        smooth[i % 4] = s;
    }
    return n;
}

/* ========================================================================= */
/* Ehlers Instantaneous Trendline                                            */
/* ========================================================================= */

size_t exprtk_ta_ehlers_itrend(const double *in, size_t n, double alpha, double *out) {
    if (n < 7) return 0;
    memset(out, 0, n * sizeof(double));

    for (size_t i = 0; i < n; i++) {
        if (i < 7) {
            out[i] = (in[i] + 2.0 * (i >= 1 ? in[i-1] : in[i]) + (i >= 2 ? in[i-2] : in[i])) / 4.0;
        } else {
            out[i] = (alpha - alpha * alpha / 4.0) * in[i]
                     + 0.5 * alpha * alpha * (i >= 1 ? in[i-1] : in[i])
                     - (alpha - 0.75 * alpha * alpha) * (i >= 2 ? in[i-2] : in[i])
                     + 2.0 * (1.0 - alpha) * out[i-1]
                     - (1.0 - alpha) * (1.0 - alpha) * out[i-2];
        }
    }
    return n;
}

/* ========================================================================= */
/* Ehlers MESA Adaptive Moving Average (MAMA/FAMA)                           */
/* ========================================================================= */

size_t exprtk_ta_ehlers_mama(const double *in, size_t n, double fast_limit, double slow_limit,
                              double *mama, double *fama) {
    if (n < 32) return 0;
    memset(mama, 0, n * sizeof(double));
    memset(fama, 0, n * sizeof(double));

    double smooth[4] = {0}, detrend[4] = {0};
    double q1[4] = {0}, i1[4] = {0};
    double ji[4] = {0}, jq[4] = {0};
    double i2 = 0, q2 = 0, re = 0, im = 0;
    double period = 0, smooth_period = 0, phase = 0;

    for (size_t bar = 0; bar < n; bar++) {
        size_t b = bar % 4, b1 = (bar + 3) % 4, b2 = (bar + 2) % 4, b3 = (bar + 1) % 4;

        /* Smooth */
        smooth[b] = (4.0 * in[bar] + 3.0 * (bar >= 1 ? in[bar-1] : in[bar])
                     + 2.0 * (bar >= 2 ? in[bar-2] : in[bar])
                     + (bar >= 3 ? in[bar-3] : in[bar])) / 10.0;

        /* Detrend */
        detrend[b] = (0.0962 * smooth[b] + 0.5769 * smooth[b2]
                      - 0.5769 * smooth[b3] - 0.0962 * smooth[b1])
                     * (0.075 * period + 0.54);

        /* InPhase and Quadrature */
        q1[b] = (0.0962 * detrend[b] + 0.5769 * detrend[b2]
                 - 0.5769 * detrend[b3] - 0.0962 * detrend[b1])
                * (0.075 * period + 0.54);
        i1[b] = detrend[b3];

        /* Advance phase by 90 degrees */
        ji[b] = (0.0962 * i1[b] + 0.5769 * i1[b2] - 0.5769 * i1[b3] - 0.0962 * i1[b1])
                * (0.075 * period + 0.54);
        jq[b] = (0.0962 * q1[b] + 0.5769 * q1[b2] - 0.5769 * q1[b3] - 0.0962 * q1[b1])
                * (0.075 * period + 0.54);

        double ni2 = i1[b] - jq[b];
        double nq2 = q1[b] + ji[b];
        i2 = 0.2 * ni2 + 0.8 * i2;
        q2 = 0.2 * nq2 + 0.8 * q2;

        re = 0.2 * (i2 * (i2 > 0 ? ni2 : -ni2) + q2 * nq2) + 0.8 * re;
        im = 0.2 * (i2 * nq2 - q2 * ni2) + 0.8 * im;

        if (fabs(im) > 1e-15 && fabs(re) > 1e-15)
            period = 2.0 * M_PI / atan2(im, re);
        if (period > 1.5 * smooth_period) period = 1.5 * smooth_period;
        if (period < 0.67 * smooth_period) period = 0.67 * smooth_period;
        if (period < 6) period = 6;
        if (period > 50) period = 50;
        smooth_period = 0.2 * period + 0.8 * smooth_period;

        if (fabs(i1[b]) > 1e-15)
            phase = atan2(q1[b], i1[b]) * 180.0 / M_PI;
        double delta_phase = phase - (bar >= 1 ? atan2(q1[b1], i1[b1]) * 180.0 / M_PI : phase);
        if (delta_phase < 1) delta_phase = 1;

        double a = fast_limit / delta_phase;
        if (a < slow_limit) a = slow_limit;
        if (a > fast_limit) a = fast_limit;

        mama[bar] = a * in[bar] + (1.0 - a) * (bar >= 1 ? mama[bar-1] : in[bar]);
        fama[bar] = 0.5 * a * mama[bar] + (1.0 - 0.5 * a) * (bar >= 1 ? fama[bar-1] : in[bar]);
    }
    return n;
}
