/**
 * @file exprtk_mod_stats.c
 * @brief Stats module: median/percentile/skewness/kurtosis/geometric_mean/
 *        harmonic_mean/zscore/wmean/wvar/ewma/ewmvar/covariance/cumsum/
 *        cumprod/rank/histogram
 */
#include "exprtk_module.h"
#include "exprtk.h"
#include "simd_helpers.h"



static exprtk_value_t fn_median(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
        return exprtk_val_num(exprtk_median(args[0].data.vector.data, args[0].data.vector.size, arena));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_percentile(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER)
        return exprtk_val_num(exprtk_percentile(args[0].data.vector.data, args[0].data.vector.size, args[1].data.number, arena));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_skewness(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
        return exprtk_val_num(exprtk_skewness(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_kurtosis(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
        return exprtk_val_num(exprtk_kurtosis(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_geometric_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
        return exprtk_val_num(exprtk_geometric_mean(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_harmonic_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
        return exprtk_val_num(exprtk_harmonic_mean(args[0].data.vector.data, args[0].data.vector.size));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n >= 2) {
            // Use SIMD-optimized mean and variance calculation
            double mean, variance;
            simd_mean_variance_avx(args[0].data.vector.data, n, &mean, &variance);

            double sd = sqrt(variance);
            if (sd > 1e-15) {
                double *res = MEM_ALLOC_ARRAY(arena, double, n);
                if (res) {
                    // Use SIMD-optimized z-score calculation
                    simd_zscore_avx(args[0].data.vector.data, res, n, mean, sd);
                    return exprtk_val_vec(res, n);
                }
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_wmean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0 && args[1].data.vector.size >= n) {
            // Use SIMD-optimized operations
            double sw = simd_sum_avx(args[1].data.vector.data, n);
            double swx = simd_dot_avx(args[1].data.vector.data, args[0].data.vector.data, n);
            if (fabs(sw) > 1e-15) return exprtk_val_num(swx / sw);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_wvar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n >= 2 && args[1].data.vector.size >= n) {
            double sw = 0, swx = 0;
            for (size_t i = 0; i < n; ++i) { sw += args[1].data.vector.data[i]; swx += args[1].data.vector.data[i] * args[0].data.vector.data[i]; }
            if (fabs(sw) > 1e-15) {
                double wm = swx / sw, num = 0;
                for (size_t i = 0; i < n; ++i) { double d = args[0].data.vector.data[i] - wm; num += args[1].data.vector.data[i] * d * d; }
                return exprtk_val_num(num / sw);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ewma(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        double alpha = args[1].data.number;
        if (alpha > 1.0) alpha = 2.0 / (alpha + 1.0);
        if (n > 0) {
            double *res = MEM_ALLOC_ARRAY(arena, double, n);
            if (res) {
                res[0] = args[0].data.vector.data[0];
                for (size_t i = 1; i < n; ++i)
                    res[i] = alpha * args[0].data.vector.data[i] + (1.0 - alpha) * res[i - 1];
                return exprtk_val_vec(res, n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ewmvar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        double alpha = args[1].data.number;
        if (alpha > 1.0) alpha = 2.0 / (alpha + 1.0);
        if (n >= 2) {
            double *res = MEM_ALLOC_ARRAY(arena, double, n);
            if (res) {
                double ema = args[0].data.vector.data[0];
                res[0] = 0;
                for (size_t i = 1; i < n; ++i) {
                    double diff = args[0].data.vector.data[i] - ema;
                    ema = alpha * args[0].data.vector.data[i] + (1.0 - alpha) * ema;
                    res[i] = (1.0 - alpha) * (res[i - 1] + alpha * diff * diff);
                }
                return exprtk_val_vec(res, n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_covariance(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n >= 2 && args[1].data.vector.size >= n) {
            // Use SIMD-optimized sum
            double sx = simd_sum_avx(args[0].data.vector.data, n);
            double sy = simd_sum_avx(args[1].data.vector.data, n);
            double mx = sx / (double)n;
            double my = sy / (double)n;

            // Compute covariance: E[(X - mx)(Y - my)]
            // This can be rewritten as: E[XY] - mx*my
            double sxy = simd_dot_avx(args[0].data.vector.data, args[1].data.vector.data, n);
            double cov = (sxy - n * mx * my) / (double)(n - 1);

            return exprtk_val_num(cov);
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cumsum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0) {
            double *res = MEM_ALLOC_ARRAY(arena, double, n);
            if (res) {
                res[0] = args[0].data.vector.data[0];
                for (size_t i = 1; i < n; ++i) res[i] = res[i - 1] + args[0].data.vector.data[i];
                return exprtk_val_vec(res, n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_cumprod(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0) {
            double *res = MEM_ALLOC_ARRAY(arena, double, n);
            if (res) {
                res[0] = args[0].data.vector.data[0];
                for (size_t i = 1; i < n; ++i) res[i] = res[i - 1] * args[0].data.vector.data[i];
                return exprtk_val_vec(res, n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        if (n > 0) {
            double *res = MEM_ALLOC_ARRAY(arena, double, n);
            rank_item_t *items = MEM_ALLOC_ARRAY(arena, rank_item_t, n);
            if (res && items) {
                for (size_t i = 0; i < n; ++i) { items[i].idx = i; items[i].val = args[0].data.vector.data[i]; }
                qsort(items, n, sizeof(rank_item_t), compare_rank_items);
                size_t i = 0;
                while (i < n) {
                    size_t j = i + 1;
                    while (j < n && fabs(items[j].val - items[i].val) < 1e-15) ++j;
                    double avg_rank = (double)(i + j + 1) / 2.0;
                    for (size_t k = i; k < j; ++k) res[items[k].idx] = avg_rank;
                    i = j;
                }
                return exprtk_val_vec(res, n);
            }
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_histogram(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
        size_t n = args[0].data.vector.size;
        size_t nbins = (size_t)args[1].data.number;
        if (n > 0 && nbins > 0) {
            double lo = args[0].data.vector.data[0], hi = lo;
            for (size_t i = 1; i < n; ++i) {
                if (args[0].data.vector.data[i] < lo) lo = args[0].data.vector.data[i];
                if (args[0].data.vector.data[i] > hi) hi = args[0].data.vector.data[i];
            }
            double *res = MEM_ALLOC_ARRAY(arena, double, nbins);
            if (res) {
                memset(res, 0, nbins * sizeof(double));
                double range = hi - lo;
                if (range < 1e-15) {
                    res[0] = (double)n;
                } else {
                    double bin_width = range / (double)nbins;
                    for (size_t i = 0; i < n; ++i) {
                        size_t b = (size_t)((args[0].data.vector.data[i] - lo) / bin_width);
                        if (b >= nbins) b = nbins - 1;
                        res[b] += 1.0;
                    }
                }
                return exprtk_val_vec(res, nbins);
            }
        }
    }
    return exprtk_val_num(0);
}

static const exprtk_func_entry_t stats_entries[] = {
    { "covariance",     fn_covariance },
    { "cumprod",        fn_cumprod },
    { "cumsum",         fn_cumsum },
    { "ewma",           fn_ewma },
    { "ewmvar",         fn_ewmvar },
    { "geometric_mean", fn_geometric_mean },
    { "harmonic_mean",  fn_harmonic_mean },
    { "histogram",      fn_histogram },
    { "kurtosis",       fn_kurtosis },
    { "median",         fn_median },
    { "percentile",     fn_percentile },
    { "rank",           fn_rank },
    { "skewness",       fn_skewness },
    { "wmean",          fn_wmean },
    { "wvar",           fn_wvar },
    { "zscore",         fn_zscore },
};

static const exprtk_module_t stats_module = {
    "stats", stats_entries, sizeof(stats_entries) / sizeof(stats_entries[0])
};

const exprtk_module_t *exprtk_module_stats(void) { return &stats_module; }
