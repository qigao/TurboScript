/**
 * @file test_nash.c
 * @brief Tests for the TurboScript Nash helpers in the finance module.
 */

#include "fin.h"
#include "exprtk.h"
#include "exprtk_module.h"
#include "tinytest.h"
#include <math.h>
#include <string.h>

#define EPSILON 0.05

suite("Nash Equilibrium") {
  group("game metadata") {
    it("classifies a zero-sum matching-pennies game") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[4];
      exprtk_value_t result;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 4096);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);

      result = exprtk_nash_game(4, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "rows").data.number, 2.0, 1e-9);
      check_float_eq(exprtk_map_get(&result, "cols").data.number, 2.0, 1e-9);
      check_float_eq(exprtk_map_get(&result, "zero_sum").data.number, 1.0, 1e-9);
      check_float_eq(exprtk_map_get(&result, "symmetric").data.number, 0.0, 1e-9);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("support enumeration") {
    it("matches the Nashpy README bimatrix example with three equilibria") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      exprtk_value_t equilibria;
      double row_payoffs[] = {1.0, 2.0, 3.0, 0.0};
      double col_payoffs[] = {0.0, 2.0, 3.0, 1.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(1e-8);

      result = exprtk_nash_support_enumeration(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "count").data.number, 3.0, 1e-9);

      equilibria = exprtk_map_get(&result, "equilibria");
      check_int_eq(equilibria.type, EXPRTK_VAL_LIST);
      check_int_eq((int)equilibria.data.list.count, 3);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("matches Nashpy's 2x2 support-enumeration fixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      exprtk_value_t equilibria;
      int found_pure_00 = 0;
      int found_pure_11 = 0;
      int found_mixed = 0;
      double row_payoffs[] = {2.0, 1.0, 0.0, 2.0};
      double col_payoffs[] = {2.0, 0.0, 1.0, 2.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(1e-8);

      result = exprtk_nash_support_enumeration(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "count").data.number, 3.0, 1e-9);

      equilibria = exprtk_map_get(&result, "equilibria");
      check_int_eq(equilibria.type, EXPRTK_VAL_LIST);
      for (size_t i = 0; i < equilibria.data.list.count; i++) {
        exprtk_value_t eq = equilibria.data.list.items[i];
        exprtk_value_t row_vec = exprtk_map_get(&eq, "row_strategy");
        exprtk_value_t col_vec = exprtk_map_get(&eq, "col_strategy");

        if (fabs(row_vec.data.vector.data[0] - 1.0) < 1e-9 &&
            fabs(col_vec.data.vector.data[0] - 1.0) < 1e-9) {
          found_pure_00 = 1;
        }
        if (fabs(row_vec.data.vector.data[1] - 1.0) < 1e-9 &&
            fabs(col_vec.data.vector.data[1] - 1.0) < 1e-9) {
          found_pure_11 = 1;
        }
        if (fabs(row_vec.data.vector.data[0] - (1.0 / 3.0)) < 1e-6 &&
            fabs(row_vec.data.vector.data[1] - (2.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[0] - (1.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[1] - (2.0 / 3.0)) < 1e-6) {
          found_mixed = 1;
        }
      }

      check(found_pure_00);
      check(found_pure_11);
      check(found_mixed);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("finds the mixed equilibrium of matching pennies") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      exprtk_value_t equilibria;
      exprtk_value_t eq0;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(1e-8);

      result = exprtk_nash_support_enumeration(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "count").data.number, 1.0, 1e-9);

      equilibria = exprtk_map_get(&result, "equilibria");
      check_int_eq(equilibria.type, EXPRTK_VAL_LIST);
      check_int_eq((int)equilibria.data.list.count, 1);

      eq0 = equilibria.data.list.items[0];
      row_vec = exprtk_map_get(&eq0, "row_strategy");
      col_vec = exprtk_map_get(&eq0, "col_strategy");

      check_float_eq(row_vec.data.vector.data[0], 0.5, EPSILON);
      check_float_eq(row_vec.data.vector.data[1], 0.5, EPSILON);
      check_float_eq(col_vec.data.vector.data[0], 0.5, EPSILON);
      check_float_eq(col_vec.data.vector.data[1], 0.5, EPSILON);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("vertex enumeration") {
    it("matches Nashpy's 3x2 vertex-enumeration fixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      exprtk_value_t equilibria;
      int found_pure = 0;
      int found_mixed_a = 0;
      int found_mixed_b = 0;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 6);
      args[1] = exprtk_val_vec(col_payoffs, 6);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(1e-8);

      result = exprtk_nash_vertex_enumeration(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "count").data.number, 3.0, 1e-9);

      equilibria = exprtk_map_get(&result, "equilibria");
      for (size_t i = 0; i < equilibria.data.list.count; i++) {
        exprtk_value_t eq = equilibria.data.list.items[i];
        exprtk_value_t row_vec = exprtk_map_get(&eq, "row_strategy");
        exprtk_value_t col_vec = exprtk_map_get(&eq, "col_strategy");

        if (fabs(row_vec.data.vector.data[0] - 1.0) < 1e-9 &&
            fabs(col_vec.data.vector.data[0] - 1.0) < 1e-9) {
          found_pure = 1;
        }
        if (fabs(row_vec.data.vector.data[1] - (1.0 / 3.0)) < 1e-6 &&
            fabs(row_vec.data.vector.data[2] - (2.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[0] - (1.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[1] - (2.0 / 3.0)) < 1e-6) {
          found_mixed_a = 1;
        }
        if (fabs(row_vec.data.vector.data[0] - 0.8) < 1e-6 &&
            fabs(row_vec.data.vector.data[1] - 0.2) < 1e-6 &&
            fabs(col_vec.data.vector.data[0] - (2.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[1] - (1.0 / 3.0)) < 1e-6) {
          found_mixed_b = 1;
        }
      }

      check(found_pure);
      check(found_mixed_a);
      check(found_mixed_b);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("lemke howson") {
    it("matches the Nashpy 3x2 fixture for selected dropped labels") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t eq;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 6);
      args[1] = exprtk_val_vec(col_payoffs, 6);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(2.0);

      args[4] = exprtk_val_num(0.0);
      eq = exprtk_nash_lemke_howson(5, args, &env, &arena);
      check_int_eq(exprtk_map_get(&eq, "engine").type, EXPRTK_VAL_STRING);
      check(strncmp(exprtk_map_get(&eq, "engine").data.string.data, "tableau", 7) == 0);
      row_vec = exprtk_map_get(&eq, "row_strategy");
      col_vec = exprtk_map_get(&eq, "col_strategy");
      check_float_eq(row_vec.data.vector.data[0], 1.0, 1e-9);
      check_float_eq(col_vec.data.vector.data[0], 1.0, 1e-9);
      exprtk_map_free(&eq);

      args[4] = exprtk_val_num(1.0);
      eq = exprtk_nash_lemke_howson(5, args, &env, &arena);
      row_vec = exprtk_map_get(&eq, "row_strategy");
      col_vec = exprtk_map_get(&eq, "col_strategy");
      check_float_eq(row_vec.data.vector.data[0], 0.0, 1e-6);
      check_float_eq(row_vec.data.vector.data[1], 1.0 / 3.0, 1e-6);
      check_float_eq(row_vec.data.vector.data[2], 2.0 / 3.0, 1e-6);
      check_float_eq(col_vec.data.vector.data[0], 1.0 / 3.0, 1e-6);
      check_float_eq(col_vec.data.vector.data[1], 2.0 / 3.0, 1e-6);
      exprtk_map_free(&eq);

      args[4] = exprtk_val_num(4.0);
      eq = exprtk_nash_lemke_howson(5, args, &env, &arena);
      row_vec = exprtk_map_get(&eq, "row_strategy");
      col_vec = exprtk_map_get(&eq, "col_strategy");
      check_float_eq(row_vec.data.vector.data[1], 1.0 / 3.0, 1e-6);
      check_float_eq(row_vec.data.vector.data[2], 2.0 / 3.0, 1e-6);
      check_float_eq(col_vec.data.vector.data[0], 1.0 / 3.0, 1e-6);
      check_float_eq(col_vec.data.vector.data[1], 2.0 / 3.0, 1e-6);
      exprtk_map_free(&eq);

      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("fictitious play") {
    it("tracks Nashpy's 3x3 fictitious-play fixture toward the symmetric mixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {
        0.5, 1.0, 0.0,
        0.0, 0.5, 1.0,
        1.0, 0.0, 0.5
      };
      double col_payoffs[] = {
        0.5, 0.0, 1.0,
        1.0, 0.5, 0.0,
        0.0, 1.0, 0.5
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 9);
      args[1] = exprtk_val_vec(col_payoffs, 9);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(3.0);
      args[4] = exprtk_val_num(10000.0);

      result = exprtk_nash_fictitious_play(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");

      check_float_eq(row_vec.data.vector.data[0], 1.0 / 3.0, 0.08);
      check_float_eq(row_vec.data.vector.data[1], 1.0 / 3.0, 0.08);
      check_float_eq(row_vec.data.vector.data[2], 1.0 / 3.0, 0.08);
      check_float_eq(col_vec.data.vector.data[0], 1.0 / 3.0, 0.08);
      check_float_eq(col_vec.data.vector.data[1], 1.0 / 3.0, 0.08);
      check_float_eq(col_vec.data.vector.data[2], 1.0 / 3.0, 0.08);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("tracks empirical play toward the matching-pennies mixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(2000.0);

      result = exprtk_nash_fictitious_play(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");

      check_float_eq(row_vec.data.vector.data[0], 0.5, 0.15);
      check_float_eq(row_vec.data.vector.data[1], 0.5, 0.15);
      check_float_eq(col_vec.data.vector.data[0], 0.5, 0.15);
      check_float_eq(col_vec.data.vector.data[1], 0.5, 0.15);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("stochastic fictitious play is deterministic for a fixed seed and stays near the mixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[7];
      exprtk_value_t first;
      exprtk_value_t second;
      exprtk_value_t row_first;
      exprtk_value_t col_first;
      exprtk_value_t row_second;
      exprtk_value_t col_second;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(4000.0);
      args[5] = exprtk_val_num(0.2);
      args[6] = exprtk_val_num(7.0);

      first = exprtk_nash_stochastic_fictitious_play(7, args, &env, &arena);
      second = exprtk_nash_stochastic_fictitious_play(7, args, &env, &arena);
      check_int_eq(first.type, EXPRTK_VAL_MAP);
      check_int_eq(second.type, EXPRTK_VAL_MAP);

      row_first = exprtk_map_get(&first, "row_strategy");
      col_first = exprtk_map_get(&first, "col_strategy");
      row_second = exprtk_map_get(&second, "row_strategy");
      col_second = exprtk_map_get(&second, "col_strategy");

      check_float_eq(row_first.data.vector.data[0], row_second.data.vector.data[0], 1e-12);
      check_float_eq(row_first.data.vector.data[1], row_second.data.vector.data[1], 1e-12);
      check_float_eq(col_first.data.vector.data[0], col_second.data.vector.data[0], 1e-12);
      check_float_eq(col_first.data.vector.data[1], col_second.data.vector.data[1], 1e-12);

      check_float_eq(row_first.data.vector.data[0], 0.5, 0.12);
      check_float_eq(row_first.data.vector.data[1], 0.5, 0.12);
      check_float_eq(col_first.data.vector.data[0], 0.5, 0.12);
      check_float_eq(col_first.data.vector.data[1], 0.5, 0.12);

      exprtk_map_free(&first);
      exprtk_map_free(&second);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("discrete replicator dynamics") {
    it("matches Nashpy's one-generation quantized update") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t history;
      double payoffs[] = {
        4.0, 3.0,
        2.0, 5.0
      };
      double population[] = {20.0, 80.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(payoffs, 4);
      args[1] = exprtk_val_num(2.0);
      args[2] = exprtk_val_vec(population, 2);
      args[3] = exprtk_val_num(1.0);
      args[4] = exprtk_val_num(1.0);
      args[5] = exprtk_val_num(2.0);

      result = exprtk_nash_discrete_replicator_dynamics(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      history = exprtk_map_get(&result, "history");
      check_int_eq(history.type, EXPRTK_VAL_LIST);
      check_int_eq((int)history.data.list.count, 1);
      check_float_eq(history.data.list.items[0].data.vector.data[0], 15.0, 1e-9);
      check_float_eq(history.data.list.items[0].data.vector.data[1], 85.0, 1e-9);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("matches Nashpy's type-1 single-step counts") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t population_out;
      double payoffs[] = {
        4.0, 3.0,
        2.0, 5.0
      };
      double population[] = {20.0, 80.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(payoffs, 4);
      args[1] = exprtk_val_num(2.0);
      args[2] = exprtk_val_vec(population, 2);
      args[3] = exprtk_val_num(1.0);
      args[4] = exprtk_val_num(0.0);
      args[5] = exprtk_val_num(1.0);

      result = exprtk_nash_discrete_replicator_dynamics(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      population_out = exprtk_map_get(&result, "population");
      check_float_eq(population_out.data.vector.data[0], 0.8, 1e-9);
      check_float_eq(population_out.data.vector.data[1], 99.2, 1e-9);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("imitation dynamics") {
    it("keeps thresholded strategies in range for positive and negative payoffs") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[8];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      exprtk_value_t raw_row;
      exprtk_value_t raw_col;
      double row_positive[] = {3.0, 0.0, 1.0, 3.0};
      double col_positive[] = {0.0, 1.0, 3.0, 0.0};
      double row_negative[] = {-1.0, 0.0, 0.0, -1.0};
      double col_negative[] = {0.0, -1.0, -1.0, 0.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_positive, 4);
      args[1] = exprtk_val_vec(col_positive, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(32.0);
      args[5] = exprtk_val_num(64.0);
      args[6] = exprtk_val_num(17.0);
      args[7] = exprtk_val_num(0.5);

      result = exprtk_nash_imitation_dynamics(8, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");
      raw_row = exprtk_map_get(&result, "raw_row_strategy");
      raw_col = exprtk_map_get(&result, "raw_col_strategy");
      for (size_t i = 0; i < row_vec.data.vector.size; i++) {
        check(row_vec.data.vector.data[i] >= 0.0 && row_vec.data.vector.data[i] <= 1.0);
        check(raw_row.data.vector.data[i] >= 0.0 && raw_row.data.vector.data[i] <= 1.0);
      }
      for (size_t i = 0; i < col_vec.data.vector.size; i++) {
        check(col_vec.data.vector.data[i] >= 0.0 && col_vec.data.vector.data[i] <= 1.0);
        check(raw_col.data.vector.data[i] >= 0.0 && raw_col.data.vector.data[i] <= 1.0);
      }
      exprtk_map_free(&result);

      args[0] = exprtk_val_vec(row_negative, 4);
      args[1] = exprtk_val_vec(col_negative, 4);
      result = exprtk_nash_imitation_dynamics(8, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");
      for (size_t i = 0; i < row_vec.data.vector.size; i++) {
        check(row_vec.data.vector.data[i] >= 0.0 && row_vec.data.vector.data[i] <= 1.0);
      }
      for (size_t i = 0; i < col_vec.data.vector.size; i++) {
        check(col_vec.data.vector.data[i] >= 0.0 && col_vec.data.vector.data[i] <= 1.0);
      }

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("is deterministic for a fixed seed") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[8];
      exprtk_value_t first;
      exprtk_value_t second;
      exprtk_value_t row_first;
      exprtk_value_t col_first;
      exprtk_value_t row_second;
      exprtk_value_t col_second;
      double row_payoffs[] = {3.0, 0.0, 1.0, 3.0};
      double col_payoffs[] = {0.0, 1.0, 3.0, 0.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(50.0);
      args[5] = exprtk_val_num(50.0);
      args[6] = exprtk_val_num(23.0);
      args[7] = exprtk_val_num(0.5);

      first = exprtk_nash_imitation_dynamics(8, args, &env, &arena);
      second = exprtk_nash_imitation_dynamics(8, args, &env, &arena);
      check_int_eq(first.type, EXPRTK_VAL_MAP);
      check_int_eq(second.type, EXPRTK_VAL_MAP);

      row_first = exprtk_map_get(&first, "row_strategy");
      col_first = exprtk_map_get(&first, "col_strategy");
      row_second = exprtk_map_get(&second, "row_strategy");
      col_second = exprtk_map_get(&second, "col_strategy");
      check_float_eq(row_first.data.vector.data[0], row_second.data.vector.data[0], 1e-12);
      check_float_eq(row_first.data.vector.data[1], row_second.data.vector.data[1], 1e-12);
      check_float_eq(col_first.data.vector.data[0], col_second.data.vector.data[0], 1e-12);
      check_float_eq(col_first.data.vector.data[1], col_second.data.vector.data[1], 1e-12);

      exprtk_map_free(&first);
      exprtk_map_free(&second);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("regret minimization") {
    it("matches the Nashpy zero-sum rock-paper-scissors fixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {
        0.0, -1.0, 1.0,
        1.0, 0.0, -1.0,
        -1.0, 1.0, 0.0
      };
      double col_payoffs[] = {
        0.0, 1.0, -1.0,
        -1.0, 0.0, 1.0,
        1.0, -1.0, 0.0
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 9);
      args[1] = exprtk_val_vec(col_payoffs, 9);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(3.0);
      args[4] = exprtk_val_num(0.1);
      args[5] = exprtk_val_num(100.0);

      result = exprtk_nash_regret_minimization(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");
      check_float_eq(row_vec.data.vector.data[0], 1.0 / 3.0, 1e-12);
      check_float_eq(row_vec.data.vector.data[1], 1.0 / 3.0, 1e-12);
      check_float_eq(row_vec.data.vector.data[2], 1.0 / 3.0, 1e-12);
      check_float_eq(col_vec.data.vector.data[0], 1.0 / 3.0, 1e-12);
      check_float_eq(col_vec.data.vector.data[1], 1.0 / 3.0, 1e-12);
      check_float_eq(col_vec.data.vector.data[2], 1.0 / 3.0, 1e-12);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("matches the Nashpy non-zero-sum pure-strategy fixture") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {
        3.0, -1.0, 3.0,
        -1.0, 3.0, 6.0,
        -1.0, 1.0, 2.0
      };
      double col_payoffs[] = {
        -3.0, 1.0, 4.0,
        1.0, -3.0, 3.0,
        -1.0, 3.0, 4.0
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 9);
      args[1] = exprtk_val_vec(col_payoffs, 9);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(3.0);
      args[4] = exprtk_val_num(0.1);
      args[5] = exprtk_val_num(100.0);

      result = exprtk_nash_regret_minimization(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");
      check_float_eq(row_vec.data.vector.data[0], 0.0, 1e-12);
      check_float_eq(row_vec.data.vector.data[1], 1.0, 1e-12);
      check_float_eq(row_vec.data.vector.data[2], 0.0, 1e-12);
      check_float_eq(col_vec.data.vector.data[0], 0.0, 1e-12);
      check_float_eq(col_vec.data.vector.data[1], 0.0, 1e-12);
      check_float_eq(col_vec.data.vector.data[2], 1.0, 1e-12);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("introspection dynamics") {
    it("is deterministic for a fixed seed and preserves valid actions") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[8];
      exprtk_value_t first;
      exprtk_value_t second;
      exprtk_value_t history;
      double row_payoffs[] = {
        3.0, 4.0,
        5.0, 1.0,
        6.0, 3.0
      };
      double col_payoffs[] = {
        5.0, 2.0,
        3.0, 4.0,
        1.0, 9.0
      };
      double initial_actions[] = {2.0, 1.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 6);
      args[1] = exprtk_val_vec(col_payoffs, 6);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(10.0);
      args[5] = exprtk_val_num(1.0);
      args[6] = exprtk_val_num(11.0);
      args[7] = exprtk_val_vec(initial_actions, 2);

      first = exprtk_nash_introspection_dynamics(8, args, &env, &arena);
      second = exprtk_nash_introspection_dynamics(8, args, &env, &arena);
      check_int_eq(first.type, EXPRTK_VAL_MAP);
      check_int_eq(second.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&first, "steps").data.number, 11.0, 1e-9);
      check_float_eq(exprtk_map_get(&second, "steps").data.number, 11.0, 1e-9);

      history = exprtk_map_get(&first, "history");
      check_int_eq(history.type, EXPRTK_VAL_LIST);
      check_int_eq((int)history.data.list.count, 11);
      for (size_t i = 0; i < history.data.list.count; i++) {
        exprtk_value_t step_a = history.data.list.items[i];
        exprtk_value_t step_b = exprtk_map_get(&second, "history").data.list.items[i];
        check_int_eq(step_a.type, EXPRTK_VAL_LIST);
        check_float_eq(step_a.data.list.items[0].data.number, step_b.data.list.items[0].data.number, 1e-12);
        check_float_eq(step_a.data.list.items[1].data.number, step_b.data.list.items[1].data.number, 1e-12);
        check(step_a.data.list.items[0].data.number >= 0.0 && step_a.data.list.items[0].data.number < 3.0);
        check(step_a.data.list.items[1].data.number >= 0.0 && step_a.data.list.items[1].data.number < 2.0);
      }

      exprtk_map_free(&first);
      exprtk_map_free(&second);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("repeated games") {
    it("matches Nashpy repeated-game dimensions for a 2x2 stage game repeated twice") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[5];
      exprtk_value_t result;
      double row_payoffs[] = {0.0, 11.0, -22.0, 3.0};
      double col_payoffs[] = {2.0, 1.0, -2.0, -3.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(2.0);

      result = exprtk_nash_repeated_game(5, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "rows").data.number, 32.0, 1e-9);
      check_float_eq(exprtk_map_get(&result, "cols").data.number, 32.0, 1e-9);
      check_float_eq(exprtk_map_get(&result, "states").data.number, 5.0, 1e-9);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("moran process") {
    it("returns a single generation for an already fixed population") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t history;
      exprtk_value_t population_items[3];
      double payoffs[] = {
        4.0, 3.0, 2.0,
        1.0, 2.0, 5.0,
        6.0, 1.0, 3.0
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      population_items[0] = exprtk_val_num(0.0);
      population_items[1] = exprtk_val_num(0.0);
      population_items[2] = exprtk_val_num(0.0);

      args[0] = exprtk_val_vec(payoffs, 9);
      args[1] = exprtk_val_num(3.0);
      args[2] = exprtk_val_list_ex(population_items, 3, 0);
      args[3] = exprtk_val_num(10.0);
      args[4] = exprtk_val_num(0.0);
      args[5] = exprtk_val_num(0.0);

      result = exprtk_nash_moran_process(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      check_float_eq(exprtk_map_get(&result, "steps").data.number, 1.0, 1e-9);
      history = exprtk_map_get(&result, "history");
      check_int_eq(history.type, EXPRTK_VAL_LIST);
      check_int_eq((int)history.data.list.count, 1);
      check_float_eq(history.data.list.items[0].data.list.items[0].data.number, 0.0, 1e-9);
      check_float_eq(history.data.list.items[0].data.list.items[1].data.number, 0.0, 1e-9);
      check_float_eq(history.data.list.items[0].data.list.items[2].data.number, 0.0, 1e-9);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("respects an identity replacement graph") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[8];
      exprtk_value_t result;
      exprtk_value_t history;
      exprtk_value_t population_items[3];
      double payoffs[] = {
        4.0, 3.0, 2.0,
        1.0, 2.0, 5.0,
        6.0, 1.0, 3.0
      };
      double replacement[] = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
      };

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      population_items[0] = exprtk_val_num(0.0);
      population_items[1] = exprtk_val_num(1.0);
      population_items[2] = exprtk_val_num(2.0);

      args[0] = exprtk_val_vec(payoffs, 9);
      args[1] = exprtk_val_num(3.0);
      args[2] = exprtk_val_list_ex(population_items, 3, 0);
      args[3] = exprtk_val_num(5.0);
      args[4] = exprtk_val_num(0.0);
      args[5] = exprtk_val_num(3.0);
      args[6] = exprtk_val_vec(replacement, 9);
      args[7] = exprtk_val_num(0.0);

      result = exprtk_nash_moran_process(7, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      history = exprtk_map_get(&result, "history");
      for (size_t i = 0; i < history.data.list.count; i++) {
        check_float_eq(history.data.list.items[i].data.list.items[0].data.number, 0.0, 1e-9);
        check_float_eq(history.data.list.items[i].data.list.items[1].data.number, 1.0, 1e-9);
        check_float_eq(history.data.list.items[i].data.list.items[2].data.number, 2.0, 1e-9);
      }

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("mutation dynamics") {
    it("keeps the matching-pennies mixed equilibrium fixed under identity mutation") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t strategy;
      double payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double mutation[] = {1.0, 0.0, 0.0, 1.0};
      double initial[] = {0.5, 0.5};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(payoffs, 4);
      args[1] = exprtk_val_num(2.0);
      args[2] = exprtk_val_num(200.0);
      args[3] = exprtk_val_num(0.02);
      args[4] = exprtk_val_vec(mutation, 4);
      args[5] = exprtk_val_vec(initial, 2);

      result = exprtk_nash_replicator_mutation(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      strategy = exprtk_map_get(&result, "strategy");
      check_float_eq(strategy.data.vector.data[0], 0.5, 1e-6);
      check_float_eq(strategy.data.vector.data[1], 0.5, 1e-6);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("asymmetric replicator") {
    it("has near-zero derivative at the Nashpy 3x2 mixed equilibrium") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[6];
      exprtk_value_t result;
      exprtk_value_t row_derivative;
      exprtk_value_t col_derivative;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };
      double row_state[] = {0.0, 1.0 / 3.0, 2.0 / 3.0};
      double col_state[] = {1.0 / 3.0, 2.0 / 3.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 6);
      args[1] = exprtk_val_vec(col_payoffs, 6);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_vec(row_state, 3);
      args[5] = exprtk_val_vec(col_state, 2);

      result = exprtk_nash_asymmetric_replicator_derivative(6, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_derivative = exprtk_map_get(&result, "row_derivative");
      col_derivative = exprtk_map_get(&result, "col_derivative");
      check_float_eq(row_derivative.data.vector.data[0], 0.0, 1e-9);
      check_float_eq(row_derivative.data.vector.data[1], 0.0, 1e-9);
      check_float_eq(row_derivative.data.vector.data[2], 0.0, 1e-9);
      check_float_eq(col_derivative.data.vector.data[0], 0.0, 1e-9);
      check_float_eq(col_derivative.data.vector.data[1], 0.0, 1e-9);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }

    it("keeps the same mixed equilibrium stable under Euler integration") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[8];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };
      double row_state[] = {0.0, 1.0 / 3.0, 2.0 / 3.0};
      double col_state[] = {1.0 / 3.0, 2.0 / 3.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 6);
      args[1] = exprtk_val_vec(col_payoffs, 6);
      args[2] = exprtk_val_num(3.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(500.0);
      args[5] = exprtk_val_num(0.01);
      args[6] = exprtk_val_vec(row_state, 3);
      args[7] = exprtk_val_vec(col_state, 2);

      result = exprtk_nash_asymmetric_replicator(8, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);
      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");

      check_float_eq(row_vec.data.vector.data[0], 0.0, 1e-6);
      check_float_eq(row_vec.data.vector.data[1], 1.0 / 3.0, 1e-4);
      check_float_eq(row_vec.data.vector.data[2], 2.0 / 3.0, 1e-4);
      check_float_eq(col_vec.data.vector.data[0], 1.0 / 3.0, 1e-4);
      check_float_eq(col_vec.data.vector.data[1], 2.0 / 3.0, 1e-4);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("replicator") {
    it("converges to matching pennies mixed strategy") {
      mem_pool_t arena;
      exprtk_env_t env;
      exprtk_value_t args[7];
      exprtk_value_t result;
      exprtk_value_t row_vec;
      exprtk_value_t col_vec;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      exprtk_env_init(&env);

      args[0] = exprtk_val_vec(row_payoffs, 4);
      args[1] = exprtk_val_vec(col_payoffs, 4);
      args[2] = exprtk_val_num(2.0);
      args[3] = exprtk_val_num(2.0);
      args[4] = exprtk_val_num(500.0);
      args[5] = exprtk_val_num(0.1);
      args[6] = exprtk_val_num(1e-6);

      result = exprtk_nash_replicator(7, args, &env, &arena);
      check_int_eq(result.type, EXPRTK_VAL_MAP);

      row_vec = exprtk_map_get(&result, "row_strategy");
      col_vec = exprtk_map_get(&result, "col_strategy");
      check_float_eq(row_vec.data.vector.data[0], 0.5, EPSILON);
      check_float_eq(row_vec.data.vector.data[1], 0.5, EPSILON);
      check_float_eq(col_vec.data.vector.data[0], 0.5, EPSILON);
      check_float_eq(col_vec.data.vector.data[1], 0.5, EPSILON);
      check(exprtk_map_get(&result, "converged").data.number >= 1.0);

      exprtk_map_free(&result);
      exprtk_env_free(&env);
      mem_destroy(&arena);
    }
  }
}
