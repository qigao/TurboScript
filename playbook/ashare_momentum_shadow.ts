/**
 * @strategy ashare_momentum_shadow.ts
 *
 * Integrated strategy combining Refined Momentum and Shadow Factors.
 * Based on B-Category research in QuantsPlaybook.
 */

import("finance");

function run_logic() {
    // 1. Refined Momentum: 20-day return standardized by volatility
    var ret20 = roc(CLOSE, 20);
    var vol20 = stddev(RETURNS, 20);
    var risk_adj_mom = ret20 / (vol20 * sqrt(20));

    // 2. Candle Shadow Analysis
    // finance.shadow returns [upper_vec, lower_vec]
    var shadows = finance.shadow(OPEN, HIGH, LOW, CLOSE);
    var upper = shadows[0];
    var lower = shadows[1];

    // normalize shadows by price
    var upper_pct = upper / CLOSE;
    var lower_pct = lower / CLOSE;

    /* ── Execution Logic ────────────────────────────────────────────────── */

    if (is_flat()) {
        // Condition:
        // A. High Momentum (Top 20% of the market usually)
        // B. Short Upper Shadow (Low selling pressure at the top)
        // C. Strong Lower Shadow (Dip buying supporting the trend)
        if (last(risk_adj_mom) > 0.5 && last(upper_pct) < 0.005 && last(lower_pct) > 0.002) {
            buy(1.0, 0, 0);
        }
    } 
    
    else if (is_long()) {
        // Exit on Momentum collapse or extreme upper shadow (shooting star risk)
        if (last(risk_adj_mom) < -0.1 || last(upper_pct) > 0.02) {
            flat();
        }
    }
}

run_logic();
