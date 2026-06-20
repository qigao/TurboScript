#ifndef EXPRTK_UNIVERSE_H
#define EXPRTK_UNIVERSE_H

#include "exprtk.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

exprtk_value_t fn_universe_is_active(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_active_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_adj_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_adjust_price(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_adjust_prices(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_top_n(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_filter_gt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_clip(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_cross_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_universe_demean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);

#ifdef __cplusplus
}
#endif

#endif
