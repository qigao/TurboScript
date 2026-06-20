/* =========================================================================
 * Module table and factory
 * ========================================================================= */
#include "exprtk.h"
#include "exprtk_universe.h"
#include "exprtk_module.h"
#include "exprtk_mod_strategy.h"
#include "fin.h"
#include "simd_helpers.h"
#include "strategy_optimizer.h"
#include <math.h>
#include <stdlib.h>

#define VEC_ARG(args, i) ((args)[i].type == EXPRTK_VAL_VECTOR)
#define VEC_DATA(args, i) ((args)[i].data.vector.data)

static int cmp_double_asc(const void *lhs, const void *rhs) {
  const double a = *(const double *)lhs;
  const double b = *(const double *)rhs;
  return (a > b) - (a < b);
}

static double z_from_conf(double conf) {
  if (conf >= 0.999) return 3.09;
  if (conf >= 0.995) return 2.58;
  if (conf >= 0.99) return 2.33;
  if (conf >= 0.975) return 1.96;
  if (conf >= 0.95) return 1.645;
  if (conf >= 0.90) return 1.282;
  return 1.0;
}

static exprtk_value_t fn_var_hist(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && VEC_ARG(args, 0) && args[1].type == EXPRTK_VAL_NUMBER) {
    const double *ret = VEC_DATA(args, 0);
    const size_t n = args[0].data.vector.size;
    const double conf = args[1].data.number;
    if (n == 0) return exprtk_val_num(0);

    double *tmp = MEM_ALLOC_ARRAY(arena, double, n);
    if (!tmp) return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i) tmp[i] = ret[i];
    qsort(tmp, n, sizeof(double), cmp_double_asc);

    const double tail = 1.0 - conf;
    size_t idx = (size_t)floor(tail * (double)(n - 1));
    if (idx >= n) idx = n - 1;
    double var = -tmp[idx];
    if (var < 0) var = 0;
    return exprtk_val_num(var);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_var_param(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && VEC_ARG(args, 0) && args[1].type == EXPRTK_VAL_NUMBER) {
    const double *ret = VEC_DATA(args, 0);
    const size_t n = args[0].data.vector.size;
    if (n < 2) return exprtk_val_num(0);

    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) mean += ret[i];
    mean /= (double)n;

    double var = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double d = ret[i] - mean;
      var += d * d;
    }
    var /= (double)(n - 1);

    double out = z_from_conf(args[1].data.number) * sqrt(var) - mean;
    if (out < 0) out = 0;
    return exprtk_val_num(out);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cvar(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  if (!(argc == 2 && VEC_ARG(args, 0) && args[1].type == EXPRTK_VAL_NUMBER))
    return exprtk_val_num(0);

  exprtk_value_t vh = fn_var_hist(argc, args, env, arena);
  if (vh.type != EXPRTK_VAL_NUMBER || vh.data.number <= 0) return exprtk_val_num(0);

  const double var = vh.data.number;
  const double *ret = VEC_DATA(args, 0);
  const size_t n = args[0].data.vector.size;
  double sum = 0.0;
  size_t cnt = 0;
  for (size_t i = 0; i < n; ++i) {
    const double loss = -ret[i];
    if (loss >= var) {
      sum += loss;
      cnt++;
    }
  }
  return exprtk_val_num(cnt == 0 ? var : sum / (double)cnt);
}

static exprtk_value_t fn_kelly(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3 && args[0].type == EXPRTK_VAL_NUMBER && args[1].type == EXPRTK_VAL_NUMBER &&
      args[2].type == EXPRTK_VAL_NUMBER) {
    const double w = args[0].data.number;
    const double avg_win = args[1].data.number;
    const double avg_loss = args[2].data.number;
    if (avg_loss <= 0.0) return exprtk_val_num(0);
    const double b = avg_win / avg_loss;
    if (b <= 0.0) return exprtk_val_num(0);
    return exprtk_val_num(w - (1.0 - w) / b);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_drawdown(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && VEC_ARG(args, 0)) {
    const size_t n = args[0].data.vector.size;
    const double *eq = VEC_DATA(args, 0);
    if (n == 0) return exprtk_val_vec(NULL, 0);

    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out) return exprtk_val_num(0);
    double peak = eq[0];
    for (size_t i = 0; i < n; ++i) {
      if (eq[i] > peak) peak = eq[i];
      out[i] = (peak > 0.0) ? (peak - eq[i]) / peak : 0.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_drawdown_stats(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && VEC_ARG(args, 0)) {
    const size_t n = args[0].data.vector.size;
    const double *eq = VEC_DATA(args, 0);
    if (n == 0) return exprtk_val_num(0);

    double peak = eq[0];
    double max_dd = 0.0;
    size_t max_dur = 0;
    size_t cur_dur = 0;
    for (size_t i = 0; i < n; ++i) {
      if (eq[i] >= peak) {
        peak = eq[i];
        cur_dur = 0;
      } else {
        cur_dur++;
        if (cur_dur > max_dur) max_dur = cur_dur;
      }
      const double dd = (peak > 0.0) ? (peak - eq[i]) / peak : 0.0;
      if (dd > max_dd) max_dd = dd;
    }

    double *out = MEM_ALLOC_ARRAY(arena, double, 2);
    if (!out) return exprtk_val_num(0);
    out[0] = max_dd;
    out[1] = (double)max_dur;
    return exprtk_val_vec(out, 2);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_pf_min_variance(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && VEC_ARG(args, 0)) {
    const double *cov = VEC_DATA(args, 0);
    const size_t m = args[0].data.vector.size;
    const size_t n = (size_t)(sqrt((double)m) + 0.5);
    if (n > 0 && n * n == m) {
      double *weights = MEM_ALLOC_ARRAY(arena, double, n);
      if (!weights) return exprtk_val_num(0);
      exprtk_pf_min_variance(cov, n, weights, arena);
      return exprtk_val_vec(weights, n);
    }
  }
  return exprtk_val_num(0);
}

static const exprtk_func_entry_t strategy_entries[] = {
    /* --- Signal setters ------------------------------------------------- */
    {"buy", fn_buy},
    {"sell", fn_sell},
    {"flat", fn_flat},
    {"set_sl", fn_set_sl},
    {"set_tp", fn_set_tp},

    /* --- Position queries ----------------------------------------------- */
    {"pos", fn_pos},
    {"entry_px", fn_entry_px},
    {"is_long", fn_is_long},
    {"is_short", fn_is_short},
    {"is_flat", fn_is_flat},
    {"unrealized_pnl", fn_unrealized_pnl},

    /* --- Universe cross-section ----------------------------------------- */
    {"rank_pct", fn_rank_pct},
    {"vec_rank", fn_vec_rank},
    {"vec_top", fn_vec_top},
    {"vec_filter_gt", fn_vec_filter_gt},
    {"vec_filter_lt", fn_vec_filter_lt},
    {"vec_where", fn_vec_where},
    {"vec_sort_idx", fn_vec_sort_idx},
    {"vec_shift", fn_vec_shift},
    {"vec_corr", fn_vec_corr},

    /* --- Risk sizing ---------------------------------------------------- */
    {"kelly_size", fn_kelly_size},
    {"risk_size", fn_risk_size},
    {"atr_sl", fn_atr_sl},
    {"kelly", fn_kelly},
    {"var_hist", fn_var_hist},
    {"var_param", fn_var_param},
    {"cvar", fn_cvar},
    {"drawdown", fn_drawdown},
    {"drawdown_stats", fn_drawdown_stats},
    {"pf_min_variance", fn_pf_min_variance},

    /* --- Trade log queries --------------------------------------------- */
    {"num_trades", fn_num_trades},
    {"cum_pnl", fn_cum_pnl},
    {"win_rate", fn_win_rate},

    /* --- Bar helpers ---------------------------------------------------- */
    {"last", fn_last},
    {"prev", fn_prev},
    {"vec_sum", fn_vec_sum},
    {"vec_mean", fn_vec_mean},
    {"vec_std", fn_vec_std},
    {"vec_max", fn_vec_max},
    {"vec_min", fn_vec_min},
    {"vec_roll_max", fn_vec_roll_max},
    {"vec_roll_min", fn_vec_roll_min},
    {"vec_vmax", fn_vec_vmax},
    {"vec_vmin", fn_vec_vmin},
    {"vec_fill", fn_vec_fill},
    {"vec_at", fn_vec_at},
    {"vec_any", fn_vec_any},
    {"vec_all", fn_vec_all},
    {"vec_cross", fn_vec_cross},
    {"vec_resample", fn_vec_resample},

    /* Performance Metrics */
    {"sharpe", fn_sharpe},
    {"sortino", fn_sortino},
    {"calmar", fn_calmar},
    {"profit_factor", fn_profit_factor},
    {"expectancy", fn_expectancy},
    {"payoff_ratio", fn_payoff_ratio},
    {"max_dd_duration", fn_max_dd_duration},
    {"ulcer_index", fn_ulcer_index},
    {"information_ratio", fn_information_ratio},
    {"treynor", fn_treynor},

    /* Walk-Forward & Multi-Timeframe */
    {"resample_ohlcv", fn_resample_ohlcv},
    {"walk_forward", fn_walk_forward},

    /* Graph Algorithms (Network Analysis) */
    {"bellman_ford", exprtk_graph_bellman_ford},
    {"has_negative_cycle", exprtk_graph_has_negative_cycle},
    {"extract_path", exprtk_graph_extract_path},
    {"detect_arbitrage", exprtk_graph_detect_arbitrage},
    {"nash_game", exprtk_nash_game},
    {"nash_support_enumeration", exprtk_nash_support_enumeration},
    {"nash_vertex_enumeration", exprtk_nash_vertex_enumeration},
    {"nash_lemke_howson", exprtk_nash_lemke_howson},
    {"nash_fictitious_play", exprtk_nash_fictitious_play},
    {"nash_stochastic_fictitious_play", exprtk_nash_stochastic_fictitious_play},
    {"nash_discrete_replicator_dynamics", exprtk_nash_discrete_replicator_dynamics},
    {"nash_imitation_dynamics", exprtk_nash_imitation_dynamics},
    {"nash_regret_minimization", exprtk_nash_regret_minimization},
    {"nash_introspection_dynamics", exprtk_nash_introspection_dynamics},
    {"nash_repeated_game", exprtk_nash_repeated_game},
    {"nash_moran_process", exprtk_nash_moran_process},
    {"nash_replicator_mutation", exprtk_nash_replicator_mutation},
    {"nash_asymmetric_replicator", exprtk_nash_asymmetric_replicator},
    {"nash_asymmetric_replicator_derivative", exprtk_nash_asymmetric_replicator_derivative},
    {"nash_replicator", exprtk_nash_replicator},

    /* --- Universe (Real) ------------------------------------------------ */
    {"univ_is_active", fn_universe_is_active},
    {"univ_active_count", fn_universe_active_count},
    {"univ_adj_factor", fn_universe_adj_factor},
    {"univ_adjust_price", fn_universe_adjust_price},
    {"univ_adjust_prices", fn_universe_adjust_prices},
    {"univ_rank", fn_universe_rank},
    {"univ_top_n", fn_universe_top_n},
    {"univ_filter_gt", fn_universe_filter_gt},
    {"univ_zscore", fn_universe_zscore},
    {"univ_clip", fn_universe_clip},
    {"univ_cross_sum", fn_universe_cross_sum},
    {"univ_demean", fn_universe_demean},

};

static const exprtk_module_t strategy_module = {
    "strategy", strategy_entries, sizeof(strategy_entries) / sizeof(strategy_entries[0])};

const exprtk_module_t *exprtk_module_strategy(void) { return &strategy_module; }

