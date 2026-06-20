#ifndef EXPRTK_MOD_STRATEGY_H
#define EXPRTK_MOD_STRATEGY_H

#include "exprtk.h"
#include "exprtk_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exported function prototypes */
exprtk_value_t fn_buy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_sell(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_flat(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_set_sl(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_set_tp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_set_trailing_sl(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_limit_order(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_stop_order(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_rank_pct(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_top(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_filter_gt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_filter_lt(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_where(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_sort_idx(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_shift(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_corr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_pos(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_entry_px(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_is_long(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_is_short(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_is_flat(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_unrealized_pnl(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_kelly_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_risk_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_atr_sl(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_num_trades(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_cum_pnl(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_win_rate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_last(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_prev(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_std(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_roll_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_roll_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_vmax(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_vmin(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_fill(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_at(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_any(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_cross(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_vec_resample(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_sharpe(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_sortino(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_calmar(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_profit_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_expectancy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_payoff_ratio(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_max_dd_duration(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_ulcer_index(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_information_ratio(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_treynor(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_resample_ohlcv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t fn_walk_forward(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);

/* Module entry point */
const exprtk_module_t *exprtk_module_strategy(void);

#ifdef __cplusplus
}
#endif

#endif /* EXPRTK_MOD_STRATEGY_H */
/* Graph Algorithms */
exprtk_value_t exprtk_graph_bellman_ford(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_graph_has_negative_cycle(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_graph_extract_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_graph_detect_arbitrage(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_game(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_support_enumeration(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_vertex_enumeration(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_lemke_howson(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_fictitious_play(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_stochastic_fictitious_play(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_discrete_replicator_dynamics(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_imitation_dynamics(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_regret_minimization(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_introspection_dynamics(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_repeated_game(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_moran_process(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_replicator_mutation(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_asymmetric_replicator(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_asymmetric_replicator_derivative(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
exprtk_value_t exprtk_nash_replicator(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena);
