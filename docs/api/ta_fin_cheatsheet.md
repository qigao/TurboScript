# TurboScript TA & Finance (fin) Cheat Sheet

面向量化金融、策略编写和技术分析的速查表。
TurboScript 提供专为高性能回测和交易设计的双引擎模块：`ta` (技术分析与定价) 和 `fin` (`strategy.*` 交易上下文与统计分析)。

> **提示:** 这些模块属于外部插件。在脚本运行前请确保通过 `import("ta")` 和 `import("fin")` 加载，或在宿主程序中 `turbo_script_load_plugin(ctx, "ta")` / `turbo_script_load_plugin(ctx, "fin")`。

## 技术指标 (TA)

所有技术指标原生支持向量操作 (SIMD 优化)，支持点号调用 `v.sma(14)`。

| 函数 / 别名 | 说明 | 示例 |
|---|---|---|
| `ta.sma(v, period)` | 简单移动平均 | `close.sma(14)` |
| `ta.ema(v, period)` | 指数移动平均 | `ta.ema(close, 20)` |
| `ta.macd(v, fast, slow, sig)` | 平滑异同移动平均线 | `var [macd, sig, hist] = close.macd(12, 26, 9)` |
| `ta.rsi(v, period)` | 相对强弱指数 | `close.rsi(14)` |
| `ta.bbands(v, period, dev)` | 布林带 | `var [up, mid, dn] = close.bbands(20, 2.0)` |
| `ta.atr(h, l, c, period)` | 真实波动幅度 | `ta.atr(high, low, close, 14)` |
| `ta.stoch(h, l, c, k, d)` | 随机指标 (KDJ) | `ta.stoch(high, low, close, 9, 3)` |
| `ta.candle_doji(o, h, l, c)` | 十字星形态识别 | `ta.candle_doji(open, high, low, close)` |
| `ta.crossover(v1, v2)` | 金叉判定 (v1 上穿 v2) | `ta.crossover(sma5, sma10)` |
| `ta.crossunder(v1, v2)` | 死叉判定 (v1 下穿 v2) | `ta.crossunder(sma5, sma10)` |

### 期权定价 (Black-Scholes-Merton)

| 函数 | 说明 | 示例 (S:标的, K:行权, T:时间, r:利率, sigma:波动率) |
|---|---|---|
| `ta.bsm_call(S, K, T, r, v)` | 看涨期权定价 | `ta.bsm_call(100, 105, 0.5, 0.02, 0.2)` |
| `ta.bsm_put(S, K, T, r, v)` | 看跌期权定价 | `ta.bsm_put(100, 95, 0.5, 0.02, 0.2)` |
| `ta.bsm_iv_call(...)` | 看涨隐含波动率求导 | `ta.bsm_iv_call(price, S, K, T, r)` |
| `ta.bsm_delta_call(...)` | 希腊字母 Delta (Call) | `ta.bsm_delta_call(S, K, T, r, v)` |

---

## 策略上下文与执行 (fin / strategy)

`strategy.*` 提供了一套完整的环境来进行信号发生、头寸管理与绩效评估。

### 交易执行

| 函数 | 说明 | 示例 |
|---|---|---|
| `strategy.buy(size)` | 做多 / 平空 (指定数量) | `strategy.buy(1.0)` |
| `strategy.sell(size)` | 做空 / 平多 (指定数量) | `strategy.sell(1.0)` |
| `strategy.flat()` | 清仓 (平掉所有当前持仓) | `strategy.flat()` |
| `strategy.set_sl(price)` | 设置当前仓位的止损价 | `strategy.set_sl(entry_px - atr * 2)` |
| `strategy.set_tp(price)` | 设置当前仓位的止盈价 | `strategy.set_tp(entry_px + atr * 3)` |

### 仓位与状态

| 函数 | 说明 | 示例 |
|---|---|---|
| `strategy.pos()` | 获取当前持仓数量 (正多负空) | `if (strategy.pos() == 0) { ... }` |
| `strategy.is_long()` | 是否持有多头 | `if (strategy.is_long()) { ... }` |
| `strategy.is_short()`| 是否持有空头 | `if (strategy.is_short()) { ... }` |
| `strategy.is_flat()` | 是否空仓 | `if (strategy.is_flat()) { ... }` |
| `strategy.entry_px()`| 当前仓位的平均开仓价 | `var entry = strategy.entry_px()` |

### 绩效评估统计

| 函数 | 说明 |
|---|---|
| `strategy.cum_pnl()` | 获取策略当前累计收益 (Cumulative PnL) |
| `strategy.unrealized_pnl()` | 获取当前仓位的未实现盈亏 (浮盈/浮亏) |
| `strategy.num_trades()` | 获取完成的交易次数 |
| `strategy.win_rate()` | 胜率 (盈利记录数 / 总记录数) |
| `strategy.sharpe()` | 夏普比率 (风险调整后收益) |
| `strategy.max_dd_duration()` | 最大回撤持续时间 |
| `strategy.kelly_size()` | 计算凯利公式推荐的最佳头寸比例 |

### 风险与组合优化

| 函数 | 说明 | 示例 |
|---|---|---|
| `strategy.var_hist(returns, conf)` | 历史模拟 VaR，返回损失幅度 | `strategy.var_hist(returns, 0.95)` |
| `strategy.var_param(returns, conf)` | 参数法 VaR，使用正态近似 | `strategy.var_param(returns, 0.95)` |
| `strategy.cvar(returns, conf)` | 条件 VaR / Expected Shortfall | `strategy.cvar(returns, 0.95)` |
| `strategy.kelly(win_rate, avg_win, avg_loss)` | 凯利比例 | `strategy.kelly(0.6, 0.02, 0.015)` |
| `strategy.drawdown(equity)` | 回撤序列 | `strategy.drawdown(equity)` |
| `strategy.drawdown_stats(equity)` | `[max_drawdown, max_duration]` | `strategy.drawdown_stats(equity)` |
| `strategy.pf_min_variance(cov)` | 最小方差权重；`cov` 是 flat n×n row-major 协方差矩阵，返回 n 个权重；奇异矩阵返回等权重 | `strategy.pf_min_variance([0.04,0.01,0.01,0.09])` |

---

## 组合示例：完整的趋势交叉策略

```js
// 1. 获取行情并计算指标
var sma_fast = close.sma(10);
var sma_slow = close.sma(30);

// 2. 状态获取
var is_flat = strategy.is_flat();
var is_long = strategy.is_long();

// 3. 交易逻辑
if (ta.crossover(sma_fast, sma_slow)) {
    // 金叉做多
    if (!is_long) {
        strategy.flat();
        strategy.buy(100); 
    }
} else if (ta.crossunder(sma_fast, sma_slow)) {
    // 死叉做空或平仓
    if (is_long) {
        strategy.flat();
    }
}

// 4. 风控与止损
if (!is_flat) {
    var entry = strategy.entry_px();
    var risk  = ta.atr(high, low, close, 14) * 2;
    if (is_long) {
        strategy.set_sl(entry - risk);
    }
}
```

---

## 资产域管理 (Universe)

`universe.*` 提供回测中资产生命周期管理、除权除息、活跃资产过滤等功能 (SIMD 加速)。

### 生命周期

| 函数 | 说明 |
|---|---|
| `universe_create(arena)` | 创建空域 |
| `universe_add_asset(u, asset)` | 注册可交易资产 |
| `universe_add_adjustment(u, adj)` | 添加拆股/分红事件 |
| `universe_finalize(u)` | 排序调整因子、分配运行时数组 |
| `universe_advance(u, date)` | 推进时钟到新交易日 (更新 active_mask) |
| `universe_free(u)` | 释放 |

### 查询

| 函数 | 说明 |
|---|---|
| `universe_is_active(u, id)` | 资产当日是否可交易 |
| `universe_active_count(u)` | 当前活跃资产数量 |
| `universe_active_ids(u, out, max)` | 填充活跃 ID 列表 |
| `universe_adj_factor(u, id)` | 获取累积复权因子 |
| `universe_adjust_price(u, id, px)` | 对单条价格复权 |
| `universe_delisted_today(u, out, max)` | 当日退市的资产 ID |

### 横截面运算 (SIMD 加速)

| 函数 | 说明 | SIMD 原语 |
|---|---|---|
| `universe_rank(u, vals, n, out)` | 活跃资产百分位排名 [0,1] | `simd_fill` + `compare_rank_items` |
| `universe_top_n(u, vals, n, k, ids)` | Top-k 资产 ID (降序) | `rank_item_t` |
| `universe_filter_gt(u, vals, n, thr, mask)` | 布尔掩码: 活跃 & 值>阈值 | — |
| `universe_adjust_prices(u, raw, adj, n)` | 批量复权 | `simd_mul` |
| `universe_zscore(u, vals, n, out)` | Z-Score 标准化 | `simd_mean_variance` + `simd_zscore` |
| `universe_demean(u, vals, n, out)` | 减去截面均值 | `simd_sum` + `simd_sub` |
| `universe_clip(u, vals, n, lo, hi, out)` | 截断到 [lo, hi] | `simd_element_max/min_scalar` |
| `universe_cross_sum(u, vals, n)` | 活跃资产求和 | `simd_sum` |

---

## 图算法 (Graph)

| 函数 | 说明 |
|---|---|
| `graph.bellman_ford(edges, nv, ne, src, dist, prev)` | Bellman-Ford 最短路 |
| `graph.has_negative_cycle(edges, nv, ne)` | 检测负环 |
| `graph.extract_path(prev, src, tgt, path)` | 从前驱数组提取路径 |
| `graph.detect_arbitrage(rates, n)` | 汇率三角套利检测 (`-log` 转换) |
