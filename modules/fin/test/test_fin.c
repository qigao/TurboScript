/**
 * @file test_fin.c
 * @brief Unit tests for finance module core functions.
 */

#include "fin.h"
#include "tinytest.h"
#include <math.h>
#include <string.h>

#define EPSILON 0.0001

/* =========================================================================
 * Performance Metrics Tests
 * ========================================================================= */

suite("fin test") {
  group("Performance Metrics") {
    it("should calculate Sharpe ratio for positive returns") {
      double returns[] = {0.01, 0.02, -0.01, 0.03, 0.01, 0.02, -0.005};
      size_t n = 7;
      double rf = 0.0;              // Risk-free rate
      double annual_factor = 252.0; // Daily returns

      double sharpe = exprtk_sharpe(returns, n, rf, annual_factor);

      check(sharpe > 0); // Positive returns should give positive Sharpe
    }

    it("should calculate Sharpe ratio with risk-free rate") {
      double returns[] = {0.01, 0.02, 0.015, 0.03, 0.01};
      size_t n = 5;
      double rf = 0.005;            // 0.5% risk-free rate
      double annual_factor = 252.0;

      double sharpe = exprtk_sharpe(returns, n, rf, annual_factor);

      check(sharpe != 0);
    }

    it("should calculate Sortino ratio") {
      double returns[] = {0.01, 0.02, -0.01, 0.03, 0.01, 0.02, -0.005};
      size_t n = 7;
      double rf = 0.0;
      double annual_factor = 252.0;

      double sortino = exprtk_sortino(returns, n, rf, annual_factor);

      check(sortino != 0);
    }

    it("should calculate Sortino ratio higher than Sharpe for positive skew") {
      double returns[] = {0.01, 0.02, -0.005, 0.03, 0.01, 0.02, -0.002};
      size_t n = 7;
      double rf = 0.0;
      double annual_factor = 252.0;

      double sharpe = exprtk_sharpe(returns, n, rf, annual_factor);
      double sortino = exprtk_sortino(returns, n, rf, annual_factor);

      // Sortino should be >= Sharpe (only penalizes downside)
      check(sortino >= sharpe - EPSILON);
    }

    it("should calculate Calmar ratio") {
      double equity[] = {10000, 10500, 11000, 10800, 11500, 12000};
      size_t n = 6;
      double annual_factor = 252.0;

      double calmar = exprtk_calmar(equity, n, annual_factor);

      check(calmar > 0); // Positive equity growth should give positive Calmar
    }

    it("should annualize Calmar with CAGR instead of linear return") {
      double equity[] = {100.0, 50.0, 200.0};
      size_t n = 3;
      double annual_factor = 2.0;

      double calmar = exprtk_calmar(equity, n, annual_factor);

      check_float_eq(calmar, 2.0, EPSILON);
    }

    it("should calculate profit factor for winning trades") {
      double trades[] = {100, -50, 150, -30, 80, -40};
      size_t n = 6;

      double pf = exprtk_profit_factor(trades, n);

      check(pf > 1.0); // More profit than loss
    }

    it("should calculate profit factor for losing trades") {
      double trades[] = {50, -100, 30, -150, 40, -80};
      size_t n = 6;

      double pf = exprtk_profit_factor(trades, n);

      check(pf < 1.0); // More loss than profit
    }

    it("should calculate expectancy for positive system") {
      double trades[] = {100, -50, 150, -30, 80, -40};
      size_t n = 6;

      double exp = exprtk_expectancy(trades, n);

      check(exp > 0); // Net positive trades
    }

    it("should calculate expectancy for negative system") {
      double trades[] = {50, -100, 30, -150, 40, -80};
      size_t n = 6;

      double exp = exprtk_expectancy(trades, n);

      check(exp < 0); // Net negative trades
    }

    it("should calculate payoff ratio") {
      double trades[] = {100, -50, 150, -30, 80, -40};
      size_t n = 6;

      double payoff = exprtk_payoff_ratio(trades, n);

      check(payoff > 0); // Payoff ratio should be positive
      check(payoff > 1.0); // Average win > average loss
    }

    it("should calculate max drawdown duration") {
      double equity[] = {10000, 11000, 10500, 9500, 9000, 9500, 10000, 11000};
      size_t n = 8;

      size_t duration = exprtk_max_dd_duration(equity, n);

      check(duration > 0); // Should have some drawdown period
      check(duration < n); // Duration can't exceed total periods
    }

    it("should calculate Ulcer Index") {
      double equity[] = {10000, 11000, 10500, 9500, 9000, 9500, 10000};
      size_t n = 7;

      double ulcer = exprtk_ulcer_index(equity, n);

      check(ulcer > 0); // Ulcer index should be positive
    }

    it("should calculate Information Ratio") {
      double returns[] = {0.01, 0.02, -0.01, 0.03, 0.01};
      double benchmark[] = {0.008, 0.015, -0.005, 0.025, 0.009};
      size_t n = 5;

      double ir = exprtk_information_ratio(returns, benchmark, n);

      check(ir != 0); // Should return a value
    }

    it("should calculate Treynor ratio") {
      double returns[] = {0.01, 0.02, -0.01, 0.03, 0.01};
      double benchmark[] = {0.008, 0.015, -0.005, 0.025, 0.009};
      size_t n = 5;
      double rf = 0.0;

      double treynor = exprtk_treynor(returns, benchmark, n, rf);

      check(treynor != 0); // Should return a value
    }
  }

  group("Edge Cases") {
    it("should handle single data point") {
      double returns[] = {0.01};
      size_t n = 1;
      double rf = 0.0;
      double annual_factor = 252.0;

      double sharpe = exprtk_sharpe(returns, n, rf, annual_factor);

      // Should handle gracefully (might return 0 or inf)
      check(sharpe == sharpe); // Check not NaN
    }

    it("should handle zero variance returns") {
      double returns[] = {0.01, 0.01, 0.01, 0.01};
      size_t n = 4;
      double rf = 0.0;
      double annual_factor = 252.0;

      double sharpe = exprtk_sharpe(returns, n, rf, annual_factor);

      // Zero variance should return inf or handle gracefully
      check(sharpe == sharpe); // Check not NaN
    }

    it("should handle all zero trades") {
      double trades[] = {0, 0, 0, 0};
      size_t n = 4;

      double pf = exprtk_profit_factor(trades, n);
      double exp = exprtk_expectancy(trades, n);

      check(pf == pf);  // Check not NaN
      check(exp == 0);  // Expectancy should be 0
    }

    it("should handle flat equity curve") {
      double equity[] = {10000, 10000, 10000, 10000};
      size_t n = 4;

      size_t duration = exprtk_max_dd_duration(equity, n);
      double ulcer = exprtk_ulcer_index(equity, n);

      check(duration == 0);  // No drawdown
      check(ulcer == 0);     // No drawdown pain
    }
  }
}

 
