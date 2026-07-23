# TurboScript Plugin API Index

Source of truth: `tScript/modules/*/src`.
Load plugins via `import("name")` or `turbo_script_load_plugin(ctx, "name")`.

## csv plugin (`csv`)

Count: 12

Expression syntax for `csv.filter` / `csv.filter_count`:
- [csv_filter_expression.md](./csv_filter_expression.md)

- `csv.close`
- `csv.col`
- `csv.cols`
- `csv.filter`
- `csv.filter_count`
- `csv.get`
- `csv.ts_get_num`
- `csv.open`
- `csv.rows`
- `csv.stream_file`
- `csv.stream_http`
- `csv.write`

## json plugin (`json`)

Count: 2

- `json.query`
- `json.to_vec`

## net plugin (`net`)

Count: 6

- `http.get`
- `http.post`
- `ws.close`
- `ws.connect`
- `ws.consume`
- `ws.send`

## sqlite plugin (`sqlite`)

Count: 6

- `sqlite.close`
- `sqlite.error`
- `sqlite.exec`
- `sqlite.open`
- `sqlite.query_col`
- `sqlite.query_scalar`

## data_bind plugin (`data_bind`)

Count: 3

- `data_bind.create`
- `data_bind.parse`
- `data_bind.close`

## fuzzy plugin (`fuzzy`)

Count: 11

- `fuzzy.search`
- `fuzzy.regex`
- `fuzzy.regex_search`
- `fuzzy.fuzzy_search`
- `fuzzy.fuzzy_regex_search`
- `fuzzy.levenshtein`
- `fuzzy.lev`
- `fuzzy.agrep`
- `fuzzy.ac`
- `fuzzy.multi_search`
- `fuzzy.tre_version`

## wasm plugin (`wasm`)

Count: 4

- `wasm.open`
- `wasm.call`
- `wasm.close`
- `wasm.last_error`

## vec plugin (`vec`)

Count: 14

- `vec.avg`
- `vec.concat`
- `vec.cumsum`
- `vec.diff`
- `vec.find`
- `vec.len`
- `vec.max`
- `vec.min`
- `vec.range`
- `vec.reverse`
- `vec.sort`
- `vec.sort_desc`
- `vec.sum`
- `vec.unique`

## ts plugin (`ts`)

Count: 30

- `ts.adf`
- `ts.autocorr`
- `ts.coint`
- `ts.cum_return`
- `ts.diff`
- `ts.dwt`
- `ts.emd`
- `ts.ewm_mean`
- `ts.ewm_std`
- `ts.expanding_mean`
- `ts.expanding_std`
- `ts.garch`
- `ts.hurst`
- `ts.log_return`
- `ts.match`
- `ts.match_candle`
- `ts.match_correl`
- `ts.match_cosine`
- `ts.match_dtw`
- `ts.match_normalized`
- `ts.match_returns`
- `ts.pacf`
- `ts.pct_change`
- `ts.rolling_beta`
- `ts.rolling_corr`
- `ts.rolling_kurt`
- `ts.rolling_mean`
- `ts.rolling_skew`
- `ts.rolling_std`
- `ts.spread`

## fin plugin (`fin` -> `strategy.*`)

Count: 65

- `strategy.atr_sl`
- `strategy.buy`
- `strategy.calmar`
- `strategy.cvar`
- `strategy.cum_pnl`
- `strategy.drawdown`
- `strategy.drawdown_stats`
- `strategy.entry_px`
- `strategy.expectancy`
- `strategy.flat`
- `strategy.information_ratio`
- `strategy.is_flat`
- `strategy.is_long`
- `strategy.is_short`
- `strategy.kelly`
- `strategy.kelly_size`
- `strategy.last`
- `strategy.max_dd_duration`
- `strategy.num_trades`
- `strategy.payoff_ratio`
- `strategy.pf_min_variance`
- `strategy.pos`
- `strategy.prev`
- `strategy.profit_factor`
- `strategy.rank_pct`
- `strategy.resample_ohlcv`
- `strategy.risk_size`
- `strategy.sell`
- `strategy.set_sl`
- `strategy.set_tp`
- `strategy.sharpe`
- `strategy.sortino`
- `strategy.treynor`
- `strategy.ulcer_index`
- `strategy.unrealized_pnl`
- `strategy.var_hist`
- `strategy.var_param`
- `strategy.vec_all`
- `strategy.vec_any`
- `strategy.vec_at`
- `strategy.vec_corr`
- `strategy.vec_cross`
- `strategy.vec_fill`
- `strategy.vec_filter_gt`
- `strategy.vec_filter_lt`
- `strategy.vec_max`
- `strategy.vec_mean`
- `strategy.vec_min`
- `strategy.vec_rank`
- `strategy.vec_resample`
- `strategy.vec_roll_max`
- `strategy.vec_roll_min`
- `strategy.vec_shift`
- `strategy.vec_sort_idx`
- `strategy.vec_std`
- `strategy.vec_sum`
- `strategy.vec_top`
- `strategy.vec_vmax`
- `strategy.vec_vmin`
- `strategy.vec_where`
- `strategy.walk_forward`
- `strategy.win_rate`

## ta plugin (`ta`)

Count: 112

- `ta.ad`
- `ta.adosc`
- `ta.adx`
- `ta.adxr`
- `ta.alma`
- `ta.apo`
- `ta.aroon`
- `ta.aroonosc`
- `ta.atr`
- `ta.avgprice`
- `ta.bbands`
- `ta.bbi`
- `ta.beta`
- `ta.boll`
- `ta.boll_dn`
- `ta.boll_up`
- `ta.bsm_call`
- `ta.bsm_delta_call`
- `ta.bsm_delta_put`
- `ta.bsm_gamma`
- `ta.bsm_iv_call`
- `ta.bsm_iv_put`
- `ta.bsm_put`
- `ta.bsm_rho_call`
- `ta.bsm_rho_put`
- `ta.bsm_theta_call`
- `ta.bsm_theta_put`
- `ta.bsm_vega`
- `ta.candle_doji`
- `ta.candle_hammer`
- `ta.cci`
- `ta.chop`
- `ta.cmo`
- `ta.correl`
- `ta.crossunder`
- `ta.crossover`
- `ta.dema`
- `ta.donchian`
- `ta.donchian_dn`
- `ta.donchian_up`
- `ta.dx`
- `ta.ehlers_cyber_cycle`
- `ta.ehlers_fama`
- `ta.ehlers_itrend`
- `ta.ehlers_mama`
- `ta.ema`
- `ta.fisher`
- `ta.fisher_trigger`
- `ta.hma`
- `ta.ichimoku`
- `ta.ichimoku_chi`
- `ta.ichimoku_kijun`
- `ta.ichimoku_sa`
- `ta.ichimoku_sb`
- `ta.ichimoku_tenkan`
- `ta.kama`
- `ta.keltner`
- `ta.keltner_dn`
- `ta.keltner_mid`
- `ta.keltner_up`
- `ta.linearreg`
- `ta.linearreg_angle`
- `ta.linearreg_intercept`
- `ta.linearreg_slope`
- `ta.macd`
- `ta.medprice`
- `ta.mfi`
- `ta.midpoint`
- `ta.midprice`
- `ta.minus_di`
- `ta.minus_dm`
- `ta.mom`
- `ta.natr`
- `ta.obv`
- `ta.opt_binomial`
- `ta.plus_di`
- `ta.plus_dm`
- `ta.ppo`
- `ta.rma`
- `ta.roc`
- `ta.rsi`
- `ta.rvi`
- `ta.sar`
- `ta.savgol`
- `ta.sma`
- `ta.squeeze`
- `ta.squeeze_on`
- `ta.stddev`
- `ta.stoch`
- `ta.stochrsi`
- `ta.supertrend`
- `ta.t3`
- `ta.tema`
- `ta.trange`
- `ta.trima`
- `ta.trix`
- `ta.tsf`
- `ta.typprice`
- `ta.ultosc`
- `ta.var`
- `ta.vhf`
- `ta.vidya`
- `ta.volatility_ratio`
- `ta.vwap`
- `ta.wclprice`
- `ta.willr`
- `ta.wma`
- `ta.zlema`
