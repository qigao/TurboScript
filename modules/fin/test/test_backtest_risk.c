/**
 * @file test_backtest_risk.c
 * @brief Unit tests for backtest and risk management functions.
 */

#include "fin.h"
#include "market_rules.h"
#include "order_manager.h"
#include "provider.h"
#include "strategy.h"
#include "strategy_optimizer.h"
#include "../src/exprtk_mod_strategy.h"
#include "tinytest.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>


#define EPSILON 0.001

typedef struct {
  const provider_bar_t *bars;
  size_t count;
  size_t index;
} test_stream_t;

typedef struct {
  const provider_bar_t *asset1;
  size_t asset1_count;
  const provider_bar_t *asset2;
  size_t asset2_count;
} test_provider_state_t;

static void *test_open_stream(provider_t *self, uint32_t asset_id, double start_date,
                              double end_date, mem_pool_t *stream_arena) {
  test_provider_state_t *state = (test_provider_state_t *)self->user_data;
  const provider_bar_t *bars = NULL;
  size_t count = 0;
  test_stream_t *stream;
  (void)start_date;
  (void)end_date;

  if (asset_id == 1) {
    bars = state->asset1;
    count = state->asset1_count;
  } else if (asset_id == 2) {
    bars = state->asset2;
    count = state->asset2_count;
  } else {
    return NULL;
  }

  stream = (test_stream_t *)mem_alloc(stream_arena, sizeof(*stream));
  if (!stream) return NULL;
  stream->bars = bars;
  stream->count = count;
  stream->index = 0;
  return stream;
}

static int test_next_bar(provider_t *self, void *stream_ptr, provider_bar_t *out) {
  test_stream_t *stream = (test_stream_t *)stream_ptr;
  (void)self;
  if (!stream || stream->index >= stream->count) return 0;
  *out = stream->bars[stream->index++];
  return 1;
}

static void test_close_stream(provider_t *self, void *stream_ptr) {
  (void)self;
  (void)stream_ptr;
}

suite("Backtest and Risk") {
  group("Simple Backtest") {
    it("should backtest a simple long strategy") {
      double open[] = {100, 101, 102, 103, 104};
      double close[] = {101, 102, 103, 104, 105};
      double signal[] = {1, 1, 1, 1, 0}; // Long then flat
      size_t n = 5;
      double equity[5];
      double trades[5];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check(num_trades > 0);
      check(equity[0] > 0.0);
      check(equity[n - 1] > 0.0);
      // Should make profit in uptrend
      check(equity[n - 1] > equity[0]);
    }

    it("should handle flat signal (no trading)") {
      double open[] = {100, 101, 102};
      double close[] = {101, 102, 103};
      double signal[] = {0, 0, 0};
      size_t n = 3;
      double equity[3];
      double trades[3];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check_int_eq(num_trades, 0);
      // Equity should remain constant
      check_float_eq(equity[0], 10000.0, EPSILON);
      check_float_eq(equity[n - 1], 10000.0, EPSILON);
    }

    it("should handle signal changes") {
      double open[] = {100, 101, 102, 103, 104};
      double close[] = {101, 102, 103, 104, 105};
      double signal[] = {1, 1, -1, -1, 0}; // Long, then short, then flat
      size_t n = 5;
      double equity[5];
      double trades[5];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check(num_trades >= 2); // At least 2 trades (long->short, short->flat)
    }

    it("should keep short equity economically consistent") {
      double open[] = {100, 100, 95};
      double close[] = {100, 95, 90};
      double signal[] = {-1, -1, 0};
      size_t n = 3;
      double equity[3];
      double trades[3];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, n, 10000.0, 0.001, equity, trades);

      check_int_eq((int)num_trades, 1);
      check(equity[0] > 0.0);
      check(equity[1] > equity[0]);
      check(equity[n - 1] > 10000.0);
    }

    it("should execute same-bar signals at the current open") {
      double open[] = {100, 110};
      double close[] = {110, 110};
      double signal[] = {1, 0};
      double equity[2];
      double trades[2];

      size_t num_trades =
          exprtk_bt_backtest(open, close, signal, 2, 10000.0, 0.0, equity, trades);

      check_int_eq((int)num_trades, 1);
      check_float_eq(equity[0], 10950.0, EPSILON);
      check_float_eq(equity[1], 10950.0, EPSILON);
      check_float_eq(trades[1], 950.0, EPSILON);
    }
  }

  group("Backtest Statistics") {
    it("should calculate backtest stats") {
      double equity[] = {10000, 10500, 11000, 10800, 11500, 12000};
      double trades[] = {0, 500, 500, -200, 700, 500};
      size_t n = 6;
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, n, 5, 252.0, stats);

      check(count >= 4);
      // Total return should be positive
      check(stats[0] > 0.0);
      // Annualized return
      check(stats[1] != 0.0);
      // Sharpe ratio
      check(stats[2] == stats[2]); // Not NaN
      // Max drawdown should be negative
      check(stats[3] <= 0.0);
    }

    it("should calculate trade statistics") {
      double equity[] = {10000, 10500, 10300, 10800, 10600};
      double trades[] = {0, 500, -200, 500, -200};
      size_t n = 5;
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, n, 4, 252.0, stats);

      check_int_eq(count, 10);
      // Win rate should be 0.5 (2 wins, 2 losses)
      check_float_eq(stats[4], 0.5, 0.1);
      // Profit factor should be > 1 (more wins than losses)
      check(stats[5] > 1.0);
    }

    it("should handle no trades") {
      double equity[] = {10000, 10000, 10000};
      double trades[] = {0, 0, 0};
      size_t n = 3;
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, n, 0, 252.0, stats);

      check(count >= 4);
      // Total return should be 0
      check_float_eq(stats[0], 0.0, EPSILON);
    }

    it("should annualize using return intervals instead of raw point count") {
      double equity[] = {100.0, 200.0};
      double trades[] = {0.0, 100.0};
      double stats[10];

      size_t count = exprtk_bt_stats(equity, trades, 2, 1, 1.0, stats);

      check(count >= 4);
      check_float_eq(stats[1], 1.0, EPSILON);
    }
  }

  group("Trading Costs") {
    it("should calculate slippage") {
      double price = 100.0;
      double size = 1000.0;
      double vol = 10000.0;
      double avg_vol = 10000.0;
      double lambda = 0.1;

      double slippage = exprtk_bt_slippage(price, size, vol, avg_vol, lambda);

      check(slippage >= 0.0);
      // Larger orders should have more slippage
      double large_slip = exprtk_bt_slippage(price, size * 2, vol, avg_vol, lambda);
      check(large_slip > slippage);
    }

    it("should use the square-root impact model per share") {
      double slippage = exprtk_bt_slippage(100.0, 1000.0, 10000.0, 10000.0, 0.1);

      check_float_eq(slippage, 3.16227766, EPSILON);
    }

    it("should calculate total cost") {
      double price = 100.0;
      double size = 100.0;
      double commission = 0.001; // 0.1%
      double tax = 0.001;        // 0.1%
      double slippage = 0.05;    // $0.05 per share

      double cost = exprtk_bt_cost(price, size, commission, tax, slippage);

      check(cost > 0.0);
      // Cost should include commission + tax + slippage
      double expected = 100 * 100 * 0.001 + 100 * 100 * 0.001 + 100 * 0.05;
      check_float_eq(cost, expected, 1.0);
    }

    it("should apply bt_cost tax symmetrically regardless of trade side") {
      double buy_cost = exprtk_bt_cost(100.0, 100.0, 0.001, 0.001, 0.05);
      double sell_cost = exprtk_bt_cost(100.0, -100.0, 0.001, 0.001, 0.05);

      check_float_eq(buy_cost, sell_cost, EPSILON);
    }
  }

  group("Order Manager") {
    it("should mark to market by asset id instead of slot index") {
      mem_pool_t arena;
      mem_init(&arena, 8192);

      order_manager_t *mgr = order_manager_create(&MARKET_US_EQUITY, 10000.0, 4, &arena);
      uint32_t asset_ids[] = {7, 42};
      double prices[] = {50.0, 120.0};

      check_not_null(mgr);
      mgr->slippage_pct = 0.0;
      mgr->spread = 0.0;
      check_int_eq(order_manager_market(mgr, 42, +1, 0.1, 100.0, 100.0, 0.0, 0.0), ORDER_OK);

      order_manager_mark_to_market(mgr, asset_ids, prices, 2);
      check(mgr->equity > 10000.0);

      order_manager_free(mgr);
      mem_destroy(&arena);
    }
  }

  group("Universe Strategy") {
    it("should process universe bars on a shared date axis") {
      static const provider_bar_t asset1_bars[] = {
        { .date = 1.0, .open = 10.0, .high = 11.0, .low = 9.0, .close = 10.0, .volume = 100.0, .asset_id = 1 },
        { .date = 2.0, .open = 11.0, .high = 12.0, .low = 10.0, .close = 11.0, .volume = 100.0, .asset_id = 1 },
        { .date = 3.0, .open = 12.0, .high = 13.0, .low = 11.0, .close = 12.0, .volume = 100.0, .asset_id = 1 },
      };
      static const provider_bar_t asset2_bars[] = {
        { .date = 1.0, .open = 20.0, .high = 21.0, .low = 19.0, .close = 20.0, .volume = 100.0, .asset_id = 2 },
        { .date = 2.0, .open = 21.0, .high = 22.0, .low = 20.0, .close = 21.0, .volume = 100.0, .asset_id = 2 },
        { .date = 3.0, .open = 22.0, .high = 23.0, .low = 21.0, .close = 22.0, .volume = 100.0, .asset_id = 2 },
      };
      mem_pool_t arena;
      mem_init(&arena, 65536);

      universe_t *u = universe_create(&arena);
      provider_t provider = {0};
      test_provider_state_t state = {
        .asset1 = asset1_bars, .asset1_count = 3,
        .asset2 = asset2_bars, .asset2_count = 3,
      };
      strategy_config_t cfg = strategy_config_default();
      strategy_ctx_t *ctx;
      universe_asset_t a1 = { .id = 1, .ticker = "A1", .start_date = 1.0, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 };
      universe_asset_t a2 = { .id = 2, .ticker = "A2", .start_date = 1.0, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 };
      size_t equity_len = 0;

      check_not_null(u);
      check_int_eq(universe_add_asset(u, &a1), 0);
      check_int_eq(universe_add_asset(u, &a2), 0);
      universe_finalize(u);

      cfg.warmup_bars = 0;
      cfg.window_capacity = 8;
      provider.market = &MARKET_US_EQUITY;
      provider.open_stream = test_open_stream;
      provider.next_bar = test_next_bar;
      provider.close_stream = test_close_stream;
      provider.user_data = &state;

      ctx = strategy_create(u, &provider, &cfg, &arena);
      check_not_null(ctx);
      check_int_eq(strategy_compile(ctx, "signal = 0;"), 0);
      check_int_eq(strategy_run_universe(ctx, 1.0, 3.0), 6);
      check_not_null(strategy_equity_curve(ctx, &equity_len));
      check_int_eq((int)equity_len, 3);

      strategy_free(ctx);
      universe_free(u);
      mem_destroy(&arena);
    }

    it("should rescale existing window bars when dividend adjustment changes") {
      static const provider_bar_t bars[] = {
        { .date = 1.0, .open = 100.0, .high = 100.0, .low = 100.0, .close = 100.0, .volume = 100.0, .asset_id = 1 },
        { .date = 2.0, .open = 90.0,  .high = 90.0,  .low = 90.0,  .close = 90.0,  .volume = 100.0, .asset_id = 1 },
      };
      mem_pool_t arena;
      mem_init(&arena, 65536);

      universe_t *u = universe_create(&arena);
      provider_t provider = {0};
      test_provider_state_t state = {
        .asset1 = bars, .asset1_count = 2,
        .asset2 = NULL, .asset2_count = 0,
      };
      strategy_config_t cfg = strategy_config_default();
      strategy_ctx_t *ctx;
      universe_asset_t asset = { .id = 1, .ticker = "DIV", .start_date = 1.0, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 };
      universe_adj_t div = { .asset_id = 1, .date = 2.0, .type = UNIVERSE_ADJ_DIVIDEND, .factor = 10.0 };
      bar_window_t *w;

      check_not_null(u);
      check_int_eq(universe_add_asset(u, &asset), 0);
      check_int_eq(universe_add_adjustment(u, &div), 0);
      universe_finalize(u);

      cfg.warmup_bars = 0;
      cfg.window_capacity = 8;
      provider.market = &MARKET_US_EQUITY;
      provider.open_stream = test_open_stream;
      provider.next_bar = test_next_bar;
      provider.close_stream = test_close_stream;
      provider.user_data = &state;

      ctx = strategy_create(u, &provider, &cfg, &arena);
      check_not_null(ctx);
      check_int_eq(strategy_compile(ctx, "signal = 0;"), 0);
      check_int_eq(strategy_run_single(ctx, 1, 1.0, 2.0), 2);

      w = ctx->windows[0];
      check_not_null(w);
      bar_window_linearize(w);
      check_int_eq((int)w->count, 2);
      check_float_eq(w->close[0], 90.0, EPSILON);
      check_float_eq(w->close[1], 81.0, EPSILON);

      strategy_free(ctx);
      universe_free(u);
      mem_destroy(&arena);
    }

    it("should reset universe runtime state when strategy is reset") {
      static const provider_bar_t bars[] = {
        { .date = 1.0, .open = 100.0, .high = 100.0, .low = 100.0, .close = 100.0, .volume = 100.0, .asset_id = 1 },
        { .date = 2.0, .open = 90.0,  .high = 90.0,  .low = 90.0,  .close = 90.0,  .volume = 100.0, .asset_id = 1 },
      };
      mem_pool_t arena;
      mem_init(&arena, 65536);

      universe_t *u = universe_create(&arena);
      provider_t provider = {0};
      test_provider_state_t state = {
        .asset1 = bars, .asset1_count = 2,
        .asset2 = NULL, .asset2_count = 0,
      };
      strategy_config_t cfg = strategy_config_default();
      strategy_ctx_t *ctx;
      universe_asset_t asset = { .id = 1, .ticker = "RST", .start_date = 1.0, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 };
      universe_adj_t div = { .asset_id = 1, .date = 2.0, .type = UNIVERSE_ADJ_DIVIDEND, .factor = 10.0 };

      check_not_null(u);
      check_int_eq(universe_add_asset(u, &asset), 0);
      check_int_eq(universe_add_adjustment(u, &div), 0);
      universe_finalize(u);

      cfg.warmup_bars = 0;
      cfg.window_capacity = 8;
      provider.market = &MARKET_US_EQUITY;
      provider.open_stream = test_open_stream;
      provider.next_bar = test_next_bar;
      provider.close_stream = test_close_stream;
      provider.user_data = &state;

      ctx = strategy_create(u, &provider, &cfg, &arena);
      check_not_null(ctx);
      check_int_eq(strategy_compile(ctx, "signal = 0;"), 0);
      check_int_eq(strategy_run_single(ctx, 1, 1.0, 2.0), 2);

      check(u->current_date > 0.0);
      check_float_eq(universe_adj_factor(u, 1), 0.9, EPSILON);

      strategy_reset(ctx);

      check_float_eq(u->current_date, 0.0, EPSILON);
      check_float_eq(universe_adj_factor(u, 1), 1.0, EPSILON);
      check_int_eq((int)u->num_delisted_today, 0);
      check_int_eq((int)ctx->total_bars, 0);

      strategy_free(ctx);
      universe_free(u);
      mem_destroy(&arena);
    }
  }

  group("Walk-Forward") {
    it("should allow NULL code when the strategy is already compiled") {
      static const provider_bar_t bars[] = {
        { .date = 1.0, .open = 10.0, .high = 10.0, .low = 10.0, .close = 10.0, .volume = 100.0, .asset_id = 1 },
        { .date = 2.0, .open = 11.0, .high = 11.0, .low = 11.0, .close = 11.0, .volume = 100.0, .asset_id = 1 },
        { .date = 5.0, .open = 12.0, .high = 12.0, .low = 12.0, .close = 12.0, .volume = 100.0, .asset_id = 1 },
        { .date = 6.0, .open = 13.0, .high = 13.0, .low = 13.0, .close = 13.0, .volume = 100.0, .asset_id = 1 },
      };
      mem_pool_t arena;
      mem_init(&arena, 65536);

      universe_t *u = universe_create(&arena);
      provider_t provider = {0};
      test_provider_state_t state = {
        .asset1 = bars, .asset1_count = 4,
        .asset2 = NULL, .asset2_count = 0,
      };
      strategy_config_t cfg = strategy_config_default();
      strategy_ctx_t *ctx;
      universe_asset_t asset = { .id = 1, .ticker = "WF", .start_date = 1.0, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 };
      const char *param_names[] = { "period" };
      double ranges[] = { 5.0, 5.0, 1.0 };
      wfo_config_t wfo = {
        .param_names = param_names,
        .ranges = ranges,
        .num_params = 1,
        .in_sample_bars = 2,
        .out_sample_bars = 1,
        .step_bars = 1,
        .metric = WFO_METRIC_SHARPE
      };
      wfo_result_t result = {0};

      check_not_null(u);
      check_int_eq(universe_add_asset(u, &asset), 0);
      universe_finalize(u);

      cfg.warmup_bars = 0;
      cfg.window_capacity = 8;
      provider.market = &MARKET_US_EQUITY;
      provider.open_stream = test_open_stream;
      provider.next_bar = test_next_bar;
      provider.close_stream = test_close_stream;
      provider.user_data = &state;

      ctx = strategy_create(u, &provider, &cfg, &arena);
      check_not_null(ctx);
      check_int_eq(strategy_compile(ctx, "signal = 0;"), 0);
      check_int_eq(strategy_walk_forward(ctx, NULL, 1, 1.0, 6.0, &wfo, &result, &arena), 0);
      check_int_eq((int)result.num_windows, 2);

      strategy_free(ctx);
      universe_free(u);
      mem_destroy(&arena);
    }

    it("should return an explicit placeholder map for script walk_forward") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t names[1];
      double ranges[] = { 5.0, 5.0, 1.0 };
      exprtk_value_t out;

      exprtk_env_init(&env);

      names[0] = exprtk_val_str((tstr_v){ .data = "period", .len = 6 });
      args[0] = exprtk_val_list_ex(names, 1, 0);
      args[1] = exprtk_val_vec(ranges, 3);
      args[2] = exprtk_val_num(10.0);
      args[3] = exprtk_val_num(5.0);
      args[4] = exprtk_val_num(1.0);
      args[5] = exprtk_val_num(0.0);

      out = fn_walk_forward(6, args, &env, &arena);
      check_int_eq(out.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&out, "error").data.number, -1.0, EPSILON);
      check_float_eq(exprtk_map_get(&out, "num_windows").data.number, 0.0, EPSILON);

      exprtk_map_free(&out);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("Position Sizing") {
    it("should calculate fixed fractional size") {
      double equity = 10000.0;
      double risk_pct = 0.02;  // Risk 2%
      double stop_dist = 0.05; // 5% stop

      double size = exprtk_fixed_frac(equity, risk_pct, stop_dist);

      check(size > 0.0);
      check(size <= equity);
      // Size should be 200 / 0.05 = 4000
      check_float_eq(size, 4000.0, 100.0);
    }

    it("should cap size at equity") {
      double equity = 10000.0;
      double risk_pct = 0.5;   // Risk 50%
      double stop_dist = 0.01; // 1% stop (very tight)

      double size = exprtk_fixed_frac(equity, risk_pct, stop_dist);

      // Should cap at equity
      check_float_eq(size, equity, EPSILON);
    }

    it("should handle zero stop distance") {
      double equity = 10000.0;
      double risk_pct = 0.02;
      double stop_dist = 0.0;

      double size = exprtk_fixed_frac(equity, risk_pct, stop_dist);

      check_float_eq(size, 0.0, EPSILON);
    }
  }

  group("Optimal F") {
    it("should calculate optimal f for winning trades") {
      double trades[] = {100, -50, 150, -30, 80, -40, 120, -20};
      size_t n = 8;
      double out[2];

      double opt_f = exprtk_optimal_f(trades, n, out);

      check(opt_f > 0.0);
      check(opt_f <= 1.0);
      check(out[0] == opt_f);
      check(out[1] > 1.0); // TWR should be > 1 for profitable system
    }

    it("should handle all winning trades") {
      double trades[] = {100, 150, 80, 120};
      size_t n = 4;
      double out[2];

      double opt_f = exprtk_optimal_f(trades, n, out);

      // No losses - can't calculate optimal f
      check_float_eq(opt_f, 0.0, EPSILON);
    }

    it("should handle losing system") {
      double trades[] = {-100, 50, -150, 30, -80};
      size_t n = 5;
      double out[2];

      double opt_f = exprtk_optimal_f(trades, n, out);

      // Should return a value (might be small)
      check(opt_f >= 0.0);
      check(opt_f <= 1.0);
    }
  }

  group("Monte Carlo Simulation") {
    it("should simulate price paths") {
      double s0 = 100.0;
      double mu = 0.10;        // 10% drift
      double sigma = 0.20;     // 20% volatility
      double dt = 1.0 / 252.0; // Daily
      size_t steps = 10;
      size_t paths = 5;
      double out[50]; // 5 paths × 10 steps
      mem_pool_t arena;
      mem_init(&arena, 4096);

      srand(42); // Fixed seed for reproducibility
      size_t result = exprtk_mc_simulate(s0, mu, sigma, dt, steps, paths, out, &arena);

      check_int_eq(result, paths);
      // All paths should start at s0
      for (size_t p = 0; p < paths; p++) {
        check_float_eq(out[p * steps], s0, EPSILON);
      }
      // Prices should be positive
      for (size_t i = 0; i < paths * steps; i++) {
        check(out[i] > 0.0);
      }

      mem_destroy(&arena);
    }

    it("should generate different paths") {
      double s0 = 100.0;
      double mu = 0.10;
      double sigma = 0.20;
      double dt = 1.0 / 252.0;
      size_t steps = 10;
      size_t paths = 3;
      double out[30];
      mem_pool_t arena;
      mem_init(&arena, 4096);

      srand(123);
      exprtk_mc_simulate(s0, mu, sigma, dt, steps, paths, out, &arena);

      // Final prices should be different across paths
      double final1 = out[1 * steps - 1];
      double final2 = out[2 * steps - 1];
      double final3 = out[3 * steps - 1];

      check(fabs(final1 - final2) > 0.1 || fabs(final2 - final3) > 0.1);

      mem_destroy(&arena);
    }
  }
}
