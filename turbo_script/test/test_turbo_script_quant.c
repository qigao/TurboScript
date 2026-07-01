#include "../src/turbo_script_internal.h"
#include "exprtk_types.h"
#include "tinytest.h"
#include "turbo_fs.h"
#include "turbo_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  turbo_script_ctx_t *ctx;
  const char *script;
  int result;
} script_coro_arg_t;

static unsigned long ts_test_nonce(void) {
  static unsigned long counter = 0;
  return ((unsigned long)time(NULL) << 16) ^ (unsigned long)clock() ^ ++counter;
}

static void ts_test_make_name(char *buf, size_t buf_size, const char *prefix, const char *suffix) {
  const char *tail = suffix ? suffix : "";
  if (!buf || buf_size == 0 || !prefix) return;
  snprintf(buf, buf_size, "%s_%lu%s", prefix, ts_test_nonce(), tail);
}

static exprtk_value_t test_triple_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0};
  }
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = args[0].data.number * 3.0};
}


spec("turbo_script_quant") {
  describe("Stock Research: TA-Lib Scripts") {
    it("SMA crossover signal generation") {
      // Scenario: A researcher loads daily closing prices and generates
      // buy/sell signals based on SMA(5) crossing above/below SMA(10).
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Simulated daily close prices (20 days of uptrend then pullback)
          "close = [100, 101, 102, 104, 106, "
          "108, 110, 112, 115, 118, "
          "          120, 119, 117, 115, 113, 112, 114, 116, 118, 120]; "

          // Compute fast and slow SMAs
          "sma_fast = ta.sma(close, 5); "
          "sma_slow = ta.sma(close, 10); "

          // Check the last bar: is fast SMA above slow SMA?
          // If yes → bullish signal (1), else bearish (0)
          "signal = if (sma_fast[19] > sma_slow[19]) { 1 } else { 0 }; "

          // Compute momentum via RSI
          "rsi = ta.rsi(close, 14); "
          "rsi_last = rsi[19]; "

          // Compute volatility
          "std = ta.stddev(close, 10, 1.0); "
          "vol_last = std[19];";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("SMA Crossover Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      // After pullback and recovery, fast SMA should be above slow SMA
      double signal = ts_get_num(ctx, "signal");
      printf("  Signal: %g (1=bullish, 0=bearish)\n", signal);

      double rsi_last = ts_get_num(ctx, "rsi_last");
      printf("  RSI(14): %.2f\n", rsi_last);
      check_float_gt(rsi_last, 0.0); // RSI should be valid

      double vol_last = ts_get_num(ctx, "vol_last");
      printf("  Volatility (StdDev): %.4f\n", vol_last);
      check_float_gt(vol_last, 0.0); // StdDev should be positive

      turbo_script_free(ctx);
    }

    it("RSI oversold screener") {
      // Scenario: A researcher screens for oversold stocks (RSI < 30)
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Stock A: strong downtrend (should be oversold)
          "stock_a = [50, 49, 47, 45, 43, 41, "
          "39, 37, 35, 33, "
          "            31, 30, 29, 28, 27]; "
          "rsi_a = ta.rsi(stock_a, 14);"
          "rsi_a_last = rsi_a[14]; "
          "oversold_a = if (rsi_a_last < 30) { 1 } else { 0 }; "

          // Stock B: strong uptrend (should be overbought)
          "stock_b = [50, 52, 54, 56, 58, 60, 62, 64, 66, 68, "
          "            70, 72, 74, 76, 78]; "
          "rsi_b = ta.rsi(stock_b, 14);"
          "rsi_b_last = rsi_b[14]; "
          "overbought_b = if (rsi_b_last > 70) { 1 } else { 0 };";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("RSI Screener Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double rsi_a = ts_get_num(ctx, "rsi_a_last");
      double rsi_b = ts_get_num(ctx, "rsi_b_last");
      printf("  Stock A RSI: %.2f (downtrend)\n", rsi_a);
      printf("  Stock B RSI: %.2f (uptrend)\n", rsi_b);

      // Downtrend stock should have lower RSI than uptrend stock
      check_float_lt(rsi_a, rsi_b);

      turbo_script_free(ctx);
    }

    it("Multi-indicator confluence scoring") {
      // Scenario: A researcher combines multiple indicators into a
      // composite score to rank stocks. Score range: 0 (bearish) to 100 (bullish).
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // 20 bars of price data with clear uptrend
          "close = [100, 102, 104, 103, 105, "
          "107, 109, 108, 110, 112, "
          "          114, 116, 115, 117, 119, 121, 123, 122, 124, 126]; "
          "high  = [102, 104, 106, 105, 107, 109, 111, 110, 112, 114, "
          "          116, 118, 117, 119, 121, 123, 125, 124, 126, 128]; "
          "low   = [ 99, 101, 103, 102, 104, 106, 108, 107, 109, 111, "
          "          113, 115, 114, 116, 118, 120, 122, 121, 123, 125]; "

          // --- Indicator 1: Trend (SMA crossover) ---
          "sma5  = ta.sma(close, 5); "
          "sma10 = ta.sma(close, 10); "
          "trend_score = if (sma5[19] > sma10[19]) { 25 } else { 0 }; "

          // --- Indicator 2: Momentum (RSI) ---
          "rsi = ta.rsi(close, 14); "
          "rsi_val = rsi[19]; "
          "mom_score = if (rsi_val > 50 and rsi_val < 70) { 25 } "
          "             else { if (rsi_val >= 70) { 10 } else { 0 } }; "

          // --- Indicator 3: Volatility (ATR normalized) ---
          "atr = ta.atr(high, low, close, 14); "
          "atr_pct = atr[19] / close[19] * 100; "
          "vol_score = if (atr_pct < 3) { 25 } else { 10 }; "

          // --- Indicator 4: Price position (above EMA) ---
          "ema20 = ta.ema(close, 20); "
          "pos_score = if (close[19] > ema20[19]) { 25 } else { 0 }; "

          // --- Composite Score ---
          "total_score = trend_score + mom_score + vol_score + pos_score;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Confluence Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double total = ts_get_num(ctx, "total_score");
      double trend = ts_get_num(ctx, "trend_score");
      double mom = ts_get_num(ctx, "mom_score");
      double vol = ts_get_num(ctx, "vol_score");
      double pos = ts_get_num(ctx, "pos_score");

      printf("  Confluence Score: %.0f / 100\n", total);
      printf("    Trend:    %.0f/25\n", trend);
      printf("    Momentum: %.0f/25\n", mom);
      printf("    Vol:      %.0f/25\n", vol);
      printf("    Position: %.0f/25\n", pos);

      // In a clear uptrend, the total score should be high
      check_float_gt(total, 50.0);

      turbo_script_free(ctx);
    }

    it("Options pricing: BSM call vs put parity") {
      // Scenario: A researcher verifies put-call parity:
      // C - P = S * e^(-qT) - K * e^(-rT)  (for q=0: C - P ≈ S - K*e^(-rT))
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // S=100, K=100, T=1yr, r=5%, sigma=20%
          "call = ta.bsm_call([100], [100], "
          "[1], [0.05], [0.2]); "
          "put  = ta.bsm_put([100], [100], [1], [0.05], [0.2]); "
          "call_price = call[0]; "
          "put_price  = put[0]; "

          // Put-call parity: C - P ≈ S - K*e^(-rT) = 100 - 100*e^(-0.05) ≈ 4.877
          "parity = call_price - put_price; "

          // Greeks
          "delta_c = bsm_delta_call([100], [100], [1], [0.05], [0.2]); "
          "delta_p = bsm_delta_put([100], [100], [1], [0.05], [0.2]); "
          "dc = delta_c[0]; "
          "dp = delta_p[0]; "

          // Delta parity: delta_call - delta_put ≈ e^(-qT) ≈ 1 (for q=0)
          "delta_parity = dc - dp;";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Options Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double call_p = ts_get_num(ctx, "call_price");
      double put_p = ts_get_num(ctx, "put_price");
      double parity = ts_get_num(ctx, "parity");
      double dc = ts_get_num(ctx, "dc");
      double dp = ts_get_num(ctx, "dp");

      printf("  Call: $%.4f\n", call_p);
      printf("  Put:  $%.4f\n", put_p);
      printf("  C - P: $%.4f (expected ~4.877)\n", parity);
      printf("  Delta Call: %.4f\n", dc);
      printf("  Delta Put:  %.4f\n", dp);

      // Put-call parity check: C - P ≈ 4.877
      check_float_eq(parity, 4.877, 0.1);

      // Call should be more expensive than put for ATM with positive rate
      check_float_gt(call_p, put_p);

      // Delta parity: delta_c - delta_p ≈ 1
      double delta_parity = ts_get_num(ctx, "delta_parity");
      check_float_eq(delta_parity, 1.0, 0.05);

      turbo_script_free(ctx);
    }

    it("CSV data pipeline: split -> analyze -> score") {
      // Scenario: A researcher loads CSV price data from a string,
      // splits it into a vector, and runs TA analysis.
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");

      const char *script =
          // Simulated CSV close prices (comma-separated)
          "csv_data = "
          "\"100,102,104,103,105,107,109,108,110,112\"; "

          // Parse CSV into a numeric vector
          "prices = split(csv_data, \",\"); "

          // Compute indicators on the parsed data
          "sma3 = ta.sma(prices, 3); "
          "ema3 = ta.ema(prices, 3); "

          // Last values
          "last_price = prices[9]; "
          "last_sma = sma3[9]; "
          "last_ema = ema3[9]; "

          // Simple trend check
          "trend = if (last_price > last_sma and last_price > last_ema) { 1 } else { 0 };";

      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("CSV Pipeline Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double last_price = ts_get_num(ctx, "last_price");
      double last_sma = ts_get_num(ctx, "last_sma");
      double last_ema = ts_get_num(ctx, "last_ema");
      double trend = ts_get_num(ctx, "trend");

      printf("  Last Price: %.2f\n", last_price);
      printf("  SMA(3):    %.2f\n", last_sma);
      printf("  EMA(3):    %.2f\n", last_ema);
      printf("  Trend:     %s\n", trend > 0 ? "BULLISH" : "BEARISH");

      check_float_eq(last_price, 112.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Function Syntax (func)") {
    it("should allow defining and calling func in script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func double_val(x) { "
                           "  return x * 2; "
                           "}"
                           "res = double_val(21);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 42.0, 0.001);
      turbo_script_free(ctx);
    }

    it("should support recursion with func") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "func factorial(n) { "
                           "  if (n <= 1) { return 1; } "
                           "  else { return n * factorial(n - 1); } "
                           "};"
                           "res = factorial(5);";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 120.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Modules") {
    it("should support importing external scripts") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      // Write it using native file_write (or just setup beforehand? native is easier if we have
      // ctx) But we can use turbo_script_run to write it!
      turbo_script_run(ctx, "write_file(\"utils.tbs\", "
                            "\"var MODULE_VERSION = 2.0; func square(x) { return x * x; };\");");

      // Main script
      const char *script = "import(\"utils.tbs\"); "
                           "res = square(5); "
                           "ver = MODULE_VERSION;";

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 25.0, 0.1);
      check_float_eq(ts_get_num(ctx, "ver"), 2.0, 0.1);

      // Clean up
      turbo_script_run(ctx, "file_remove(\"utils.tbs\");");
      turbo_script_free(ctx);
    }

    it("should resolve nested relative script imports from run_file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *util_src = "func helper(x) { return x + 1; };";
      const char *child_src = "import(\"./utils.tbs\");";
      const char *main_src = "import(\"sub/child.tbs\"); res = helper(7);";
      char root_dir[TURBO_FS_MAX_PATH];
      char sub_dir[TURBO_FS_MAX_PATH];
      char main_path[TURBO_FS_MAX_PATH];
      char child_path[TURBO_FS_MAX_PATH];
      char util_path[TURBO_FS_MAX_PATH];
      turbo_fs_buf_t buf;

      ts_test_make_name(root_dir, sizeof(root_dir), "_ts_import_rel", "");
      check_int_eq(turbo_fs_mkdir(root_dir, 0755), 0);
      check_int_eq(turbo_fs_path_join(sub_dir, sizeof(sub_dir), root_dir, "sub"), 0);
      check_int_eq(turbo_fs_mkdir(sub_dir, 0755), 0);
      check_int_eq(turbo_fs_path_join(main_path, sizeof(main_path), root_dir, "main.tbs"), 0);
      check_int_eq(turbo_fs_path_join(child_path, sizeof(child_path), sub_dir, "child.tbs"), 0);
      check_int_eq(turbo_fs_path_join(util_path, sizeof(util_path), sub_dir, "utils.tbs"), 0);

      buf = turbo_fs_buf_init((char *)util_src, strlen(util_src));
      check_int_eq(turbo_fs_write_file(util_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)child_src, strlen(child_src));
      check_int_eq(turbo_fs_write_file(child_path, &buf), 0);
      buf = turbo_fs_buf_init((char *)main_src, strlen(main_src));
      check_int_eq(turbo_fs_write_file(main_path, &buf), 0);

      check_int_eq(turbo_script_run_file(ctx, main_path), 0);
      check_float_eq(ts_get_num(ctx, "res"), 8.0, 0.001);

      check_int_eq(turbo_fs_unlink(main_path), 0);
      check_int_eq(turbo_fs_unlink(child_path), 0);
      check_int_eq(turbo_fs_unlink(util_path), 0);
      check_int_eq(turbo_fs_rmdir(sub_dir), 0);
      check_int_eq(turbo_fs_rmdir(root_dir), 0);
      turbo_script_free(ctx);
    }

    it("should only treat .tbs files as script imports") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      turbo_script_run(ctx, "write_file(\"legacy.ts\", \"legacy_value = 7;\");");

      check_int_eq(turbo_script_run(ctx, "import(\"legacy.ts\");"), -1);
      check_float_eq(ts_get_num(ctx, "legacy_value"), 0.0, 0.001);

      turbo_script_run(ctx, "file_remove(\"legacy.ts\");");
      turbo_script_free(ctx);
    }

    it("should return explicit script exports as a module map") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *module_src = "load_count = load_count + 1; "
                               "func add1(x) { return x + 1; }; "
                               "export(\"add1\"); "
                               "export(\"answer\", 41); "
                               "export(\"load_count\");";
      char module_path[TURBO_FS_MAX_PATH];
      char script[512];
      turbo_fs_buf_t buf;

      ts_test_make_name(module_path, sizeof(module_path), "_ts_export_mod", ".tbs");
      buf = turbo_fs_buf_init((char *)module_src, strlen(module_src));
      check_int_eq(turbo_fs_write_file(module_path, &buf), 0);

      snprintf(script, sizeof(script),
               "var m1 = import(\"%s\"); "
               "var m2 = import(\"%s\"); "
               "var f = m1.add1; "
               "var res = f(4) + m2.answer; "
               "var loads = m2.load_count;",
               module_path, module_path);

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "res"), 46.0, 0.001);
      check_float_eq(ts_get_num(ctx, "loads"), 1.0, 0.001);

      check_int_eq(turbo_fs_unlink(module_path), 0);
      turbo_script_free(ctx);
    }

    it("should import a script module without leaking globals") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *module_src = "shared = 99; "
                               "secret = 5; "
                               "func add_secret(x) { return x + secret; }; "
                               "export(\"add_secret\"); "
                               "export(\"shared\");";
      char module_path[TURBO_FS_MAX_PATH];
      char script[640];
      turbo_fs_buf_t buf;

      ts_test_make_name(module_path, sizeof(module_path), "_ts_isolated_mod", ".tbs");
      buf = turbo_fs_buf_init((char *)module_src, strlen(module_src));
      check_int_eq(turbo_fs_write_file(module_path, &buf), 0);

      snprintf(script, sizeof(script),
               "shared = 7; "
               "var mod = import_module(\"%s\"); "
               "var f = mod.add_secret; "
               "var out = f(3); "
               "var exported_shared = mod.shared; "
               "var global_shared = shared; "
               "var leaked_secret = secret;",
               module_path);

      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "out"), 8.0, 0.001);
      check_float_eq(ts_get_num(ctx, "exported_shared"), 99.0, 0.001);
      check_float_eq(ts_get_num(ctx, "global_shared"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "leaked_secret"), 0.0, 0.001);

      check_int_eq(turbo_fs_unlink(module_path), 0);
      turbo_script_free(ctx);
    }
  }

  describe("TA Indicators and JSON Vectors") {
    it("should compute SMA and RSI on mocked Polymarket data") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "ta"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "parser"), 0);

      // Mocked Polymarket history JSON (array of objects with 'p' field)
      const char *mock_history = "["
                                 "{\"p\":0.51},{\"p\":0.52},{\"p\":0.53},{\"p\":0.54},{\"p\":0.55},"
                                 "{\"p\":0.56},{\"p\":0.57},{\"p\":0.58},{\"p\":0.59},{\"p\":0.60}"
                                 "]";
      ts_bind_str(ctx, "history_json", mock_history);

      const char *script = "prices = "
                           "json.to_vec(history_json, \"p\");"
                           "sma3 = ta.sma(prices, 3);"
                           "rsi5 = ta.rsi(prices, 5);"
                           "last_price = prices[9];"
                           "last_sma = sma3[9];"
                           "last_rsi = rsi5[9];"
                           "res = last_sma;";

      int run_res = turbo_script_run(ctx, script);
      if (run_res != 0) printf("TA Test Run Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(run_res, 0);

      double lp = ts_get_num(ctx, "last_price");
      printf("  Last Price: %g\n", lp);
      check_float_eq(lp, 0.60, 0.001);

      double ls = ts_get_num(ctx, "res");
      printf("  SMA(3): %g\n", ls);
      check_float_eq(ls, 0.59, 0.001);

      double rsi_val = ts_get_num(ctx, "last_rsi");
      printf("  RSI(5): %g\n", rsi_val);
      check_float_gt(rsi_val, 0.0);
      check_float_eq(rsi_val, 100.0, 0.1);

      turbo_script_free(ctx);
    }
  }

  describe("Compile/Exec Separation") {
    it("should compile once and exec twice with different x") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, "y = x * 2;");
      check_not_null(compiled);

      ts_bind_num(ctx, "x", 5.0);
      turbo_script_exec(ctx, compiled);

      check_float_eq(ts_get_num(ctx, "y"), 10.0, 0.001);

      ts_bind_num(ctx, "x", 100.0);
      turbo_script_exec(ctx, compiled);
      check_float_eq(ts_get_num(ctx, "y"), 200.0, 0.001);

      turbo_script_compiled_free(compiled);
      turbo_script_free(ctx);
    }

    it("should return NULL on compile error") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_compiled_t *compiled = turbo_script_compile(ctx, "x = 10 + * 5");
      check_null(compiled);
      turbo_script_free(ctx);
    }
  }

  describe("Vector Data Channel") {
    it("should inject double[] from C and read back from script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      double data[] = {1.0, 2.0, 3.0, 4.0, 5.0};
      check_int_eq(ts_bind_vec(ctx, "v", data, 5), 0);

      check_int_eq(turbo_script_run(ctx, "s = "
                                         "vec.sum(v); a = vec.avg(v);"),
                   0);
      check_float_eq(ts_get_num(ctx, "s"), 15.0, 0.001);
      check_float_eq(ts_get_num(ctx, "a"), 3.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should extract vector from script back to C") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_run(ctx, "v = "
                                         "split(\"10,20,30\", \",\");"),
                   0);

      const double *out = NULL;
      size_t out_len = 0;
      check_int_eq(ts_get_vec(ctx, "v", &out, &out_len), 0);
      check_int_eq((int)out_len, 3);
      check_float_eq(out[0], 10.0, 0.001);
      check_float_eq(out[1], 20.0, 0.001);
      check_float_eq(out[2], 30.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should return -1 for not-found vector") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const double *out = NULL;
      size_t out_len = 0;
      check_int_eq(ts_get_vec(ctx, "nonexistent", &out, &out_len), -1);
      turbo_script_free(ctx);
    }
  }

  describe("String Variable Access") {
    it("should roundtrip set_var_str and get_var_str") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_str(ctx, "greeting", "hello world");
      const char *val = ts_get_str(ctx, "greeting");
      check_not_null(val);
      check_int_eq(strcmp(val, "hello world"), 0);
      turbo_script_free(ctx);
    }

    it("should return NULL for wrong type") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      ts_bind_num(ctx, "num", 42.0);
      const char *val = ts_get_str(ctx, "num");
      check_null(val);
      turbo_script_free(ctx);
    }
  }

  describe("User Function Registration") {
    it("should bind C function and call from script") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);

      ts_bind_func(ctx, "triple", test_triple_fn, NULL);
      check_int_eq(turbo_script_run(ctx, "r = triple(7);"), 0);
      check_float_eq(ts_get_num(ctx, "r"), 21.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Error Propagation") {
    it("should abort on wrong arg type") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      int res = turbo_script_run(ctx, "r = vec.avg(42);");
      check_int_eq(res, -1);
      turbo_script_free(ctx);
    }

    it("should not abort on runtime failure like missing file") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      int res = turbo_script_run(ctx, "data = "
                                      "read_file(\"nonexistent_file_xyz.txt\");");
      check_int_eq(res, 0);
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: Compound Assignment") {
    it("should support += -= *= /=") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "x = 10; "
                           "x += 5; " // 15
                           "x -= 3; " // 12
                           "x *= 2; " // 24
                           "x /= 4;"; // 6
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "x"), 6.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: For Loop") {
    it("should support for(init; cond; post) { body }") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "sum = 0; "
                           "for (i = 0; i < 10; i += 1) { "
                           "  sum += i; "
                           "}";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "sum"), 45.0, 0.001); // 0+1+...+9
      turbo_script_free(ctx);
    }
  }

  describe("Grammar: Vector Slicing") {
    it("should support arr[start..end] slicing") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = "v = [10, 20, 30, 40, 50]; "
                           "s = v[1..4]; "
                           "l = vec.len(s); "
                           "total = vec.sum(s);";
      check_int_eq(turbo_script_run(ctx, script), 0);
      check_float_eq(ts_get_num(ctx, "l"), 3.0, 0.001);      // elements at index 1,2,3
      check_float_eq(ts_get_num(ctx, "total"), 90.0, 0.001); // 20+30+40
      turbo_script_free(ctx);
    }
  }

  describe("Containers") {
    it("should use broad truthiness for containers") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      const char *script = ""
                           "m0 = map{}; "
                           "m1 = map{a: 1}; "
                           "l0 = list(); "
                           "l1 = list(42); "
                           "miss1 = m1.nope; "
                           "miss2 = m1[\"nope\"]; "
                           "tm0 = if (m0) { 1 } else { 0 }; "
                           "tm1 = if (m1) { 1 } else { 0 }; "
                           "tl0 = if (l0) { 1 } else { 0 }; "
                           "tl1 = if (l1) { 1 } else { 0 };";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Container contract Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "miss1"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "miss2"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tm0"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tm1"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tl0"), 0.0, 0.001);
      check_float_eq(ts_get_num(ctx, "tl1"), 1.0, 0.001);
      turbo_script_free(ctx);
    }
  }

  describe("Quant: Risk Metrics") {
    it("should compute VaR, CVaR, Kelly criterion") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script =
          " returns = [-0.02, 0.01, -0.03, "
          "0.02, 0.01, -0.01, 0.03, -0.02, 0.01, -0.04, "
          "            0.02, 0.01, -0.01, 0.03, -0.02, 0.01, -0.03, 0.02, 0.01, -0.01]; "
          "vh = strategy.var_hist(returns, 0.95); "
          "vp = strategy.var_param(returns, 0.95); "
          "cv = strategy.cvar(returns, 0.95); "
          "k = strategy.kelly(0.6, 0.02, 0.015);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Risk Metrics Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      double vh = ts_get_num(ctx, "vh");
      double vp = ts_get_num(ctx, "vp");
      double cv = ts_get_num(ctx, "cv");
      double k = ts_get_num(ctx, "k");
      printf("  VaR(hist): %.4f\n", vh);
      printf("  VaR(param): %.4f\n", vp);
      printf("  CVaR: %.4f\n", cv);
      printf("  Kelly: %.4f\n", k);

      // VaR should be positive (loss magnitude)
      check_float_gt(vh, 0.0);
      // Kelly should be positive for profitable strategy
      check_float_gt(k, 0.0);

      turbo_script_free(ctx);
    }

    it("should compute drawdown stats") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script = "equity = [100, "
                           "105, 103, 108, 106, 110, 107, 112, 115, 113]; "
                           "dd = strategy.drawdown(equity); "
                           "dd_stats = strategy.drawdown_stats(equity);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Drawdown Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      // dd should be a vector of same length
      const double *dd_data = NULL;
      size_t dd_len = 0;
      check_int_eq(ts_get_vec(ctx, "dd", &dd_data, &dd_len), 0);
      check_int_eq((int)dd_len, 10);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Signal Detection") {
    it("should detect crossover and crossunder") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");
      const char *script = " fast = [1, 3, 5, 4, 2, 4, 6]; "
                           "slow = [2, 2, 4, 5, 3, 3, 5]; "
                           "co = ta.crossover(fast, slow); "
                           "cu = ta.crossunder(fast, slow); "
                           "co_len = vec.len(co); "
                           "cu_len = vec.len(cu);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Signal Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "co_len"), 7.0, 0.001);
      check_float_eq(ts_get_num(ctx, "cu_len"), 7.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Candlestick Patterns") {
    it("should detect doji and hammer patterns") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "ta");
      const char *script =
          // Doji: open ≈ close, long shadows
          "o = [100, 100, 100, 100, 100]; "
          "h = [105, 105, 105, 105, 105]; "
          "l = [ 95,  95,  95,  95,  95]; "
          "c = [100.1, 100.2, 99.9, 100, 100.1]; "
          "doji = ta.candle_doji(o, h, l, c, 0.1); "
          "hammer = ta.candle_hammer(o, h, l, c); "
          "doji_len = vec.len(doji); "
          "hammer_len = vec.len(hammer);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Candle Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      check_float_eq(ts_get_num(ctx, "doji_len"), 5.0, 0.001);
      check_float_eq(ts_get_num(ctx, "hammer_len"), 5.0, 0.001);

      turbo_script_free(ctx);
    }
  }

  describe("Quant: Portfolio Optimization") {
    it("should compute minimum variance weights") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script =
          // 2x2 covariance matrix (2 assets): [var1, cov12, cov21, var2]
          "cov = [0.04, 0.01, 0.01, 0.09]; "
          "w = strategy.pf_min_variance(cov); "
          "w_len = vec.len(w);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Portfolio Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      const double *w_data = NULL;
      size_t w_len = 0;
      check_int_eq(ts_get_vec(ctx, "w", &w_data, &w_len), 0);
      check_int_eq((int)w_len, 2);

      // Weights should sum to ~1
      double w_sum = w_data[0] + w_data[1];
      printf("  Weights: [%.4f, %.4f] sum=%.4f\n", w_data[0], w_data[1], w_sum);
      check_float_eq(w_sum, 1.0, 0.05);

      turbo_script_free(ctx);
    }
    it("should compute minimum variance weights for three assets") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      turbo_script_load_plugin(ctx, "fin");
      const char *script =
          "cov = [0.04, 0.01, 0.00, "
          "       0.01, 0.09, 0.02, "
          "       0.00, 0.02, 0.16]; "
          "w = strategy.pf_min_variance(cov);";
      int res = turbo_script_run(ctx, script);
      if (res != 0) printf("Portfolio 3 Asset Error: %s\n", turbo_script_get_error(ctx));
      check_int_eq(res, 0);

      const double *w_data = NULL;
      size_t w_len = 0;
      check_int_eq(ts_get_vec(ctx, "w", &w_data, &w_len), 0);
      check_int_eq((int)w_len, 3);
      check_float_eq(w_data[0] + w_data[1] + w_data[2], 1.0, 0.001);

      turbo_script_free(ctx);
    }

    it("should not expose finance risk helpers through ta aliases") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_int_eq(turbo_script_load_plugin(ctx, "ta"), 0);
      check_int_eq(turbo_script_load_plugin(ctx, "fin"), 0);

      const char *script = "returns = [-0.02, 0.01, -0.03]; "
                           "bad = try { ta.var_hist(returns, 0.95); 0 } catch (e) { 1 };";
      int res = turbo_script_run(ctx, script);
      check_int_eq(res, 0);
      check_float_eq(ts_get_num(ctx, "bad"), 1.0, 0.001);

      turbo_script_free(ctx);
    }
  }

}
