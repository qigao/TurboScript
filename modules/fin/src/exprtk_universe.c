#include "exprtk_universe.h"
#include "universe.h"
#include "exprtk_types.h"
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

exprtk_value_t fn_universe_is_active(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    bool active = universe_is_active(u, (uint32_t)args[0].data.number);
    return exprtk_val_num(active ? 1.0 : 0.0);
}

exprtk_value_t fn_universe_active_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)argc; (void)args; (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u) return exprtk_val_num(0.0);
    return exprtk_val_num((double)universe_active_count(u));
}

exprtk_value_t fn_universe_adj_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(1.0);
    return exprtk_val_num(universe_adj_factor(u, (uint32_t)args[0].data.number));
}

exprtk_value_t fn_universe_adjust_price(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_NUMBER) 
        return exprtk_val_num(0.0);
    return exprtk_val_num(universe_adjust_price(u, (uint32_t)args[0].data.number, args[1].data.number));
}

exprtk_value_t fn_universe_adjust_prices(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_adjust_prices(u, args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_rank(u, args[0].data.vector.data, n, out);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_top_n(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    size_t k = (size_t)args[1].data.number;
    uint32_t *out_ids = MEM_ALLOC_ARRAY(arena, uint32_t, k);
    if (!out_ids) return exprtk_val_num(0.0);
    size_t written = universe_top_n(u, args[0].data.vector.data, n, k, out_ids);
    double *out = MEM_ALLOC_ARRAY(arena, double, written);
    if (!out) return exprtk_val_num(0.0);
    for (size_t i = 0; i < written; i++) {
        out[i] = (double)out_ids[i];
    }
    return exprtk_val_vec(out, written);
}

exprtk_value_t fn_universe_filter_gt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double threshold = args[1].data.number;
    uint8_t *mask = MEM_ALLOC_ARRAY(arena, uint8_t, n);
    if (!mask) return exprtk_val_num(0.0);
    universe_filter_gt(u, args[0].data.vector.data, n, threshold, mask);
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    for (size_t i = 0; i < n; i++) {
        out[i] = mask[i] ? 1.0 : 0.0;
    }
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_zscore(u, args[0].data.vector.data, n, out);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_clip(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 3 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER || args[2].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double lo = args[1].data.number;
    double hi = args[2].data.number;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_clip(u, args[0].data.vector.data, n, lo, hi, out);
    return exprtk_val_vec(out, n);
}

exprtk_value_t fn_universe_cross_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    return exprtk_val_num(universe_cross_sum(u, args[0].data.vector.data, n));
}

exprtk_value_t fn_universe_demean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    universe_t *u = (universe_t *)env->user_data;
    if (!u || argc < 1 || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0.0);
    size_t n = args[0].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0.0);
    universe_demean(u, args[0].data.vector.data, n, out);
    return exprtk_val_vec(out, n);
}
