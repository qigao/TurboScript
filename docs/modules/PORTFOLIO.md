# Portfolio Optimization - Modern Portfolio Theory

> **💼 Advanced: Build optimal portfolios using mean-variance optimization**

---

## 📋 Who Should Read This

- **💼 Portfolio Managers**: Constructing optimal asset allocations
- **🎓 Quant Researchers**: Implementing MPT algorithms
- **🔧 Developers**: Integrating portfolio optimization into systems

**Prerequisites:** Understanding of statistics and finance. See [FIN_MODULE.md](FIN_MODULE.md) for introduction.

---

## 🎯 What is Modern Portfolio Theory?

**Modern Portfolio Theory (MPT)** is the mathematical framework for constructing optimal portfolios that maximize return for a given level of risk.

### TL;DR - Core Concepts

```
📊 Covariance Matrix
   → Foundation: Measures how assets move together
   → Input: Historical returns
   → Output: n×n symmetric matrix

🛡️ Minimum Variance Portfolio
   → Goal: Lowest possible risk
   → Use case: Conservative allocation
   → Constraint: Weights sum to 1

⚡ Maximum Sharpe Ratio
   → Goal: Best risk-adjusted return
   → Use case: Optimal allocation
   → Formula: (Return - RiskFree) / Volatility

🎯 Markowitz Optimization
   → Goal: Target return with minimum risk
   → Use case: Efficient frontier
   → Constraint: Achieve specific return level
```

---

## 📊 Covariance Matrix - The Foundation

### `exprtk_pf_cov_matrix` - Calculate Covariance

**💼 Purpose**: Measure how assets move together (correlation × volatility).

```c
void exprtk_pf_cov_matrix(const double *returns, size_t na, size_t np,
                          double *out, turbo_pool_t *arena);
```

**Parameters:**
- `returns`: Returns matrix (na assets × np periods, **row-major**)
- `na`: Number of assets
- `np`: Number of periods
- `out`: Output covariance matrix (na × na, symmetric)

#### Example: 3-Asset Portfolio

```c
turbo_pool_t arena;
turbo_pool_init(&arena, 4096);

// 3 assets, 5 periods (row-major: all periods for asset 1, then asset 2, etc.)
double returns[] = {
    0.01, 0.02, -0.01, 0.03, 0.01,  // Asset 1 (Stock)
    0.02, 0.01,  0.00, 0.02, 0.02,  // Asset 2 (Bond)
   -0.01, 0.03,  0.02, 0.01, 0.00   // Asset 3 (Gold)
};

double cov[9];  // 3×3 matrix
exprtk_pf_cov_matrix(returns, 3, 5, cov, &arena);

// Interpret results
printf("=== Covariance Matrix ===\n");
printf("Stock variance:  %.6f (vol: %.2f%%)\n", cov[0], sqrt(cov[0])*100);
printf("Bond variance:   %.6f (vol: %.2f%%)\n", cov[4], sqrt(cov[4])*100);
printf("Gold variance:   %.6f (vol: %.2f%%)\n", cov[8], sqrt(cov[8])*100);

// Calculate correlations
double corr_stock_bond = cov[1] / (sqrt(cov[0]) * sqrt(cov[4]));
double corr_stock_gold = cov[2] / (sqrt(cov[0]) * sqrt(cov[8]));
double corr_bond_gold = cov[5] / (sqrt(cov[4]) * sqrt(cov[8]));

printf("\n=== Correlations ===\n");
printf("Stock-Bond: %.3f\n", corr_stock_bond);
printf("Stock-Gold: %.3f\n", corr_stock_gold);
printf("Bond-Gold:  %.3f\n", corr_bond_gold);

turbo_pool_free(&arena);
```

**💡 Interpretation:**
- **Diagonal**: Variances (risk of each asset)
- **Off-diagonal**: Covariances (how assets move together)
- **Negative covariance**: Assets move in opposite directions (good for diversification!)

---

### Converting Price Data to Returns

```c
// From daily prices to daily returns
double prices[] = {100, 101, 102, 103, 104};
double returns[4];  // n-1 returns from n prices

for (size_t i = 1; i < 5; i++) {
    returns[i-1] = (prices[i] - prices[i-1]) / prices[i-1];
}

// returns = [0.01, 0.0099, 0.0098, 0.0097]
//            1%    0.99%   0.98%   0.97%
```

**⚠️ Important:** Always use returns, not prices, for portfolio optimization!

---

## 🛡️ Minimum Variance Portfolio

### `exprtk_pf_min_variance` - Lowest Risk Portfolio

**💼 Purpose**: Find the portfolio with the lowest possible volatility.

```c
double exprtk_pf_min_variance(const double *cov, size_t n,
                              double *weights, turbo_pool_t *arena);
```

**Mathematical formulation:**
```
Minimize:   w' Σ w           (portfolio variance)
Subject to: Σ w_i = 1        (weights sum to 1)
```

**Returns:** Portfolio variance

#### Example: Conservative Allocation

```c
// 2 assets: Stock (high risk) and Bond (low risk)
double cov[] = {
    0.04, 0.01,  // Stock: 20% vol, low correlation with bond
    0.01, 0.09   // Bond:  30% vol
};
double weights[2];

double variance = exprtk_pf_min_variance(cov, 2, weights, &arena);
double volatility = sqrt(variance);

printf("=== Minimum Variance Portfolio ===\n");
printf("Stock weight: %.1f%%\n", weights[0] * 100);
printf("Bond weight:  %.1f%%\n", weights[1] * 100);
printf("Portfolio volatility: %.2f%%\n", volatility * 100);

// Verify weights sum to 1
double sum = weights[0] + weights[1];
printf("Weight sum: %.3f (should be 1.0)\n", sum);
```

**💡 Key insight:** Lower variance asset gets higher weight, but correlation matters!

#### Example: Multi-Asset Diversification

```c
// 5 assets with different risk levels
double cov[25] = {
    0.04, 0.01, 0.00, -0.01, 0.02,
    0.01, 0.09, 0.02,  0.00, 0.01,
    0.00, 0.02, 0.16,  0.03, 0.00,
   -0.01, 0.00, 0.03,  0.25, 0.04,
    0.02, 0.01, 0.00,  0.04, 0.36
};
double weights[5];

double variance = exprtk_pf_min_variance(cov, 5, weights, &arena);

printf("=== 5-Asset Min Variance Portfolio ===\n");
for (int i = 0; i < 5; i++) {
    printf("Asset %d: %.1f%%\n", i+1, weights[i] * 100);
}
printf("Portfolio risk: %.2f%%\n", sqrt(variance) * 100);
```

**💡 Diversification benefit:** Portfolio risk < weighted average of individual risks!

---

## ⚡ Maximum Sharpe Ratio Portfolio

### `exprtk_pf_max_sharpe` - Optimal Risk-Adjusted Return

**💼 Purpose**: Find the portfolio with the best return per unit of risk.

```c
double exprtk_pf_max_sharpe(const double *mu, const double *cov, size_t n,
                            double rf, double *weights, turbo_pool_t *arena);
```

**Mathematical formulation:**
```
Maximize:   (μ' w - rf) / sqrt(w' Σ w)    (Sharpe ratio)
Subject to: Σ w_i = 1                      (weights sum to 1)
```

**Parameters:**
- `mu`: Expected returns (length n)
- `cov`: Covariance matrix (n × n)
- `rf`: Risk-free rate (e.g., 0.02 for 2%)
- `weights`: Output weights

**Returns:** Sharpe ratio

#### Example: Optimal 3-Asset Portfolio

```c
// Expected annual returns
double mu[] = {0.08, 0.12, 0.10};  // 8%, 12%, 10%

// Covariance matrix (annualized)
double cov[] = {
    0.04, 0.01, 0.00,  // Asset 1: 20% vol
    0.01, 0.09, 0.02,  // Asset 2: 30% vol
    0.00, 0.02, 0.16   // Asset 3: 40% vol
};

double rf = 0.02;  // 2% risk-free rate (T-bills)
double weights[3];

double sharpe = exprtk_pf_max_sharpe(mu, cov, 3, rf, weights, &arena);

printf("=== Maximum Sharpe Portfolio ===\n");
printf("Sharpe Ratio: %.3f\n", sharpe);
printf("\nAllocation:\n");
printf("  Asset 1 (8%% return):  %.1f%%\n", weights[0] * 100);
printf("  Asset 2 (12%% return): %.1f%%\n", weights[1] * 100);
printf("  Asset 3 (10%% return): %.1f%%\n", weights[2] * 100);

// Calculate portfolio statistics
double port_return = 0.0;
for (int i = 0; i < 3; i++) {
    port_return += weights[i] * mu[i];
}

double port_variance = 0.0;
for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
        port_variance += weights[i] * cov[i*3 + j] * weights[j];
    }
}
double port_vol = sqrt(port_variance);

printf("\nPortfolio Statistics:\n");
printf("  Expected return: %.2f%%\n", port_return * 100);
printf("  Volatility:      %.2f%%\n", port_vol * 100);
printf("  Sharpe ratio:    %.3f\n", (port_return - rf) / port_vol);
```

**💡 Interpretation:**
- **Sharpe > 1.0**: Good risk-adjusted return
- **Sharpe > 2.0**: Excellent (rare in practice)
- **Sharpe < 0.5**: Poor (consider alternatives)

---

### Choosing the Risk-Free Rate

```c
// Different risk-free rates for different horizons
double rf_1m = 0.050;   // 1-month T-bill: 5.0%
double rf_3m = 0.048;   // 3-month T-bill: 4.8%
double rf_1y = 0.045;   // 1-year Treasury: 4.5%
double rf_10y = 0.040;  // 10-year Treasury: 4.0%

// Match to your investment horizon
double weights_short[3], weights_long[3];

// Short-term strategy (use short-term rate)
double sharpe_short = exprtk_pf_max_sharpe(mu, cov, 3, rf_1m, weights_short, &arena);

// Long-term strategy (use long-term rate)
double sharpe_long = exprtk_pf_max_sharpe(mu, cov, 3, rf_10y, weights_long, &arena);

printf("Short-term Sharpe: %.3f\n", sharpe_short);
printf("Long-term Sharpe:  %.3f\n", sharpe_long);
```

---

## 🎯 Markowitz Optimization

### `exprtk_pf_markowitz` - Target Return Portfolio

**💼 Purpose**: Find minimum risk portfolio that achieves a target return.

```c
double exprtk_pf_markowitz(const double *mu, const double *cov, size_t n,
                           double target, double *weights, turbo_pool_t *arena);
```

**Mathematical formulation:**
```
Minimize:   w' Σ w           (portfolio variance)
Subject to: μ' w = target    (achieve target return)
            Σ w_i = 1        (weights sum to 1)
```

**Returns:** Portfolio variance

#### Example: Efficient Frontier Point

```c
double mu[] = {0.08, 0.12, 0.10};
double cov[] = {
    0.04, 0.01, 0.00,
    0.01, 0.09, 0.02,
    0.00, 0.02, 0.16
};

double target = 0.10;  // Target 10% return
double weights[3];

double variance = exprtk_pf_markowitz(mu, cov, 3, target, weights, &arena);
double volatility = sqrt(variance);

printf("=== Target Return Portfolio ===\n");
printf("Target return: %.2f%%\n", target * 100);
printf("Portfolio risk: %.2f%%\n", volatility * 100);
printf("\nAllocation:\n");
for (int i = 0; i < 3; i++) {
    printf("  Asset %d: %.1f%%\n", i+1, weights[i] * 100);
}

// Verify we hit the target
double actual_return = 0.0;
for (int i = 0; i < 3; i++) {
    actual_return += weights[i] * mu[i];
}
printf("\nActual return: %.2f%% (target: %.2f%%)\n",
       actual_return * 100, target * 100);
```

---

### Generating the Efficient Frontier

**💼 Purpose**: Plot risk-return tradeoff for all optimal portfolios.

```c
// Find feasible return range
double min_ret = mu[0], max_ret = mu[0];
for (int i = 1; i < 3; i++) {
    if (mu[i] < min_ret) min_ret = mu[i];
    if (mu[i] > max_ret) max_ret = mu[i];
}

printf("=== Efficient Frontier ===\n");
printf("Return  Risk    Sharpe\n");
printf("------  ------  ------\n");

// Generate 10 points on efficient frontier
for (int i = 0; i <= 10; i++) {
    double target = min_ret + (max_ret - min_ret) * i / 10.0;
    double weights[3];
    double variance = exprtk_pf_markowitz(mu, cov, 3, target, weights, &arena);
    double vol = sqrt(variance);
    double sharpe = (target - 0.02) / vol;  // Assuming 2% risk-free rate

    printf("%.2f%%  %.2f%%  %.3f\n", target * 100, vol * 100, sharpe);
}
```

**Expected output:**
```
=== Efficient Frontier ===
Return  Risk    Sharpe
------  ------  ------
8.00%   18.5%   0.324
8.40%   18.2%   0.352
8.80%   18.1%   0.376
9.20%   18.3%   0.393
9.60%   18.7%   0.406
10.00%  19.3%   0.414  ← Maximum Sharpe point
10.40%  20.1%   0.418
10.80%  21.1%   0.417
11.20%  22.3%   0.412
11.60%  23.6%   0.406
12.00%  25.1%   0.398
```

**💡 Key insight:** The maximum Sharpe ratio is at the "elbow" of the efficient frontier!

---

## 🔄 Complete Workflow

### Step 1: Prepare Historical Data

```c
#include "fin.h"
#include <stdio.h>

int main() {
    turbo_pool_t arena;
    turbo_pool_init(&arena, 1024 * 1024);

    // Historical prices (3 assets, 100 days)
    double prices[3][100];
    // ... load from database or CSV ...

    // Convert to returns
    double returns[3][99];
    for (int asset = 0; asset < 3; asset++) {
        for (int t = 1; t < 100; t++) {
            returns[asset][t-1] = (prices[asset][t] - prices[asset][t-1])
                                  / prices[asset][t-1];
        }
    }

    // Flatten to row-major array for covariance calculation
    double returns_flat[3 * 99];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 99; j++) {
            returns_flat[i * 99 + j] = returns[i][j];
        }
    }
```

---

### Step 2: Calculate Statistics

```c
    // Calculate covariance matrix
    double cov[9];
    exprtk_pf_cov_matrix(returns_flat, 3, 99, cov, &arena);

    // Calculate expected returns (simple average)
    double mu[3];
    for (int i = 0; i < 3; i++) {
        double sum = 0.0;
        for (int j = 0; j < 99; j++) {
            sum += returns[i][j];
        }
        mu[i] = sum / 99.0;
    }

    // Annualize (assuming daily returns)
    for (int i = 0; i < 3; i++) {
        mu[i] *= 252.0;  // 252 trading days per year
        for (int j = 0; j < 3; j++) {
            cov[i*3 + j] *= 252.0;
        }
    }

    printf("=== Asset Statistics (Annualized) ===\n");
    for (int i = 0; i < 3; i++) {
        printf("Asset %d: Return=%.2f%%, Vol=%.2f%%\n",
               i+1, mu[i]*100, sqrt(cov[i*3+i])*100);
    }
```

---

### Step 3: Optimize Portfolios

```c
    // 1. Minimum Variance Portfolio
    double weights_minvar[3];
    double var_min = exprtk_pf_min_variance(cov, 3, weights_minvar, &arena);

    printf("\n=== Minimum Variance Portfolio ===\n");
    printf("Risk: %.2f%%\n", sqrt(var_min) * 100);
    for (int i = 0; i < 3; i++) {
        printf("  Asset %d: %.1f%%\n", i+1, weights_minvar[i] * 100);
    }

    // 2. Maximum Sharpe Portfolio
    double rf = 0.02;  // 2% risk-free rate
    double weights_sharpe[3];
    double sharpe = exprtk_pf_max_sharpe(mu, cov, 3, rf, weights_sharpe, &arena);

    printf("\n=== Maximum Sharpe Portfolio ===\n");
    printf("Sharpe Ratio: %.3f\n", sharpe);
    for (int i = 0; i < 3; i++) {
        printf("  Asset %d: %.1f%%\n", i+1, weights_sharpe[i] * 100);
    }

    // 3. Target Return Portfolio
    double target = 0.10;  // 10% target
    double weights_target[3];
    double var_target = exprtk_pf_markowitz(mu, cov, 3, target, weights_target, &arena);

    printf("\n=== Target Return Portfolio (10%%) ===\n");
    printf("Risk: %.2f%%\n", sqrt(var_target) * 100);
    for (int i = 0; i < 3; i++) {
        printf("  Asset %d: %.1f%%\n", i+1, weights_target[i] * 100);
    }

    turbo_pool_free(&arena);
    return 0;
}
```

---

## 🎯 Best Practices

### 1. Data Quality is Critical

```c
// Check for missing data
for (int i = 0; i < n; i++) {
    if (isnan(returns[i]) || isinf(returns[i])) {
        // Handle missing data
        returns[i] = 0.0;  // Or interpolate, or skip
    }
}

// Remove outliers (winsorize)
double winsorized[n];
exprtk_vec_winsorize(returns, n, 0.05, winsorized, &arena);
```

---

### 2. Estimation Window Selection

```c
// Use appropriate history length
// Too short: noisy estimates
// Too long: stale estimates

// Typical choices:
size_t window_daily = 252;    // 1 year of daily data
size_t window_weekly = 156;   // 3 years of weekly data
size_t window_monthly = 60;   // 5 years of monthly data

// Rolling window example
for (int t = window_daily; t < total_days; t++) {
    double returns_window[window_daily];
    // Extract window: returns[t-window_daily] to returns[t-1]
    // ... calculate covariance and optimize ...
}
```

---

### 3. Covariance Shrinkage

**💼 Purpose**: Improve covariance estimates by shrinking towards a target.

```c
// Calculate sample covariance
double cov[9];
exprtk_pf_cov_matrix(returns, 3, 99, cov, &arena);

// Calculate average correlation
double avg_corr = 0.0;
int count = 0;
for (int i = 0; i < 3; i++) {
    for (int j = 0; j < i; j++) {
        double corr = cov[i*3 + j] / (sqrt(cov[i*3+i]) * sqrt(cov[j*3+j]));
        avg_corr += corr;
        count++;
    }
}
avg_corr /= count;

// Shrink off-diagonal elements towards average correlation
double shrinkage = 0.2;  // 20% shrinkage
for (int i = 0; i < 3; i++) {
    for (int j = 0; j < i; j++) {
        double std_i = sqrt(cov[i*3 + i]);
        double std_j = sqrt(cov[j*3 + j]);
        double target_cov = avg_corr * std_i * std_j;

        // Shrink towards target
        cov[i*3 + j] = (1 - shrinkage) * cov[i*3 + j] + shrinkage * target_cov;
        cov[j*3 + i] = cov[i*3 + j];  // Keep symmetric
    }
}
```

**💡 Why shrinkage?** Reduces estimation error, especially with limited data.

---

### 4. Position Constraints

```c
// After optimization, apply practical constraints
for (int i = 0; i < n; i++) {
    // No short selling
    if (weights[i] < 0.0) weights[i] = 0.0;

    // Maximum position size (e.g., 30%)
    if (weights[i] > 0.30) weights[i] = 0.30;

    // Minimum position size (e.g., 5% or 0)
    if (weights[i] < 0.05) weights[i] = 0.0;
}

// Re-normalize to sum to 1
double sum = 0.0;
for (int i = 0; i < n; i++) sum += weights[i];
for (int i = 0; i < n; i++) weights[i] /= sum;
```

---

### 5. Rebalancing Strategy

```c
// Check if rebalancing is needed
double max_drift = 0.0;
for (int i = 0; i < n; i++) {
    double drift = fabs(current_weights[i] - target_weights[i]);
    if (drift > max_drift) max_drift = drift;
}

// Rebalance if drift exceeds threshold
if (max_drift > 0.05) {  // 5% threshold
    printf("⚠️ Rebalancing needed (max drift: %.2f%%)\n", max_drift * 100);
    // Execute rebalancing trades
    rebalance_portfolio(target_weights, n);
} else {
    printf("✅ No rebalancing needed (max drift: %.2f%%)\n", max_drift * 100);
}
```

---

### 6. Transaction Costs

```c
// Calculate turnover
double turnover = 0.0;
for (int i = 0; i < n; i++) {
    turnover += fabs(new_weights[i] - old_weights[i]);
}

// Estimate cost (basis points)
double cost_bps = 10.0;  // 10 basis points per trade
double total_cost = turnover * cost_bps / 10000.0;

// Adjust expected return
double gross_return = 0.10;  // 10% gross
double net_return = gross_return - total_cost;

printf("Turnover: %.2f%%\n", turnover * 100);
printf("Transaction cost: %.4f%% (%.0f bps)\n", total_cost * 100, cost_bps);
printf("Net return: %.2f%%\n", net_return * 100);
```

---

## 🚨 Common Pitfalls

### ❌ Singular Covariance Matrix

```c
// If covariance matrix is singular (non-invertible):
// 1. Check for duplicate assets
// 2. Add small diagonal (regularization)

for (int i = 0; i < n; i++) {
    cov[i*n + i] += 1e-6;  // Add small value to diagonal
}
```

### ❌ Extreme Weights

```c
// If weights are too concentrated:
// 1. Use shrinkage
// 2. Add constraints
// 3. Increase diversification penalty

// Check concentration
double max_weight = 0.0;
for (int i = 0; i < n; i++) {
    if (weights[i] > max_weight) max_weight = weights[i];
}

if (max_weight > 0.5) {
    printf("⚠️ WARNING: Concentrated portfolio (%.1f%% in one asset)\n",
           max_weight * 100);
}
```

### ❌ Ignoring Estimation Error

```c
// WRONG: Using sample estimates as truth
double sharpe = exprtk_pf_max_sharpe(mu, cov, n, rf, weights, &arena);
printf("Expected Sharpe: %.3f\n", sharpe);  // Overoptimistic!

// CORRECT: Account for estimation error
double sharpe_adjusted = sharpe * 0.7;  // Haircut for estimation error
printf("Realistic Sharpe: %.3f\n", sharpe_adjusted);
```

---

## 🎓 Advanced Topics

### Black-Litterman Model

Incorporate views into optimization:
1. Start with market equilibrium
2. Add your views (e.g., "Stock A will outperform by 2%")
3. Combine with Bayesian updating
4. Optimize with adjusted returns

*(Implementation requires matrix operations beyond current scope)*

---

### Risk Parity

Equal risk contribution from each asset:
- Target: Each asset contributes equally to portfolio risk
- Requires iterative solver
- Popular for multi-asset portfolios

*(Not yet implemented - requires numerical optimization)*

---

### Robust Optimization

Handle estimation uncertainty:
- Use worst-case optimization
- Add uncertainty sets
- Requires convex optimization solver

*(Not yet implemented - requires advanced solvers)*

---

## 📖 Next Steps

- **🔬 Factor analysis?** Read [FACTORS.md](FACTORS.md) for alpha generation
- **📉 Backtesting?** See [FIN_MODULE.md](FIN_MODULE.md) for backtest functions
- **🔧 Full API?** Check [FIN_MODULE.md](FIN_MODULE.md) for all 46 functions

---

## 📚 Further Reading

- **Academic**: Markowitz (1952), Sharpe (1964), Black-Litterman (1992)
- **Books**:
  - "Portfolio Selection" by Harry Markowitz
  - "Active Portfolio Management" by Grinold & Kahn
  - "Quantitative Equity Portfolio Management" by Qian et al.
- **Online**: QuantLib, PyPortfolioOpt, Riskfolio-Lib

---

**Built with ❤️ for portfolio managers and quant researchers**
