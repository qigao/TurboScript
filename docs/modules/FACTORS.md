# Factor Processing - Alpha Generation Guide

> **🎓 Advanced: Build sophisticated factor models for quantitative trading**

---

## 📋 Who Should Read This

- **🎓 Quant Researchers**: Building alpha factors and trading signals
- **📊 Data Scientists**: Feature engineering for ML models
- **🔧 Developers**: Integrating factor processing into trading systems

**Prerequisites:** Basic understanding of statistics and quantitative finance. See [FIN_MODULE.md](FIN_MODULE.md) for introduction.

---

## 🎯 What Are Factors?

**Factors** are quantitative measures that explain asset returns. They form the foundation of:

- **Alpha generation**: Finding profitable trading signals
- **Risk modeling**: Understanding portfolio exposures
- **Portfolio construction**: Combining signals optimally

### TL;DR - Factor Categories

```
📊 Standardization (4 functions)
   → rank, zscore, winsorize, standardize
   → Purpose: Normalize data for cross-sectional comparison

📈 Distribution (3 functions)
   → skewness, kurtosis, entropy
   → Purpose: Measure distribution properties and tail risk

⚡ Realized Volatility (3 functions)
   → rvar, rskew, rkurt
   → Purpose: High-frequency volatility measures

🏛️ Market Microstructure (5 functions)
   → illiq, vol_ratio, trend_strength, efficiency, cgo
   → Purpose: Market quality and liquidity indicators

🌐 Cross-Sectional (5 functions)
   → csad, quantile, fvd, rsj, apm
   → Purpose: Multi-asset analysis and herding detection

🧠 Behavioral Finance (2 functions)
   → salience, str
   → Purpose: Attention-driven and reversal factors
```

---

## 📊 Basic Standardization

### `exprtk_vec_rank` - Cross-Sectional Ranking

**🎓 Purpose**: Convert values to ranks for percentile-based signals.

```c
size_t exprtk_vec_rank(const double *in, size_t n,
                       double *out, turbo_pool_t *arena);
```

**Returns:** Ranks from 0 (lowest) to n-1 (highest)

#### Example: Stock Selection

```c
// Rank 5 stocks by momentum
double momentum[] = {0.15, 0.08, 0.22, 0.05, 0.18};
double ranks[5];

exprtk_vec_rank(momentum, 5, ranks, &arena);
// ranks = [2, 1, 4, 0, 3]
//          ↑  ↑  ↑  ↑  ↑
//        15% 8% 22% 5% 18%

// Long-short portfolio construction
for (int i = 0; i < 5; i++) {
    if (ranks[i] >= 4) {
        // Top 20% (rank 4) - go long
        printf("Stock %d: LONG (rank %.0f)\n", i, ranks[i]);
    } else if (ranks[i] == 0) {
        // Bottom 20% (rank 0) - go short
        printf("Stock %d: SHORT (rank %.0f)\n", i, ranks[i]);
    }
}
```

**💡 Why ranking?**
- **Robust to outliers**: Extreme values don't dominate
- **Non-parametric**: No distribution assumptions
- **Cross-sectional**: Compares assets at same point in time

**⚠️ Handles ties**: Uses average rank for tied values.

---

### `exprtk_vec_zscore` - Z-Score Normalization

**🎓 Purpose**: Standardize to zero mean and unit variance for combining factors.

```c
size_t exprtk_vec_zscore(const double *in, size_t n,
                         double *out, turbo_pool_t *arena);
```

**Formula:** `z = (x - μ) / σ` where μ is mean, σ is standard deviation

#### Example: Multi-Factor Combination

```c
// Three different factors with different scales
double momentum[] = {0.15, 0.08, 0.22, 0.05, 0.18};     // Returns (%)
double value[] = {1.2, 0.8, 1.5, 0.9, 1.1};             // P/E ratios
double quality[] = {0.85, 0.92, 0.78, 0.88, 0.90};      // ROE

// Standardize each factor
double z_momentum[5], z_value[5], z_quality[5];
exprtk_vec_zscore(momentum, 5, z_momentum, &arena);
exprtk_vec_zscore(value, 5, z_value, &arena);
exprtk_vec_zscore(quality, 5, z_quality, &arena);

// Combine with equal weights
double combined[5];
for (int i = 0; i < 5; i++) {
    combined[i] = (z_momentum[i] + z_value[i] + z_quality[i]) / 3.0;
    printf("Stock %d: Combined Score = %.2f\n", i, combined[i]);
}
```

**💡 When to use:**
- Combining factors with different units
- Detecting outliers (|z| > 2 or 3)
- Statistical arbitrage signals

**⚠️ Note**: Uses population variance (n), not sample variance (n-1).

---

### `exprtk_vec_winsorize` - Outlier Capping

**🎓 Purpose**: Cap extreme values to reduce outlier impact.

```c
size_t exprtk_vec_winsorize(const double *in, size_t n,
                            double limit_pct, double *out,
                            turbo_pool_t *arena);
```

**Parameters:**
- `limit_pct`: Percentile to cap at (e.g., 0.05 for 5th/95th percentiles)

#### Example: Robust Factor Construction

```c
// Raw returns with outlier
double returns[] = {0.01, 0.02, -0.01, 0.50, 0.01};  // 50% is outlier
double winsorized[5];

// Cap at 10th/90th percentiles
exprtk_vec_winsorize(returns, 5, 0.1, winsorized, &arena);

printf("Original:   ");
for (int i = 0; i < 5; i++) printf("%.2f ", returns[i]);
printf("\nWinsorized: ");
for (int i = 0; i < 5; i++) printf("%.2f ", winsorized[i]);
// Output: 50% capped to 90th percentile value
```

**💡 Best practices:**
- Use 5-10% for typical applications
- Apply before calculating factors
- Reduces impact of data errors

---

### `exprtk_vec_standardize` - Min-Max Scaling

**🎓 Purpose**: Scale to [0, 1] range for bounded inputs.

```c
size_t exprtk_vec_standardize(const double *in, size_t n,
                              double *out, turbo_pool_t *arena);
```

**Formula:** `x_scaled = (x - min) / (max - min)`

#### Example: Neural Network Inputs

```c
double prices[] = {100, 150, 120, 180, 110};
double scaled[5];

exprtk_vec_standardize(prices, 5, scaled, &arena);
// scaled = [0.0, 0.625, 0.25, 1.0, 0.125]
//           ↑    ↑      ↑     ↑    ↑
//          min  mid    mid   max  low
```

**💡 Use cases:**
- Neural network inputs
- Visualization
- Bounded factor values

---

## 📈 Distribution Statistics

### `exprtk_vec_skewness` - Distribution Asymmetry

**🎓 Purpose**: Measure tail asymmetry for risk assessment.

```c
double exprtk_vec_skewness(const double *in, size_t n);
```

**Returns:**
- **Positive**: Right-skewed (long right tail, more extreme gains)
- **Negative**: Left-skewed (long left tail, more extreme losses)
- **Zero**: Symmetric distribution

#### Example: Crash Risk Detection

```c
// Two strategies with same mean/variance
double strategy_a[] = {0.01, 0.01, 0.01, 0.01, 0.01};  // Consistent
double strategy_b[] = {-0.05, 0.02, 0.02, 0.02, 0.04}; // Crash risk

double skew_a = exprtk_vec_skewness(strategy_a, 5);
double skew_b = exprtk_vec_skewness(strategy_b, 5);

printf("Strategy A skewness: %.3f (symmetric)\n", skew_a);
printf("Strategy B skewness: %.3f (negative = crash risk)\n", skew_b);

if (skew_b < -0.5) {
    printf("⚠️ WARNING: High crash risk detected!\n");
}
```

**💡 Trading implications:**
- **Negative skew**: Avoid or hedge (crash risk)
- **Positive skew**: Prefer (lottery-like payoffs)
- **Regime detection**: Skew changes before crises

---

### `exprtk_vec_kurtosis` - Tail Thickness

**🎓 Purpose**: Measure tail risk (excess kurtosis).

```c
double exprtk_vec_kurtosis(const double *in, size_t n);
```

**Returns:**
- **Positive**: Fat tails (more extreme events than normal)
- **Negative**: Thin tails (fewer extremes)
- **Zero**: Normal distribution

#### Example: VaR Adjustment

```c
double returns[] = {0.01, 0.02, -0.01, 0.05, -0.04, 0.01, -0.03};
double kurt = exprtk_vec_kurtosis(returns, 7);

printf("Kurtosis: %.3f\n", kurt);

if (kurt > 1.0) {
    printf("⚠️ Fat tails detected!\n");
    printf("→ Standard VaR underestimates risk\n");
    printf("→ Use higher confidence level or CVaR\n");
}
```

**💡 Risk management:**
- **High kurtosis**: Increase risk buffer
- **Crisis periods**: Kurtosis spikes
- **Option pricing**: Affects volatility smile

---

### `exprtk_vec_entropy` - Information Content

**🎓 Purpose**: Measure distribution uniformity.

```c
double exprtk_vec_entropy(const double *in, size_t n, size_t bins);
```

**Returns:**
- **High**: Uniform distribution (more uncertainty)
- **Low**: Concentrated distribution (less uncertainty)

#### Example: Market Efficiency

```c
// Two markets
double efficient_market[] = {0.01, -0.01, 0.02, -0.02, 0.01};  // Random
double trending_market[] = {0.02, 0.02, 0.02, 0.02, 0.02};     // Trending

double entropy_eff = exprtk_vec_entropy(efficient_market, 5, 5);
double entropy_trend = exprtk_vec_entropy(trending_market, 5, 5);

printf("Efficient market entropy: %.3f (high = random)\n", entropy_eff);
printf("Trending market entropy:  %.3f (low = predictable)\n", entropy_trend);
```

---

## ⚡ Realized Volatility

### `exprtk_vec_rvar` - Realized Variance

**🎓 Purpose**: High-frequency volatility measure.

```c
double exprtk_vec_rvar(const double *in, size_t n);
```

**Formula:** `RV = Σ r² / n`

#### Example: Volatility Forecasting

```c
// Intraday 5-minute returns
double returns_5min[78];  // 6.5 hours * 12 periods/hour
// ... load data ...

double daily_rvar = exprtk_vec_rvar(returns_5min, 78);
double daily_vol = sqrt(daily_rvar);

printf("Realized volatility: %.2f%%\n", daily_vol * 100);

// Annualize
double annual_vol = daily_vol * sqrt(252);
printf("Annualized volatility: %.2f%%\n", annual_vol * 100);
```

**💡 Advantages over standard deviation:**
- Uses high-frequency data
- More accurate volatility estimates
- Better for option pricing

---

### `exprtk_vec_rskew` - Realized Skewness

**🎓 Purpose**: Intraday tail asymmetry.

```c
double exprtk_vec_rskew(const double *in, size_t n);
```

#### Example: Jump Risk Detection

```c
double rskew = exprtk_vec_rskew(returns_5min, 78);

if (rskew < -1.0) {
    printf("⚠️ Negative jump risk detected!\n");
    printf("→ Consider protective puts\n");
} else if (rskew > 1.0) {
    printf("✅ Positive jump potential\n");
    printf("→ Consider call options\n");
}
```

---

### `exprtk_vec_rkurt` - Realized Kurtosis

**🎓 Purpose**: Intraday tail thickness.

```c
double exprtk_vec_rkurt(const double *in, size_t n);
```

#### Example: Volatility Regime Detection

```c
double rkurt = exprtk_vec_rkurt(returns_5min, 78);

if (rkurt > 3.0) {
    printf("⚠️ High intraday volatility regime\n");
    printf("→ Reduce position sizes\n");
    printf("→ Widen stops\n");
}
```

---

## 🏛️ Market Microstructure

### `exprtk_vec_illiq` - Amihud Illiquidity

**🎓 Purpose**: Measure price impact of trading.

```c
double exprtk_vec_illiq(const double *ret, const double *amount, size_t n);
```

**Formula:** `ILLIQ = avg(|return| / dollar_volume)`

**Returns:** Higher = less liquid (harder to trade)

#### Example: Liquidity Risk Premium

```c
// 5 days of data
double returns[] = {0.01, -0.02, 0.015, -0.01, 0.02};
double volumes[] = {1000000, 1500000, 800000, 1200000, 900000};  // Dollar volume

double illiq = exprtk_vec_illiq(returns, volumes, 5);

printf("Illiquidity: %.6f\n", illiq);

if (illiq > 0.00001) {
    printf("⚠️ High illiquidity!\n");
    printf("→ Expect higher transaction costs\n");
    printf("→ Use limit orders\n");
    printf("→ Split large orders\n");
}
```

**💡 Trading implications:**
- **High illiquidity**: Requires liquidity premium
- **Transaction costs**: Scale with illiquidity
- **Portfolio construction**: Constrain illiquid assets

---

### `exprtk_vec_trend_strength` - Trend Quality

**🎓 Purpose**: Measure trend persistence (R² of linear regression).

```c
double exprtk_vec_trend_strength(const double *price, size_t n);
```

**Returns:** 0.0 (no trend) to 1.0 (perfect trend)

#### Example: Trend Following vs Mean Reversion

```c
double prices[] = {100, 101, 102, 103, 104, 105};
double strength = exprtk_vec_trend_strength(prices, 6);

printf("Trend strength: %.3f\n", strength);

if (strength > 0.7) {
    printf("✅ Strong trend - use momentum strategy\n");
} else if (strength < 0.3) {
    printf("⚠️ Weak trend - use mean reversion\n");
} else {
    printf("⚡ Mixed regime - stay flat\n");
}
```

---

### `exprtk_vec_efficiency` - Price Efficiency

**🎓 Purpose**: Measure directional movement efficiency.

```c
double exprtk_vec_efficiency(const double *price, size_t n);
```

**Formula:** `Efficiency = net_movement / total_movement`

**Returns:** 0.0 (random) to 1.0 (straight line)

#### Example: Market Regime Classification

```c
double prices[] = {100, 101, 103, 102, 105, 104, 107};
double efficiency = exprtk_vec_efficiency(prices, 7);

printf("Efficiency: %.3f\n", efficiency);

if (efficiency > 0.6) {
    printf("✅ Efficient trending market\n");
    printf("→ Follow the trend\n");
} else if (efficiency < 0.4) {
    printf("⚠️ Choppy market\n");
    printf("→ Reduce position sizes\n");
    printf("→ Use wider stops\n");
}
```

---

## 🌐 Cross-Sectional Factors

### `exprtk_vec_csad` - Cross-Sectional Absolute Deviation

**🎓 Purpose**: Detect herding behavior across assets.

```c
size_t exprtk_vec_csad(const double *rets, size_t na, size_t np,
                       double *out, turbo_pool_t *arena);
```

**Parameters:**
- `rets`: Returns matrix (na assets × np periods, row-major)
- `na`: Number of assets
- `np`: Number of periods
- `out`: Output CSAD for each period (length np)

#### Example: Herding Detection

```c
// 3 stocks, 5 days
double returns[] = {
    0.01, 0.02, -0.01, 0.03, 0.01,  // Stock 1
    0.02, 0.01,  0.00, 0.02, 0.02,  // Stock 2
   -0.01, 0.03,  0.02, 0.01, 0.00   // Stock 3
};
double csad[5];

exprtk_vec_csad(returns, 3, 5, csad, &arena);

for (int t = 0; t < 5; t++) {
    printf("Day %d CSAD: %.4f ", t, csad[t]);
    if (csad[t] < 0.01) {
        printf("⚠️ HERDING!\n");
    } else {
        printf("✅ Dispersion\n");
    }
}
```

**💡 Interpretation:**
- **Low CSAD**: Herding (all stocks move together)
- **High CSAD**: Dispersion (stock-picking opportunity)
- **Crisis indicator**: CSAD drops before crashes

---

### `exprtk_vec_quantile` - Rolling Quantile

**🎓 Purpose**: Calculate rolling percentile rank.

```c
size_t exprtk_vec_quantile(const double *in, size_t n, size_t window,
                           double *out, turbo_pool_t *arena);
```

#### Example: Relative Strength Indicator

```c
double prices[] = {100, 102, 101, 105, 103, 108, 107};
double quantile[7];

exprtk_vec_quantile(prices, 7, 3, quantile, &arena);  // 3-day window

for (int i = 0; i < 7; i++) {
    printf("Day %d: Price=%.0f, Quantile=%.2f ", i, prices[i], quantile[i]);
    if (quantile[i] > 0.8) {
        printf("🔥 OVERBOUGHT\n");
    } else if (quantile[i] < 0.2) {
        printf("❄️ OVERSOLD\n");
    } else {
        printf("✅ NEUTRAL\n");
    }
}
```

---

## 🧠 Behavioral Finance

### `exprtk_vec_salience` - Attention Factor

**🎓 Purpose**: Measure attention-grabbing price movements.

```c
double exprtk_vec_salience(const double *ret, const double *mkt_ret,
                           size_t n, double delta);
```

**Theory:** Investors pay more attention to stocks with extreme deviations from market.

#### Example: Attention-Driven Trading

```c
double stock_ret[] = {0.01, 0.05, -0.02, 0.08, -0.01};
double market_ret[] = {0.01, 0.02, -0.01, 0.02, 0.00};

double salience = exprtk_vec_salience(stock_ret, market_ret, 5, 0.7);

printf("Salience: %.3f\n", salience);

if (salience > 0.5) {
    printf("🔥 High attention stock!\n");
    printf("→ Expect short-term overreaction\n");
    printf("→ Fade the move (contrarian)\n");
}
```

---

### `exprtk_vec_str` - Short-Term Reversal

**🎓 Purpose**: Measure mean reversion tendency.

```c
double exprtk_vec_str(const double *ret, const double *mkt_ret,
                      size_t n, double delta);
```

**Formula:** Negative autocorrelation of excess returns

#### Example: Mean Reversion Strategy

```c
double stock_ret[] = {0.02, -0.01, 0.03, -0.02, 0.01, -0.01};
double market_ret[] = {0.01, 0.00, 0.01, -0.01, 0.00, 0.00};

double str = exprtk_vec_str(stock_ret, market_ret, 6, 1.0);

printf("STR: %.3f\n", str);

if (str < -0.3) {
    printf("✅ Strong mean reversion\n");
    printf("→ Fade recent moves\n");
    printf("→ Buy dips, sell rips\n");
} else if (str > 0.3) {
    printf("⚠️ Momentum regime\n");
    printf("→ Follow the trend\n");
}
```

---

## 🎯 Best Practices

### 1. Factor Combination Pipeline

```c
// Professional factor construction workflow
turbo_pool_t arena;
turbo_pool_init(&arena, 1024 * 1024);

// Step 1: Clean data (remove outliers)
double raw_factor[100];
double winsorized[100];
exprtk_vec_winsorize(raw_factor, 100, 0.05, winsorized, &arena);

// Step 2: Standardize (for combining)
double zscore[100];
exprtk_vec_zscore(winsorized, 100, zscore, &arena);

// Step 3: Rank (for portfolio construction)
double ranks[100];
exprtk_vec_rank(zscore, 100, ranks, &arena);

// Step 4: Select top/bottom deciles
for (int i = 0; i < 100; i++) {
    if (ranks[i] >= 90) {
        // Top 10% - long
    } else if (ranks[i] < 10) {
        // Bottom 10% - short
    }
}

turbo_pool_free(&arena);
```

---

### 2. Multi-Factor Model

```c
// Combine momentum, value, and quality
double momentum[100], value[100], quality[100];
// ... calculate factors ...

// Standardize each
double z_mom[100], z_val[100], z_qual[100];
exprtk_vec_zscore(momentum, 100, z_mom, &arena);
exprtk_vec_zscore(value, 100, z_val, &arena);
exprtk_vec_zscore(quality, 100, z_qual, &arena);

// Combine with custom weights
double combined[100];
for (int i = 0; i < 100; i++) {
    combined[i] = 0.4 * z_mom[i]   // 40% momentum
                + 0.3 * z_val[i]   // 30% value
                + 0.3 * z_qual[i]; // 30% quality
}

// Rank for portfolio construction
double ranks[100];
exprtk_vec_rank(combined, 100, ranks, &arena);
```

---

### 3. Risk-Adjusted Factors

```c
// Adjust factor for volatility
double raw_factor[100];
double returns[100];

// Calculate factor
exprtk_vec_zscore(raw_factor, 100, raw_factor, &arena);

// Calculate volatility
double rvar = exprtk_vec_rvar(returns, 100);
double vol = sqrt(rvar);

// Risk-adjust
double risk_adj_factor[100];
for (int i = 0; i < 100; i++) {
    risk_adj_factor[i] = raw_factor[i] / vol;
}
```

---

### 4. Performance Tips

```c
// ✅ GOOD: Reuse arena for batch processing
turbo_pool_t arena;
turbo_pool_init(&arena, 1024 * 1024);

for (int stock = 0; stock < 1000; stock++) {
    exprtk_vec_rank(data[stock], n, ranks[stock], &arena);
    exprtk_vec_zscore(data[stock], n, zscore[stock], &arena);
}

turbo_pool_free(&arena);

// ❌ BAD: Init/free in loop
for (int stock = 0; stock < 1000; stock++) {
    turbo_pool_t arena;
    turbo_pool_init(&arena, 1024);  // Slow!
    exprtk_vec_rank(data[stock], n, ranks[stock], &arena);
    turbo_pool_free(&arena);
}
```

---

## 🚨 Common Pitfalls

### ❌ Look-Ahead Bias

```c
// WRONG: Using future data
for (int t = 0; t < n; t++) {
    // This uses data from t+1, t+2, ... (future!)
    double factor = exprtk_vec_zscore(data, n, output, &arena);
}

// CORRECT: Use only past data
for (int t = window; t < n; t++) {
    // Only use data[0] to data[t]
    double factor = exprtk_vec_zscore(&data[t-window], window, output, &arena);
}
```

### ❌ Survivorship Bias

```c
// WRONG: Only using stocks that survived
double returns[100];  // Only current S&P 500 stocks

// CORRECT: Include delisted stocks
double returns[150];  // Current + delisted stocks
```

### ❌ Overfitting

```c
// WRONG: Testing 100 factors, using best one
for (int i = 0; i < 100; i++) {
    double sharpe = backtest_factor(factors[i]);
    if (sharpe > best_sharpe) best_factor = i;
}

// CORRECT: Use out-of-sample testing
double train_sharpe = backtest_factor(factor, train_data);
double test_sharpe = backtest_factor(factor, test_data);
if (test_sharpe > threshold) use_factor();
```

---

## 📖 Next Steps

- **💼 Portfolio construction?** Read [PORTFOLIO.md](PORTFOLIO.md) for optimization
- **📉 Backtesting?** See [FIN_MODULE.md](FIN_MODULE.md) for backtest functions
- **🔧 API reference?** Check [FIN_MODULE.md](FIN_MODULE.md) for all 46 functions

---

## 📚 Further Reading

- **Academic**: Fama-French factors, momentum, quality
- **Practitioner**: WorldQuant 101 Alphas, QuantConnect
- **Books**: "Quantitative Equity Portfolio Management" by Qian et al.

---

**Built with ❤️ for quantitative researchers**
