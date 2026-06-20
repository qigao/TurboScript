/**
 * @file test_universe.c
 * @brief Unit tests for universe management and SIMD-accelerated
 *        cross-sectional operations.
 */

#include "universe.h"
#include "provider.h"
#include "market_rules.h"
#include "../src/exprtk_universe.h"
#include "tinytest.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <float.h>

#define EPSILON 0.0001

/* =========================================================================
 * Helper: build a small test universe
 * ========================================================================= */

static universe_t *make_test_universe(mem_pool_t *arena) {
    universe_t *u = universe_create(arena);
    if (!u) return NULL;

    /* 5 assets: AAPL(0), MSFT(1), GOOG(2), AMZN(3), TSLA(4) */
    universe_asset_t assets[] = {
        { .id = 0, .ticker = "AAPL",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 1, .ticker = "MSFT",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 2, .ticker = "GOOG",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 150, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 3, .ticker = "AMZN",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = 300, .lot_size = 1, .tick_size = 0.01 },
        { .id = 4, .ticker = "TSLA",  .exchange = "NYSE", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 200, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
    };

    for (size_t i = 0; i < 5; i++) {
        universe_add_asset(u, &assets[i]);
    }

    universe_finalize(u);
    return u;
}

/* =========================================================================
 * Tests
 * ========================================================================= */

suite("Universe Management") {

  /* -------------------------------------------------------------------
   * Lifecycle
   * ------------------------------------------------------------------- */
  group("Lifecycle") {
    it("should create and destroy a universe") {
      mem_pool_t arena;
      mem_init(&arena, 8192);

      universe_t *u = universe_create(&arena);
      check_not_null(u);
      check_int_eq((int)u->num_assets, 0);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should add assets and finalize") {
      mem_pool_t arena;
      mem_init(&arena, 8192);

      universe_t *u = make_test_universe(&arena);
      check_not_null(u);
      check_int_eq((int)u->num_assets, 5);
      check_not_null(u->active_mask);
      check_not_null(u->cum_adj_cache);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should fail when explicit adjustments source is missing") {
      mem_pool_t arena;
      mem_init(&arena, 16384);
      const char *assets_path = "test_provider_assets.csv";
      FILE *f = fopen(assets_path, "w");
      check_not_null(f);
      fprintf(f, "id,ticker,exchange,type,start_date,end_date,lot_size,tick_size\n");
      fprintf(f, "1,AAPL,NYSE,EQUITY,2024-01-01,,1,0.01\n");
      fclose(f);

      universe_t *u = universe_create(&arena);
      check_not_null(u);

      provider_t *p = provider_csv_create(&MARKET_US_EQUITY, ".", &arena);
      check_not_null(p);

      check_int_eq(provider_load_universe(p, u, assets_path, "missing_adjustments.csv", &arena), -1);

      universe_free(u);
      remove(assets_path);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Active Mask & Advance
   * ------------------------------------------------------------------- */
  group("Active Mask") {
    it("should activate assets based on start_date") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      /* date=120: AAPL(100), MSFT(100), AMZN(100) active; GOOG(150), TSLA(200) not yet */
      universe_advance(u, 120);
      check_int_eq((int)universe_active_count(u), 3);
      check(universe_is_active(u, 0));   /* AAPL */
      check(universe_is_active(u, 1));   /* MSFT */
      check(!universe_is_active(u, 2));  /* GOOG not yet */
      check(universe_is_active(u, 3));   /* AMZN */
      check(!universe_is_active(u, 4));  /* TSLA not yet */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should activate later assets as date progresses") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      /* date=200: all 5 assets are active (AMZN end_date=300 > 200) */
      universe_advance(u, 200);
      check_int_eq((int)universe_active_count(u), 5);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should deactivate delisted assets") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 250);
      check(universe_is_active(u, 3)); /* AMZN still active (end_date=300 > 250) */

      universe_advance(u, 300);
      check(!universe_is_active(u, 3)); /* AMZN delisted (end_date=300 <= 300) */
      check_int_eq((int)universe_active_count(u), 4); /* 5 minus AMZN */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should report delisted_today on exact delist date") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 250);
      universe_advance(u, 300);

      uint32_t delisted[8];
      size_t n = universe_delisted_today(u, delisted, 8);
      check_int_eq((int)n, 1);
      check_int_eq((int)delisted[0], 3); /* AMZN id=3 */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should fill active_ids correctly") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120);
      uint32_t ids[8];
      size_t n = universe_active_ids(u, ids, 8);
      check_int_eq((int)n, 3);
      check_int_eq((int)ids[0], 0); /* AAPL */
      check_int_eq((int)ids[1], 1); /* MSFT */
      check_int_eq((int)ids[2], 3); /* AMZN */

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Adjustments
   * ------------------------------------------------------------------- */
  group("Price Adjustments") {
    it("should apply split adjustment factor") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "AAPL", .exchange = "NYSE",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_add_asset(u, &asset);

      /* 4:1 split on day 200 */
      universe_adj_t adj = {
        .asset_id = 0, .date = 200,
        .type = UNIVERSE_ADJ_SPLIT, .factor = 4.0
      };
      universe_add_adjustment(u, &adj);

      universe_finalize(u);

      /* Before the split date: factor should be 1/4 = 0.25 */
      universe_advance(u, 250);
      double factor = universe_adj_factor(u, 0);
      check_float_eq(factor, 0.25, EPSILON);

      /* Adjusted price: 400 * 0.25 = 100 */
      double adjusted = universe_adjust_price(u, 0, 400.0);
      check_float_eq(adjusted, 100.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should handle multiple splits correctly") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "TEST", .exchange = "X",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_add_asset(u, &asset);

      /* 2:1 split on day 200, then 3:1 split on day 300 */
      universe_adj_t adj1 = { .asset_id = 0, .date = 200, .type = UNIVERSE_ADJ_SPLIT, .factor = 2.0 };
      universe_adj_t adj2 = { .asset_id = 0, .date = 300, .type = UNIVERSE_ADJ_SPLIT, .factor = 3.0 };
      universe_add_adjustment(u, &adj1);
      universe_add_adjustment(u, &adj2);

      universe_finalize(u);

      /* After both splits: cum_adj = 1/(2*3) = 1/6 */
      universe_advance(u, 350);
      double factor = universe_adj_factor(u, 0);
      check_float_eq(factor, 1.0 / 6.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should apply dividend adjustment factor using previous close") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "DIV", .exchange = "X",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_adj_t adj = {
        .asset_id = 0, .date = 200, .type = UNIVERSE_ADJ_DIVIDEND, .factor = 10.0
      };

      check_int_eq(universe_add_asset(u, &asset), 0);
      check_int_eq(universe_add_adjustment(u, &adj), 0);
      universe_finalize(u);

      universe_advance(u, 199);
      check_float_eq(universe_adj_factor(u, 0), 1.0, EPSILON);

      universe_advance(u, 200);
      universe_apply_runtime_adjustments(u, 0, 200, 100.0);
      check_float_eq(universe_adj_factor(u, 0), 0.9, EPSILON);
      check_float_eq(universe_adjust_price(u, 0, 100.0), 90.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should reset runtime dividend state when rewinding dates") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "DIV", .exchange = "X",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_adj_t adj = {
        .asset_id = 0, .date = 200, .type = UNIVERSE_ADJ_DIVIDEND, .factor = 10.0
      };

      check_int_eq(universe_add_asset(u, &asset), 0);
      check_int_eq(universe_add_adjustment(u, &adj), 0);
      universe_finalize(u);

      universe_advance(u, 200);
      universe_apply_runtime_adjustments(u, 0, 200, 100.0);
      check_float_eq(universe_adj_factor(u, 0), 0.9, EPSILON);

      universe_advance(u, 150);
      check_float_eq(universe_adj_factor(u, 0), 1.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should bulk-adjust prices with SIMD") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t assets[] = {
        { .id = 0, .ticker = "A", .exchange = "X", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
        { .id = 1, .ticker = "B", .exchange = "X", .asset_type = UNIVERSE_ASSET_EQUITY,
          .start_date = 100, .end_date = UNIVERSE_DATE_NONE, .lot_size = 1, .tick_size = 0.01 },
      };
      universe_add_asset(u, &assets[0]);
      universe_add_asset(u, &assets[1]);

      /* 2:1 split for asset 0 */
      universe_adj_t adj = { .asset_id = 0, .date = 200, .type = UNIVERSE_ADJ_SPLIT, .factor = 2.0 };
      universe_add_adjustment(u, &adj);

      universe_finalize(u);
      universe_advance(u, 250);

      double raw[] = { 200.0, 100.0 };
      double adjusted[2];
      universe_adjust_prices(u, raw, adjusted, 2);

      /* Asset 0: 200 * 0.5 = 100, Asset 1: 100 * 1.0 = 100 */
      check_float_eq(adjusted[0], 100.0, EPSILON);
      check_float_eq(adjusted[1], 100.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Cross-Sectional: Rank
   * ------------------------------------------------------------------- */
  group("Cross-Sectional Rank") {
    it("should rank active assets correctly") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200); /* All 5 active */

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_rank(u, values, 5, out);

      /* Sorted order: MSFT(10)=0, TSLA(20)=1, GOOG(30)=2, AMZN(40)=3, AAPL(50)=4 */
      /* Rank = position / (n-1) = position / 4 */
      check_float_eq(out[0], 4.0 / 4.0, EPSILON); /* AAPL=50 → rank 4/4 = 1.0 */
      check_float_eq(out[1], 0.0 / 4.0, EPSILON); /* MSFT=10 → rank 0/4 = 0.0 */
      check_float_eq(out[2], 2.0 / 4.0, EPSILON); /* GOOG=30 → rank 2/4 = 0.5 */
      check_float_eq(out[3], 3.0 / 4.0, EPSILON); /* AMZN=40 → rank 3/4 = 0.75 */
      check_float_eq(out[4], 1.0 / 4.0, EPSILON); /* TSLA=20 → rank 1/4 = 0.25 */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should set inactive asset ranks to zero") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* Only AAPL, MSFT, AMZN active */

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double out[5];
      universe_rank(u, values, 5, out);

      /* GOOG(idx=2) and TSLA(idx=4) are inactive → 0.0 */
      check_float_eq(out[2], 0.0, EPSILON);
      check_float_eq(out[4], 0.0, EPSILON);

      /* Active: MSFT(10)=rank0, AMZN(20)=rank1, AAPL(30)=rank2 */
      check_float_eq(out[1], 0.0 / 2.0, EPSILON); /* MSFT → 0.0 */
      check_float_eq(out[3], 1.0 / 2.0, EPSILON); /* AMZN → 0.5 */
      check_float_eq(out[0], 2.0 / 2.0, EPSILON); /* AAPL → 1.0 */

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Cross-Sectional: Top-N
   * ------------------------------------------------------------------- */
  group("Cross-Sectional Top-N") {
    it("should return top-k asset IDs by value") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      uint32_t top_ids[3];
      size_t n = universe_top_n(u, values, 5, 3, top_ids);

      check_int_eq((int)n, 3);
      check_int_eq((int)top_ids[0], 0); /* AAPL=50 (highest) */
      check_int_eq((int)top_ids[1], 3); /* AMZN=40 */
      check_int_eq((int)top_ids[2], 2); /* GOOG=30 */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should clamp k to active count") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* 3 active */

      double values[] = { 30.0, 10.0, 0.0, 20.0, 0.0 };
      uint32_t top_ids[10];
      size_t n = universe_top_n(u, values, 5, 10, top_ids);

      check_int_eq((int)n, 3); /* only 3 active */

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Cross-Sectional: Filter
   * ------------------------------------------------------------------- */
  group("Cross-Sectional Filter") {
    it("should filter assets above threshold") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      uint8_t mask[5];
      size_t count = universe_filter_gt(u, values, 5, 25.0, mask);

      check_int_eq((int)count, 3); /* AAPL(50), GOOG(30), AMZN(40) > 25 */
      check_int_eq(mask[0], 1); /* AAPL */
      check_int_eq(mask[1], 0); /* MSFT=10 */
      check_int_eq(mask[2], 1); /* GOOG=30 */
      check_int_eq(mask[3], 1); /* AMZN=40 */
      check_int_eq(mask[4], 0); /* TSLA=20 */

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Z-Score
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Z-Score") {
    it("should z-score normalize active assets") {
      mem_pool_t arena;
      mem_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200); /* All 5 active */

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_zscore(u, values, 5, out);

      /* Mean of actives = (50+10+30+40+20)/5 = 30
       * Var = ((20^2 + 20^2 + 0 + 10^2 + 10^2) / 4) = 250  (sample var)
       * SD = sqrt(250) ≈ 15.811
       * z(AAPL) = (50-30)/15.811 ≈ 1.265
       * z(MSFT) = (10-30)/15.811 ≈ -1.265
       * z(GOOG) = (30-30)/15.811 ≈ 0.0 */
      check_float_eq(out[2], 0.0, 0.01);           /* GOOG is at the mean */
      check(out[0] > 0);                            /* AAPL above mean */
      check(out[1] < 0);                            /* MSFT below mean */
      check_float_eq(out[0], -out[1], 0.01);        /* Symmetric around mean */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should set inactive slots to zero in zscore") {
      mem_pool_t arena;
      mem_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* AAPL, MSFT, AMZN active */

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double out[5];
      universe_zscore(u, values, 5, out);

      check_float_eq(out[2], 0.0, EPSILON); /* GOOG inactive */
      check_float_eq(out[4], 0.0, EPSILON); /* TSLA inactive */

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Demean
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Demean") {
    it("should subtract cross-sectional mean from active assets") {
      mem_pool_t arena;
      mem_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_demean(u, values, 5, out);

      /* Mean = 30.0 */
      check_float_eq(out[0],  20.0, EPSILON); /* 50 - 30 */
      check_float_eq(out[1], -20.0, EPSILON); /* 10 - 30 */
      check_float_eq(out[2],   0.0, EPSILON); /* 30 - 30 */
      check_float_eq(out[3],  10.0, EPSILON); /* 40 - 30 */
      check_float_eq(out[4], -10.0, EPSILON); /* 20 - 30 */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should set inactive slots to zero in demean") {
      mem_pool_t arena;
      mem_init(&arena, 16384);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120);

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double out[5];
      universe_demean(u, values, 5, out);

      check_float_eq(out[2], 0.0, EPSILON);
      check_float_eq(out[4], 0.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Clip
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Clip") {
    it("should clamp values to range") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double out[5];
      universe_clip(u, values, 5, 15.0, 45.0, out);

      check_float_eq(out[0], 45.0, EPSILON); /* 50 clamped to 45 */
      check_float_eq(out[1], 15.0, EPSILON); /* 10 clamped to 15 */
      check_float_eq(out[2], 30.0, EPSILON); /* 30 unchanged */
      check_float_eq(out[3], 40.0, EPSILON); /* 40 unchanged */
      check_float_eq(out[4], 20.0, EPSILON); /* 20 unchanged */

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * SIMD Cross-Sectional: Cross Sum
   * ------------------------------------------------------------------- */
  group("SIMD Cross-Sectional Sum") {
    it("should sum only active asset values") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 120); /* AAPL, MSFT, AMZN active */

      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      double sum = universe_cross_sum(u, values, 5);

      /* Only active: 30 + 10 + 20 = 60 (ignores GOOG and TSLA) */
      check_float_eq(sum, 60.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should sum all when all active") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      universe_advance(u, 200);

      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      double sum = universe_cross_sum(u, values, 5);

      check_float_eq(sum, 150.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Lookup Helpers
   * ------------------------------------------------------------------- */
  group("Lookup") {
    it("should find assets by ID") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      check_int_eq((int)universe_find_asset(u, 0), 0);
      check_int_eq((int)universe_find_asset(u, 3), 3);
      check(universe_find_asset(u, 99) == SIZE_MAX);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should find assets by ticker") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);

      const universe_asset_t *a = universe_find_by_ticker(u, "GOOG");
      check_not_null(a);
      check_int_eq((int)a->id, 2);

      check(universe_find_by_ticker(u, "NOPE") == NULL);

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  /* -------------------------------------------------------------------
   * Edge Cases
   * ------------------------------------------------------------------- */
  group("Edge Cases") {
    it("should handle empty universe") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);
      universe_finalize(u);

      check_int_eq((int)universe_active_count(u), 0);

      double values[] = { 1.0 };
      double out[1];
      universe_rank(u, values, 0, out); /* n=0 should not crash */

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should handle single asset universe") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = universe_create(&arena);

      universe_asset_t asset = {
        .id = 0, .ticker = "SOLO", .exchange = "X",
        .asset_type = UNIVERSE_ASSET_EQUITY,
        .start_date = 100, .end_date = UNIVERSE_DATE_NONE,
        .lot_size = 1, .tick_size = 0.01
      };
      universe_add_asset(u, &asset);
      universe_finalize(u);

      universe_advance(u, 200);
      check_int_eq((int)universe_active_count(u), 1);

      double values[] = { 42.0 };
      double out[1];
      universe_rank(u, values, 1, out);
      check_float_eq(out[0], 0.0, EPSILON); /* single asset rank = 0/1 = 0 */

      double sum = universe_cross_sum(u, values, 1);
      check_float_eq(sum, 42.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }

    it("should handle NULL inputs gracefully") {
      mem_pool_t arena;
      mem_init(&arena, 8192);
      universe_t *u = make_test_universe(&arena);
      universe_advance(u, 200);

      /* These should not crash */
      universe_rank(NULL, NULL, 0, NULL);
      universe_zscore(NULL, NULL, 0, NULL);
      universe_demean(NULL, NULL, 0, NULL);
      universe_clip(NULL, NULL, 0, 0, 0, NULL);
      check_float_eq(universe_cross_sum(NULL, NULL, 0), 0.0, EPSILON);

      universe_free(u);
      mem_destroy(&arena);
    }
  }

  group("Exprtk Universe Wrappers") {
    it("should return a full mask vector from filter_gt wrapper") {
      mem_pool_t arena;
      mem_pool_t scratch;
      mem_init(&arena, 16384);
      mem_init(&scratch, 4096);
      universe_t *u = make_test_universe(&arena);
      exprtk_env_t env;
      double values[] = { 50.0, 10.0, 30.0, 40.0, 20.0 };
      exprtk_value_t args[2];
      exprtk_value_t out;

      check_not_null(u);
      universe_advance(u, 120); /* active: AAPL, MSFT, AMZN */

      exprtk_env_init(&env);
      env.user_data = u;
      args[0] = exprtk_val_vec(values, 5);
      args[1] = exprtk_val_num(25.0);

      out = fn_universe_filter_gt(2, args, &env, &scratch);
      check_int_eq(out.type, EXPRTK_VAL_VECTOR);
      check_int_eq((int)out.data.vector.size, 5);
      check_float_eq(out.data.vector.data[0], 1.0, EPSILON);
      check_float_eq(out.data.vector.data[1], 0.0, EPSILON);
      check_float_eq(out.data.vector.data[2], 0.0, EPSILON); /* inactive asset stays masked out */
      check_float_eq(out.data.vector.data[3], 1.0, EPSILON); /* AMZN=40 > 25 */
      check_float_eq(out.data.vector.data[4], 0.0, EPSILON); /* inactive asset */

      exprtk_env_free(&env);
      universe_free(u);
      mem_destroy(&scratch);
      mem_destroy(&arena);
    }

    it("should return active-only sum from cross_sum wrapper") {
      mem_pool_t arena;
      mem_pool_t scratch;
      mem_init(&arena, 16384);
      mem_init(&scratch, 4096);
      universe_t *u = make_test_universe(&arena);
      exprtk_env_t env;
      double values[] = { 30.0, 10.0, 99.0, 20.0, 99.0 };
      exprtk_value_t args[1];
      exprtk_value_t out;

      check_not_null(u);
      universe_advance(u, 120); /* active: AAPL, MSFT, AMZN */

      exprtk_env_init(&env);
      env.user_data = u;
      args[0] = exprtk_val_vec(values, 5);

      out = fn_universe_cross_sum(1, args, &env, &scratch);
      check_int_eq(out.type, EXPRTK_VAL_NUMBER);
      check_float_eq(out.data.number, 60.0, EPSILON);

      exprtk_env_free(&env);
      universe_free(u);
      mem_destroy(&scratch);
      mem_destroy(&arena);
    }
  }
}
