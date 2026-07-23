/**
 * @file exprtk_mod_strategy.c
 * @brief TurboScript "strategy" module — event-driven trading primitives.
 *
 * This module is mounted into the TurboScript environment by the strategy
 * runner before each bar evaluation.  It exposes functions that scripts can
 * call to express their intent without knowing anything about the underlying
 * C order manager.
 *
 * ── Design principle ────────────────────────────────────────────────────────
 *
 * The script does NOT call order functions directly.  Instead it sets output
 * variables that the strategy runner reads after exprtk_eval() returns:
 *
 *   signal      — +1 buy, -1 sell, 0 flat (no trade)
 *   size        — fraction of equity (0.0 – 1.0]
 *   stop_loss   — absolute price level (0 to disable / keep previous)
 *   take_profit — absolute price level (0 to disable / keep previous)
 *
 * This module provides:
 *   (a) CONVENIENCE wrappers: buy(), sell(), flat(), set_sl(), set_tp() that
 *       write those env vars in one call (idiomatic scripting UX).
 *   (b) UNIVERSE cross-section helpers: rank(), top_n(), filter_gt() that
 *       operate on score vectors and return result vectors.
 *   (c) POSITION query helpers: pos(), entry_px() that read the current
 *       position/entry-price scalars (already bound as env vars by the runner,
 *       but these helper functions accept dynamic asset-id arguments).
 *   (d) RISK sizing helpers: kelly_size(), fixed_frac_size() callable from script.
 *   (e) TRADE log scalars: cum_pnl(), num_trades(), win_rate() for
 *       adaptive strategy logic (e.g., reducing size after a losing streak).
 *
 * ── Module name: "strategy" ─────────────────────────────────────────────────
 *
 * Scripts access these as plain function calls (not namespaced), since the
 * module is mounted at the root env:
 *
 *   var rsi_val = last(rsi(C, 14));
 *   if (rsi_val < 30) {
 *       buy(0.1, C[n-1] * 0.95, C[n-1] * 1.10);
 *   } else if (rsi_val > 70 && pos() > 0) {
 *       sell(1.0, 0, 0);
 *   }
 */

#include "exprtk.h"
#include "exprtk_mod_strategy.h"
#include "exprtk_module.h"
#include "fin.h"
#include "simd_helpers.h"
#include "strategy_optimizer.h"
#include <math.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>

/* =========================================================================
 * Helpers: read/write named scalars in the env
 * ========================================================================= */
static double env_get_num(exprtk_env_t *env, const char *name, double def) {
  exprtk_value_t v = exprtk_env_get(env, name);
  return (v.type == EXPRTK_VAL_NUMBER) ? v.data.number : def;
}

static void env_set_num(exprtk_env_t *env, const char *name, double val) {
  exprtk_env_set(env, name, exprtk_val_num(val));
}

/* =========================================================================
 * (a) Convenience signal setters
 * ========================================================================= */

/**
 * buy([size=0.1, [stop_loss=0, [take_profit=0]]])
 * Sets signal=+1, size=..., stop_loss=..., take_profit=...
 */
exprtk_value_t fn_buy(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)arena;
  double sz = (argc >= 1 && args[0].type == EXPRTK_VAL_NUMBER) ? args[0].data.number : 0.1;
  double sl = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) ? args[1].data.number : 0.0;
  double tp = (argc >= 3 && args[2].type == EXPRTK_VAL_NUMBER) ? args[2].data.number : 0.0;
  env_set_num(env, "signal", 1.0);
  env_set_num(env, "size", sz);
  env_set_num(env, "stop_loss", sl);
  env_set_num(env, "take_profit", tp);
  return exprtk_val_num(1.0);
}

/**
 * sell([size=1.0, [stop_loss=0, [take_profit=0]]])
 * Sets signal=-1 (close long or open short if allowed by market rules).
 */
exprtk_value_t fn_sell(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)arena;
  double sz = (argc >= 1 && args[0].type == EXPRTK_VAL_NUMBER) ? args[0].data.number : 1.0;
  double sl = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) ? args[1].data.number : 0.0;
  double tp = (argc >= 3 && args[2].type == EXPRTK_VAL_NUMBER) ? args[2].data.number : 0.0;
  env_set_num(env, "signal", -1.0);
  env_set_num(env, "size", sz);
  env_set_num(env, "stop_loss", sl);
  env_set_num(env, "take_profit", tp);
  return exprtk_val_num(-1.0);
}

/**
 * flat()
 * Signal = 0 → close any open position, submit no new trade.
 */
exprtk_value_t fn_flat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  env_set_num(env, "signal", 0.0);
  return exprtk_val_num(0.0);
}

/**
 * set_sl(price)  — update stop-loss without changing signal
 */
exprtk_value_t fn_set_sl(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_NUMBER)
    env_set_num(env, "stop_loss", args[0].data.number);
  return exprtk_val_num(env_get_num(env, "stop_loss", 0.0));
}

/**
 * set_tp(price)  — update take-profit without changing signal
 */
exprtk_value_t fn_set_tp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_NUMBER)
    env_set_num(env, "take_profit", args[0].data.number);
  return exprtk_val_num(env_get_num(env, "take_profit", 0.0));
}

/**
 * set_trailing_sl(distance, is_pct)  — update trailing stop-loss
 */
exprtk_value_t fn_set_trailing_sl(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_NUMBER) {
    env_set_num(env, "trailing_dist", args[0].data.number);
  }
  if (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) {
    env_set_num(env, "trailing_pct", args[1].data.number);
  }
  return exprtk_val_num(env_get_num(env, "trailing_dist", 0.0));
}

/**
 * limit_order(signal, limit_price, bars_valid)
 */
exprtk_value_t fn_limit_order(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)arena;
  if (argc >= 2) {
    env_set_num(env, "signal", args[0].data.number);
    env_set_num(env, "signal_type", 1.0);
    env_set_num(env, "signal_price", args[1].data.number);
  }
  if (argc >= 3) {
    env_set_num(env, "bars_valid", args[2].data.number);
  }
  return exprtk_val_num(1.0);
}

/**
 * stop_order(signal, stop_price, bars_valid)
 */
exprtk_value_t fn_stop_order(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)arena;
  if (argc >= 2) {
    env_set_num(env, "signal", args[0].data.number);
    env_set_num(env, "signal_type", 2.0);
    env_set_num(env, "signal_price", args[1].data.number);
  }
  if (argc >= 3) {
    env_set_num(env, "bars_valid", args[2].data.number);
  }
  return exprtk_val_num(1.0);
}

/* =========================================================================
 * (b) Universe cross-section helpers
 * scalar-input versions suitable for scripting (per-asset invocation)
 * ========================================================================= */

/**
 * rank_pct(score, min_score, max_score)
 * → Linearly normalises score into [0,1] relative to known min/max range.
 * Typically called from inside a loop over all active assets.
 */
exprtk_value_t fn_rank_pct(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 3) {
    double v = args[0].data.number;
    double lo = args[1].data.number;
    double hi = args[2].data.number;
    double rng = hi - lo;
    return exprtk_val_num((rng > 1e-12) ? (v - lo) / rng : 0.5);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_rank(scores[])
 * → Returns a vector of percentile ranks (0..1) for each element.
 *   Elements equal to 0 are treated as inactive and returned as 0.
 */
exprtk_value_t fn_vec_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *src = args[0].data.vector.data;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    /* Count active (non-zero), find min/max */
    double mn = 1e300, mx = -1e300;
    size_t active = 0;
    for (size_t i = 0; i < n; i++) {
      if (src[i] != 0.0) {
        if (src[i] < mn)
          mn = src[i];
        if (src[i] > mx)
          mx = src[i];
        active++;
      }
    }

    double rng = mx - mn;
    for (size_t i = 0; i < n; i++) {
      if (src[i] == 0.0 || active == 0) {
        out[i] = 0.0;
        continue;
      }
      out[i] = (rng > 1e-12) ? (src[i] - mn) / rng : 0.5;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_top(scores[], k)
 * → Returns a binary mask vector: 1 for the k highest non-zero scores.
 */
exprtk_value_t fn_vec_top(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    size_t k = (size_t)args[1].data.number;
    double *src = args[0].data.vector.data;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    for (size_t i = 0; i < n; i++)
      out[i] = 0.0;

    /* Simple O(n*k) selection — fast enough for universe sizes < 5000 */
    for (size_t t = 0; t < k; t++) {
      double best = -1e300;
      size_t best_i = SIZE_MAX;
      for (size_t i = 0; i < n; i++) {
        if (out[i] == 0.0 && src[i] != 0.0 && src[i] > best) {
          best = src[i];
          best_i = i;
        }
      }
      if (best_i != SIZE_MAX)
        out[best_i] = 1.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_filter_gt(values[], threshold)
 * → Binary mask: 1 where value > threshold AND value != 0 (active).
 */
exprtk_value_t fn_vec_filter_gt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double thr = args[1].data.number;
    double *src = args[0].data.vector.data;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);
    for (size_t i = 0; i < n; i++)
      out[i] = (src[i] != 0.0 && src[i] > thr) ? 1.0 : 0.0;
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_filter_lt(values[], threshold)
 * → Binary mask: 1 where value < threshold AND value != 0 (active).
 */
exprtk_value_t fn_vec_filter_lt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double thr = args[1].data.number;
    double *src = args[0].data.vector.data;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);
    for (size_t i = 0; i < n; i++)
      out[i] = (src[i] != 0.0 && src[i] < thr) ? 1.0 : 0.0;
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_where(mask[])
 * -> Returns a vector of indices where mask[i] != 0.
 * Useful for turning a binary filter into a list of targets to iterate over.
 */
exprtk_value_t fn_vec_where(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *mask = args[0].data.vector.data;

    /* First pass: count */
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
      if (fabs(mask[i]) > 1e-9)
        count++;
    }

    double *out = MEM_ALLOC_ARRAY(arena, double, count);
    if (!out)
      return exprtk_val_num(0.0);

    /* Second pass: collect */
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
      if (fabs(mask[i]) > 1e-9)
        out[j++] = (double)i;
    }
    return exprtk_val_vec(out, count);
  }
  return exprtk_val_num(0.0);
}

typedef struct {
  size_t idx;
  double val;
} sort_pair_t;

static int sort_pair_cmp(const void *a, const void *b) {
  double va = ((const sort_pair_t *)a)->val;
  double vb = ((const sort_pair_t *)b)->val;
  if (va < vb)
    return 1; /* Descending */
  if (va > vb)
    return -1;
  return 0;
}

/**
 * vec_sort_idx(values[])
 * -> Returns a vector of indices [0...N-1] sorted by values[i] descending.
 */
exprtk_value_t fn_vec_sort_idx(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *src = args[0].data.vector.data;
    if (n == 0)
      return args[0];

    sort_pair_t *pairs = MEM_ALLOC_ARRAY(arena, sort_pair_t, n);
    if (!pairs)
      return exprtk_val_num(0.0);

    for (size_t i = 0; i < n; i++) {
      pairs[i].idx = i;
      pairs[i].val = src[i];
    }

    qsort(pairs, n, sizeof(sort_pair_t), sort_pair_cmp);

    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    for (size_t i = 0; i < n; i++) {
      out[i] = (double)pairs[i].idx;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_shift(v[], n)
 * -> Shifts vector by n positions. Pad with 0.
 * Positive n shifts right (forward in time), negative n shifts left (back in time).
 */
exprtk_value_t fn_vec_shift(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = args[0].data.vector.size;
    double *src = args[0].data.vector.data;
    int shift = (int)args[1].data.number;

    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);
    memset(out, 0, n * sizeof(double));

    for (int i = 0; i < (int)n; i++) {
      int target = i + shift;
      if (target >= 0 && target < (int)n) {
        out[target] = src[i];
      }
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_corr(v1[], v2[])
 * -> Returns Pearson correlation coefficient [-1, 1].
 */
exprtk_value_t fn_vec_corr(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n1 = args[0].data.vector.size;
    size_t n2 = args[1].data.vector.size;
    size_t n = (n1 < n2) ? n1 : n2;
    if (n < 2)
      return exprtk_val_num(0.0);

    double *v1 = args[0].data.vector.data;
    double *v2 = args[1].data.vector.data;

    double sum1 = 0, sum2 = 0, sum1sq = 0, sum2sq = 0, sum_prod = 0;
    for (size_t i = 0; i < n; i++) {
      sum1 += v1[i];
      sum2 += v2[i];
      sum1sq += v1[i] * v1[i];
      sum2sq += v2[i] * v2[i];
      sum_prod += v1[i] * v2[i];
    }

    double num = (double)n * sum_prod - (sum1 * sum2);
    double den = sqrt(((double)n * sum1sq - sum1 * sum1) * ((double)n * sum2sq - sum2 * sum2));

    if (fabs(den) < 1e-12)
      return exprtk_val_num(0.0);
    return exprtk_val_num(num / den);
  }
  return exprtk_val_num(0.0);
}

/* =========================================================================
 * (c) Position query helpers
 * ========================================================================= */

/**
 * pos()
 * → Returns the current position quantity (from the env "position" var).
 * The runner binds "position" before each bar, so this just proxies it.
 */
exprtk_value_t fn_pos(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  return exprtk_val_num(env_get_num(env, "position", 0.0));
}

/**
 * entry_px()
 * → Returns the average entry price (runner binds "entry_price" in env).
 */
exprtk_value_t fn_entry_px(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  return exprtk_val_num(env_get_num(env, "entry_price", 0.0));
}

/**
 * is_long()   → 1 if position > 0, else 0.
 */
exprtk_value_t fn_is_long(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  return exprtk_val_num(env_get_num(env, "position", 0.0) > 0.0 ? 1.0 : 0.0);
}

/**
 * is_short()  → 1 if position < 0, else 0.
 */
exprtk_value_t fn_is_short(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  return exprtk_val_num(env_get_num(env, "position", 0.0) < 0.0 ? 1.0 : 0.0);
}

/**
 * is_flat()  → 1 if position == 0, else 0.
 */
exprtk_value_t fn_is_flat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  double p = env_get_num(env, "position", 0.0);
  return exprtk_val_num((p > -1e-9 && p < 1e-9) ? 1.0 : 0.0);
}

/**
 * unrealized_pnl()
 * → (current_close - entry_price) * position  (inferred from env vars)
 */
exprtk_value_t fn_unrealized_pnl(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  double pos = env_get_num(env, "position", 0.0);
  double entry = env_get_num(env, "entry_price", 0.0);
  double close = env_get_num(env, "prev_close", 0.0); /* last known close */
  return exprtk_val_num((close - entry) * pos);
}

/* =========================================================================
 * (d) Risk sizing helpers
 * ========================================================================= */

/**
 * kelly_size(win_rate, avg_win, avg_loss)
 * → Kelly fraction: w - (1-w)/ratio, clamped to [0,1].
 */
exprtk_value_t fn_kelly_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 3) {
    double w = args[0].data.number; /* win rate */
    double avg_win = args[1].data.number;
    double avg_loss = args[2].data.number;
    if (avg_loss < 1e-9)
      return exprtk_val_num(0.0);
    double ratio = avg_win / avg_loss;
    double k = w - (1.0 - w) / ratio;
    if (k < 0.0)
      k = 0.0;
    if (k > 1.0)
      k = 1.0;
    return exprtk_val_num(k);
  }
  return exprtk_val_num(0.0);
}

/**
 * risk_size(risk_pct, entry, stop_loss)
 * → Fraction of equity to allocate so that a full stop-loss hit costs risk_pct.
 *   size = risk_pct / |entry - stop_loss| × entry
 */
exprtk_value_t fn_risk_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 3) {
    double risk = args[0].data.number; /* e.g. 0.01 for 1% */
    double entry = args[1].data.number;
    double sl = args[2].data.number;
    double dist = fabs(entry - sl);
    if (dist < 1e-9 || entry < 1e-9)
      return exprtk_val_num(0.0);
    double sz = risk / (dist / entry);
    if (sz > 1.0)
      sz = 1.0;
    return exprtk_val_num(sz);
  }
  return exprtk_val_num(0.0);
}

/**
 * atr_sl(atr_val, multiplier)
 * → stop_loss distance = atr_val * multiplier.
 * Convenience: caller subtracts from entry to get the SL price.
 */
exprtk_value_t fn_atr_sl(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 2)
    return exprtk_val_num(args[0].data.number * args[1].data.number);
  if (argc == 1)
    return exprtk_val_num(args[0].data.number * 2.0); /* default 2× ATR */
  return exprtk_val_num(0.0);
}

/* =========================================================================
 * (e) Trade-log query helpers
 * These read vars the runner updates at end-of-day into the env:
 *   "num_trades", "num_wins", "cum_pnl"
 * ========================================================================= */

/**
 * num_trades()  → total closed trades so far.
 */
exprtk_value_t fn_num_trades(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  return exprtk_val_num(env_get_num(env, "num_trades", 0.0));
}

/**
 * cum_pnl()  → cumulative realized P&L.
 */
exprtk_value_t fn_cum_pnl(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  return exprtk_val_num(env_get_num(env, "cum_pnl", 0.0));
}

/**
 * win_rate()  → fraction of closed trades that were profitable (0..1).
 */
exprtk_value_t fn_win_rate(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)arena;
  double n_total = env_get_num(env, "num_trades", 0.0);
  double n_wins = env_get_num(env, "num_wins", 0.0);
  return exprtk_val_num((n_total > 0.5) ? (n_wins / n_total) : 0.0);
}

/* =========================================================================
 * (f) Bar helpers (convenience; C[] and n are already in scope as env vars)
 * ========================================================================= */

/**
 * last(vec[]) → return the last element of a vector.
 * Equivalent to vec[n-1] but works without knowing n in script.
 */
exprtk_value_t fn_last(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size > 0) {
    size_t n = args[0].data.vector.size;
    return exprtk_val_num(args[0].data.vector.data[n - 1]);
  }
  return exprtk_val_num(0.0);
}

/**
 * prev(vec[], k=1) → return the k-th element from the end (0 = most recent).
 */
exprtk_value_t fn_prev(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    size_t k = (argc >= 2) ? (size_t)args[1].data.number : 1;
    if (n > k)
      return exprtk_val_num(args[0].data.vector.data[n - 1 - k]);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_sum(vec[]) → sum of all elements.
 */
exprtk_value_t fn_vec_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    return exprtk_val_num(simd_sum(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_mean(vec[]) → arithmetic mean.
 */
exprtk_value_t fn_vec_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size > 0) {
    size_t n = args[0].data.vector.size;
    double sum = simd_sum(args[0].data.vector.data, n);
    return exprtk_val_num(sum / (double)n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_std(vec[]) → population std-dev.
 */
exprtk_value_t fn_vec_std(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size > 1) {
    size_t n = args[0].data.vector.size;
    double *d = args[0].data.vector.data;
    double mean, variance;
    simd_mean_variance(d, n, &mean, &variance);
    double pop_var = (n > 1) ? variance * (double)(n - 1) / (double)n : 0.0;
    return exprtk_val_num(sqrt(pop_var));
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_max(vec[]) / vec_min(vec[])
 */
exprtk_value_t fn_vec_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size > 0) {
    return exprtk_val_num(simd_max(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0.0);
}

exprtk_value_t fn_vec_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size > 0) {
    return exprtk_val_num(simd_min(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_roll_max(v[], window)
 * -> Returns a vector of rolling maximums.
 */
exprtk_value_t fn_vec_roll_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = args[0].data.vector.size;
    int win = (int)args[1].data.number;
    if (win <= 0 || n == 0)
      return args[0];

    double *src = args[0].data.vector.data;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    for (int i = 0; i < (int)n; i++) {
      int start = i - win + 1;
      if (start < 0)
        start = 0;
      size_t len = (size_t)(i - start + 1);
      out[i] = simd_max(&src[start], len);
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_roll_min(v[], window)
 */
exprtk_value_t fn_vec_roll_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = args[0].data.vector.size;
    int win = (int)args[1].data.number;
    if (win <= 0 || n == 0)
      return args[0];

    double *src = args[0].data.vector.data;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    for (int i = 0; i < (int)n; i++) {
      int start = i - win + 1;
      if (start < 0)
        start = 0;
      size_t len = (size_t)(i - start + 1);
      out[i] = simd_min(&src[start], len);
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_vmax(v1[], v2[])
 * -> Element-wise maximum of two vectors (or vector and scalar).
 */
exprtk_value_t fn_vec_vmax(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc >= 2) {
    size_t n = 0;
    double *v1 = NULL, *v2 = NULL;
    double s1 = 0, s2 = 0;
    int is_v1 = (args[0].type == EXPRTK_VAL_VECTOR);
    int is_v2 = (args[1].type == EXPRTK_VAL_VECTOR);

    if (is_v1) {
      n = args[0].data.vector.size;
      v1 = args[0].data.vector.data;
    } else {
      s1 = args[0].data.number;
    }
    if (is_v2) {
      if (n == 0)
        n = args[1].data.vector.size;
      else if (n != args[1].data.vector.size)
        return exprtk_val_num(0.0);
      v2 = args[1].data.vector.data;
    } else {
      s2 = args[1].data.number;
    }

    if (n == 0)
      return exprtk_val_num(fmax(s1, s2));

    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    if (is_v1 && is_v2) {
      simd_element_max(v1, v2, out, n);
    } else if (is_v1) {
      simd_element_max_scalar(v1, s2, out, n);
    } else if (is_v2) {
      simd_element_max_scalar(v2, s1, out, n);
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_vmin(v1[], v2[])
 */
exprtk_value_t fn_vec_vmin(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc >= 2) {
    size_t n = 0;
    double *v1 = NULL, *v2 = NULL;
    double s1 = 0, s2 = 0;
    int is_v1 = (args[0].type == EXPRTK_VAL_VECTOR);
    int is_v2 = (args[1].type == EXPRTK_VAL_VECTOR);

    if (is_v1) {
      n = args[0].data.vector.size;
      v1 = args[0].data.vector.data;
    } else {
      s1 = args[0].data.number;
    }
    if (is_v2) {
      if (n == 0)
        n = args[1].data.vector.size;
      else if (n != args[1].data.vector.size)
        return exprtk_val_num(0.0);
      v2 = args[1].data.vector.data;
    } else {
      s2 = args[1].data.number;
    }

    if (n == 0)
      return exprtk_val_num(fmin(s1, s2));

    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);

    if (is_v1 && is_v2) {
      simd_element_min(v1, v2, out, n);
    } else if (is_v1) {
      simd_element_min_scalar(v1, s2, out, n);
    } else if (is_v2) {
      simd_element_min_scalar(v2, s1, out, n);
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_fill(n, val)
 * -> Creates a vector of size n filled with val.
 */
exprtk_value_t fn_vec_fill(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_NUMBER && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = (size_t)args[0].data.number;
    double val = args[1].data.number;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);
    for (size_t i = 0; i < n; i++)
      out[i] = val;
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_at(vec, idx)
 * -> Returns the value at idx.
 */
exprtk_value_t fn_vec_at(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = args[0].data.vector.size;
    int idx = (int)args[1].data.number;
    if (idx >= 0 && (size_t)idx < n) {
      return exprtk_val_num(args[0].data.vector.data[idx]);
    }
  }
  return exprtk_val_num(NAN);
}

/**
 * vec_any(v[]) -> scalar 1.0 if any element is non-zero, else 0.0.
 */
exprtk_value_t fn_vec_any(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    for (size_t i = 0; i < args[0].data.vector.size; i++) {
      if (fabs(args[0].data.vector.data[i]) > 1e-9)
        return exprtk_val_num(1.0);
    }
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_all(v[]) -> scalar 1.0 if all elements are non-zero, else 0.0.
 */
exprtk_value_t fn_vec_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_num(0.0);
    for (size_t i = 0; i < n; i++) {
      if (fabs(args[0].data.vector.data[i]) <= 1e-9)
        return exprtk_val_num(0.0);
    }
    return exprtk_val_num(1.0);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_cross(v1, v2)
 * -> Vector of 1.0 (cross-over), -1.0 (cross-under), or 0.0.
 */
exprtk_value_t fn_vec_cross(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n1 = args[0].data.vector.size;
    size_t n2 = args[1].data.vector.size;
    size_t n = (n1 < n2) ? n1 : n2;

    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0.0);
    memset(out, 0, n * sizeof(double));

    if (n < 2)
      return exprtk_val_vec(out, n);

    const double *v1 = args[0].data.vector.data;
    const double *v2 = args[1].data.vector.data;

    for (size_t i = 1; i < n; i++) {
      if (v1[i - 1] <= v2[i - 1] && v1[i] > v2[i])
        out[i] = 1.0;
      else if (v1[i - 1] >= v2[i - 1] && v1[i] < v2[i])
        out[i] = -1.0;
    }
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0.0);
}

/**
 * vec_resample(vec, period, mode)
 * Aggregates a vector into a higher timeframe (smaller length).
 * Modes: 0=Last(Close), 1=First(Open), 2=Max(High), 3=Min(Low), 4=Sum(Volume), 5=Mean
 * Default mode is 0.
 */
exprtk_value_t fn_vec_resample(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc < 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) {
    return exprtk_val_num(NAN);
  }

  int period = (int)args[1].data.number;
  if (period <= 0)
    return exprtk_val_num(NAN);

  int mode = 0;
  if (argc >= 3 && args[2].type == EXPRTK_VAL_NUMBER) {
    mode = (int)args[2].data.number;
  }

  size_t in_len = args[0].data.vector.size;
  if (in_len == 0)
    return args[0]; /* empty */

  size_t out_len = in_len / period;
  if (out_len == 0)
    out_len = 1; /* at least 1 if partial period */

  double *out = MEM_ALLOC_ARRAY(arena, double, out_len);
  if (!out)
    return exprtk_val_num(NAN);

  const double *in = args[0].data.vector.data;
  ptrdiff_t end_idx = (ptrdiff_t)in_len - 1;
  ptrdiff_t out_idx = (ptrdiff_t)out_len - 1;

  while (end_idx >= 0 && out_idx >= 0) {
    ptrdiff_t start_idx = end_idx - period + 1;
    if (start_idx < 0)
      start_idx = 0;

    double v = 0.0;
    switch (mode) {
    case 1:
      v = in[start_idx];
      break; /* First/Open */
    case 2:  /* Max/High */
      v = in[start_idx];
      for (ptrdiff_t i = start_idx + 1; i <= end_idx; i++) {
        if (in[i] > v)
          v = in[i];
      }
      break;
    case 3: /* Min/Low */
      v = in[start_idx];
      for (ptrdiff_t i = start_idx + 1; i <= end_idx; i++) {
        if (in[i] < v)
          v = in[i];
      }
      break;
    case 4: /* Sum/Volume */
      for (ptrdiff_t i = start_idx; i <= end_idx; i++)
        v += in[i];
      break;
    case 5: { /* Mean */
      for (ptrdiff_t i = start_idx; i <= end_idx; i++)
        v += in[i];
      v /= (double)(end_idx - start_idx + 1);
      break;
    }
    default:
      v = in[end_idx];
      break; /* Mode 0 (Last/Close) */
    }

    out[out_idx] = v;
    end_idx -= period;
    out_idx--;
  }

  return exprtk_val_vec(out, out_len);
}

/* =========================================================================
 * Performance Metrics wrappers
 * ========================================================================= */

exprtk_value_t fn_sharpe(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double rf = (argc >= 2) ? args[1].data.number : 0.0;
    double af = (argc >= 3) ? args[2].data.number : 252.0;
    return exprtk_val_num(
        exprtk_sharpe(args[0].data.vector.data, args[0].data.vector.size, rf, af));
  }
  return exprtk_val_num(0);
}

exprtk_value_t fn_sortino(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double rf = (argc >= 2) ? args[1].data.number : 0.0;
    double af = (argc >= 3) ? args[2].data.number : 252.0;
    return exprtk_val_num(
        exprtk_sortino(args[0].data.vector.data, args[0].data.vector.size, rf, af));
  }
  return exprtk_val_num(0);
}

exprtk_value_t fn_calmar(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double af = (argc >= 2) ? args[1].data.number : 252.0;
    return exprtk_val_num(exprtk_calmar(args[0].data.vector.data, args[0].data.vector.size, af));
  }
  return exprtk_val_num(0);
}

exprtk_value_t fn_profit_factor(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR)
    return exprtk_val_num(exprtk_profit_factor(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

exprtk_value_t fn_expectancy(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR)
    return exprtk_val_num(exprtk_expectancy(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

exprtk_value_t fn_payoff_ratio(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR)
    return exprtk_val_num(exprtk_payoff_ratio(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

exprtk_value_t fn_max_dd_duration(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR)
    return exprtk_val_num(
        (double)exprtk_max_dd_duration(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

exprtk_value_t fn_ulcer_index(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_VECTOR)
    return exprtk_val_num(exprtk_ulcer_index(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

exprtk_value_t fn_information_ratio(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR)
    return exprtk_val_num(exprtk_information_ratio(
        args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

exprtk_value_t fn_treynor(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc >= 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    double rf = (argc >= 3) ? args[2].data.number : 0.0;
    return exprtk_val_num(exprtk_treynor(args[0].data.vector.data, args[1].data.vector.data,
                                         args[0].data.vector.size, rf));
  }
  return exprtk_val_num(0);
}

/* =========================================================================
 * (g) Multi-Timeframe OHLCV Aggregator
 * ========================================================================= */

/**
 * resample_ohlcv(O, H, L, C, V, factor)
 * → Returns a map: {open: vec, high: vec, low: vec, close: vec, volume: vec, count: num}
 * Reuses fn_vec_resample logic with all 5 modes at once.
 */
exprtk_value_t fn_resample_ohlcv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)env;
  if (argc < 6)
    return exprtk_val_num(NAN);
  for (size_t i = 0; i < 5; i++) {
    if (args[i].type != EXPRTK_VAL_VECTOR)
      return exprtk_val_num(NAN);
  }
  if (args[5].type != EXPRTK_VAL_NUMBER)
    return exprtk_val_num(NAN);

  int period = (int)args[5].data.number;
  if (period <= 0)
    return exprtk_val_num(NAN);

  size_t in_len = args[0].data.vector.size;
  if (in_len == 0)
    return exprtk_val_num(NAN);

  size_t out_len = in_len / (size_t)period;
  if (out_len == 0)
    out_len = 1;

  const double *O = args[0].data.vector.data;
  const double *H = args[1].data.vector.data;
  const double *L = args[2].data.vector.data;
  const double *C = args[3].data.vector.data;
  const double *V = args[4].data.vector.data;

  double *ro = MEM_ALLOC_ARRAY(arena, double, out_len);
  double *rh = MEM_ALLOC_ARRAY(arena, double, out_len);
  double *rl = MEM_ALLOC_ARRAY(arena, double, out_len);
  double *rc = MEM_ALLOC_ARRAY(arena, double, out_len);
  double *rv = MEM_ALLOC_ARRAY(arena, double, out_len);
  if (!ro || !rh || !rl || !rc || !rv)
    return exprtk_val_num(NAN);

  /* Aggregate from the end (align to most recent bar) */
  ptrdiff_t end_idx = (ptrdiff_t)in_len - 1;
  ptrdiff_t out_idx = (ptrdiff_t)out_len - 1;

  while (end_idx >= 0 && out_idx >= 0) {
    ptrdiff_t start_idx = end_idx - period + 1;
    if (start_idx < 0)
      start_idx = 0;

    /* Open = first bar's open */
    ro[out_idx] = O[start_idx];
    /* Close = last bar's close */
    rc[out_idx] = C[end_idx];
    /* High = max of highs */
    double mx = H[start_idx];
    for (ptrdiff_t i = start_idx + 1; i <= end_idx; i++)
      if (H[i] > mx)
        mx = H[i];
    rh[out_idx] = mx;
    /* Low = min of lows */
    double mn = L[start_idx];
    for (ptrdiff_t i = start_idx + 1; i <= end_idx; i++)
      if (L[i] < mn)
        mn = L[i];
    rl[out_idx] = mn;
    /* Volume = sum */
    double vs = 0.0;
    for (ptrdiff_t i = start_idx; i <= end_idx; i++)
      vs += V[i];
    rv[out_idx] = vs;

    end_idx -= period;
    out_idx--;
  }

  exprtk_value_t m = exprtk_val_map();
  exprtk_map_set(&m, "open", exprtk_val_vec(ro, out_len));
  exprtk_map_set(&m, "high", exprtk_val_vec(rh, out_len));
  exprtk_map_set(&m, "low", exprtk_val_vec(rl, out_len));
  exprtk_map_set(&m, "close", exprtk_val_vec(rc, out_len));
  exprtk_map_set(&m, "volume", exprtk_val_vec(rv, out_len));
  exprtk_map_set(&m, "count", exprtk_val_num((double)out_len));
  return m;
}

/* =========================================================================
 * (h) Walk-Forward Optimization Script Wrapper
 * ========================================================================= */

/**
 * walk_forward(param_names_list, ranges_vec, is_bars, oos_bars, step, metric)
 * Current status: script-side walk-forward is a placeholder.
 * It validates the argument shapes, then returns a map:
 *   {error: -1, num_windows: 0}
 * The real implementation lives in the C API strategy_walk_forward().
 *
 * param_names_list: a list of strings (parameter names)
 * ranges_vec:       flat vector [min1,max1,step1, min2,max2,step2, ...]
 * is_bars:          number of in-sample bars
 * oos_bars:         number of out-of-sample bars
 * step:             window slide step in bars
 * metric:           0=sharpe, 1=sortino, 2=calmar, 3=profit_factor
 */
exprtk_value_t fn_walk_forward(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc < 6)
    return exprtk_val_num(NAN);

  /* arg0: param names (list of strings) */
  if (args[0].type != EXPRTK_VAL_LIST)
    return exprtk_val_num(NAN);
  size_t np = args[0].data.list.count;
  if (np == 0)
    return exprtk_val_num(NAN);

  const char **names = (const char **)MEM_ALLOC_ARRAY(arena, const char *, np);
  if (!names)
    return exprtk_val_num(NAN);
  for (size_t i = 0; i < np; i++) {
    exprtk_value_t item = args[0].data.list.items[i];
    if (item.type != EXPRTK_VAL_STRING)
      return exprtk_val_num(NAN);
    names[i] = item.data.string.data;
  }

  /* arg1: ranges vector */
  if (args[1].type != EXPRTK_VAL_VECTOR)
    return exprtk_val_num(NAN);
  if (args[1].data.vector.size != np * 3)
    return exprtk_val_num(NAN);

  /* arg2-5: scalars */
  for (size_t i = 2; i < 6; i++) {
    if (args[i].type != EXPRTK_VAL_NUMBER)
      return exprtk_val_num(NAN);
  }

  wfo_config_t cfg = {0};
  cfg.param_names = names;
  cfg.ranges = args[1].data.vector.data;
  cfg.num_params = np;
  cfg.in_sample_bars = (size_t)args[2].data.number;
  cfg.out_sample_bars = (size_t)args[3].data.number;
  cfg.step_bars = (size_t)args[4].data.number;
  cfg.metric = (wfo_metric_t)(int)args[5].data.number;

  /* Script-side walk-forward remains a placeholder because the real engine
   * needs a strategy context, compiled code, and an asset id from the C API. */
  (void)cfg;

  /* Return an explicit placeholder map instead of pretending success. */
  exprtk_value_t m = exprtk_val_map();
  exprtk_map_set(&m, "error", exprtk_val_num(-1.0));
  exprtk_map_set(&m, "num_windows", exprtk_val_num(0.0));
  return m;
}
