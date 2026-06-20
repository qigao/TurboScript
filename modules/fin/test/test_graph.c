/**
 * @file test_graph.c
 * @brief Unit tests for graph algorithms in fin module
 */

#include "fin.h"
#include "exprtk_module.h"
#include "tinytest.h"
#include <math.h>
#include <float.h>
#include <string.h>

#define EPSILON 0.001

/* Helper to find function by name in module */
static exprtk_builtin_fn find_function(const exprtk_module_t *mod, const char *name) {
  for (size_t i = 0; i < mod->count; i++) {
    if (strcmp(mod->entries[i].name, name) == 0) {
      return mod->entries[i].fn;
    }
  }
  return NULL;
}

suite("Graph Algorithms") {
  group("Bellman-Ford") {
    it("should find shortest paths in simple graph") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // Simple graph: 0 -> 1 (5), 0 -> 2 (3), 1 -> 2 (1), 2 -> 1 (2)
      double edges[] = {
        0, 1, 5,
        0, 2, 3,
        1, 2, 1,
        2, 1, 2
      };
      size_t n_vertices = 3;
      size_t n_edges = 4;
      size_t source = 0;

      double dist[3];
      double prev[3];

      // Call via exprtk wrapper
      exprtk_value_t args[6];
      args[0] = exprtk_val_vec(edges, 12);
      args[1] = exprtk_val_num(n_vertices);
      args[2] = exprtk_val_num(n_edges);
      args[3] = exprtk_val_num(source);
      args[4] = exprtk_val_vec(dist, 3);
      args[5] = exprtk_val_vec(prev, 3);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "bellman_ford");
      exprtk_value_t result = fn(6, args, NULL, &arena);

      // Should succeed (no negative cycle)
      check_float_eq(result.data.number, 0.0, EPSILON);

      // Check distances
      check_float_eq(dist[0], 0.0, EPSILON);  // Source
      check_float_eq(dist[1], 5.0, EPSILON);  // 0 -> 2 -> 1 = 3 + 2 = 5
      check_float_eq(dist[2], 3.0, EPSILON);  // 0 -> 2 = 3

      mem_destroy(&arena);
    }

    it("should detect negative cycle") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // Graph with negative cycle: 0 -> 1 (1), 1 -> 2 (2), 2 -> 0 (-5)
      double edges[] = {
        0, 1, 1,
        1, 2, 2,
        2, 0, -5
      };
      size_t n_vertices = 3;
      size_t n_edges = 3;
      size_t source = 0;

      double dist[3];
      double prev[3];

      exprtk_value_t args[6];
      args[0] = exprtk_val_vec(edges, 9);
      args[1] = exprtk_val_num(n_vertices);
      args[2] = exprtk_val_num(n_edges);
      args[3] = exprtk_val_num(source);
      args[4] = exprtk_val_vec(dist, 3);
      args[5] = exprtk_val_vec(prev, 3);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "bellman_ford");
      exprtk_value_t result = fn(6, args, NULL, &arena);

      // Should detect negative cycle
      check_float_eq(result.data.number, -1.0, EPSILON);

      mem_destroy(&arena);
    }

    it("should handle disconnected vertices") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // Graph: 0 -> 1 (5), vertex 2 is disconnected
      double edges[] = {
        0, 1, 5
      };
      size_t n_vertices = 3;
      size_t n_edges = 1;
      size_t source = 0;

      double dist[3];
      double prev[3];

      exprtk_value_t args[6];
      args[0] = exprtk_val_vec(edges, 3);
      args[1] = exprtk_val_num(n_vertices);
      args[2] = exprtk_val_num(n_edges);
      args[3] = exprtk_val_num(source);
      args[4] = exprtk_val_vec(dist, 3);
      args[5] = exprtk_val_vec(prev, 3);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "bellman_ford");
      exprtk_value_t result = fn(6, args, NULL, &arena);

      check_float_eq(result.data.number, 0.0, EPSILON);
      check_float_eq(dist[0], 0.0, EPSILON);
      check_float_eq(dist[1], 5.0, EPSILON);
      check(dist[2] == DBL_MAX);  // Unreachable

      mem_destroy(&arena);
    }
  }

  group("Negative Cycle Detection") {
    it("should detect negative cycle") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      double edges[] = {
        0, 1, 1,
        1, 2, 2,
        2, 0, -5
      };

      exprtk_value_t args[3];
      args[0] = exprtk_val_vec(edges, 9);
      args[1] = exprtk_val_num(3);  // n_vertices
      args[2] = exprtk_val_num(3);  // n_edges

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "has_negative_cycle");
      exprtk_value_t result = fn(3, args, NULL, &arena);

      check_float_eq(result.data.number, 1.0, EPSILON);  // Has cycle

      mem_destroy(&arena);
    }

    it("should return false for positive cycle") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      double edges[] = {
        0, 1, 1,
        1, 2, 2,
        2, 0, 3
      };

      exprtk_value_t args[3];
      args[0] = exprtk_val_vec(edges, 9);
      args[1] = exprtk_val_num(3);
      args[2] = exprtk_val_num(3);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "has_negative_cycle");
      exprtk_value_t result = fn(3, args, NULL, &arena);

      check_float_eq(result.data.number, 0.0, EPSILON);  // No negative cycle

      mem_destroy(&arena);
    }
  }

  group("Path Extraction") {
    it("should extract path from predecessor array") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // Predecessor array: prev[0] = -1, prev[1] = 0, prev[2] = 1, prev[3] = 2
      // Path from 0 to 3: 0 -> 1 -> 2 -> 3
      double prev[] = {-1, 0, 1, 2};
      double path[10];

      exprtk_value_t args[4];
      args[0] = exprtk_val_vec(prev, 4);
      args[1] = exprtk_val_num(0);  // source
      args[2] = exprtk_val_num(3);  // target
      args[3] = exprtk_val_vec(path, 10);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "extract_path");
      exprtk_value_t result = fn(4, args, NULL, &arena);

      // Path length should be 4
      check_float_eq(result.data.number, 4.0, EPSILON);

      // Check path
      check_float_eq(path[0], 0.0, EPSILON);
      check_float_eq(path[1], 1.0, EPSILON);
      check_float_eq(path[2], 2.0, EPSILON);
      check_float_eq(path[3], 3.0, EPSILON);

      mem_destroy(&arena);
    }

    it("should return -1 for unreachable target") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      double prev[] = {-1, 0, -1};  // Vertex 2 unreachable
      double path[10];

      exprtk_value_t args[4];
      args[0] = exprtk_val_vec(prev, 3);
      args[1] = exprtk_val_num(0);
      args[2] = exprtk_val_num(2);
      args[3] = exprtk_val_vec(path, 10);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "extract_path");
      exprtk_value_t result = fn(4, args, NULL, &arena);

      check_float_eq(result.data.number, -1.0, EPSILON);

      mem_destroy(&arena);
    }
  }

  group("Currency Arbitrage Detection") {
    it("should detect arbitrage opportunity") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // 3 currencies with arbitrage: USD, EUR, GBP
      // USD -> EUR: 1.2, EUR -> GBP: 0.9, GBP -> USD: 1.0
      // Cycle: 1 USD -> 1.2 EUR -> 1.08 GBP -> 1.08 USD (profit!)
      double rates[] = {
        1.0, 1.2, 0.0,  // USD -> EUR, GBP
        0.0, 1.0, 0.9,  // EUR -> USD, GBP
        1.0, 0.0, 1.0   // GBP -> USD, EUR
      };

      exprtk_value_t args[2];
      args[0] = exprtk_val_vec(rates, 9);
      args[1] = exprtk_val_num(3);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "detect_arbitrage");
      exprtk_value_t result = fn(2, args, NULL, &arena);

      check_float_eq(result.data.number, 1.0, EPSILON);  // Arbitrage exists

      mem_destroy(&arena);
    }

    it("should return false for fair rates") {
      mem_pool_t arena;
      mem_init(&arena, 4096);

      // Fair exchange rates (no arbitrage)
      // Use simple integer ratios to avoid floating point errors
      double rates[] = {
        1.0, 2.0, 3.0,      // USD -> EUR (2x), GBP (3x)
        0.5, 1.0, 1.5,      // EUR -> USD (0.5x), GBP (1.5x)
        1.0/3.0, 2.0/3.0, 1.0   // GBP -> USD (1/3x), EUR (2/3x)
      };

      exprtk_value_t args[2];
      args[0] = exprtk_val_vec(rates, 9);
      args[1] = exprtk_val_num(3);

      const exprtk_module_t *mod = exprtk_module_strategy();
      exprtk_builtin_fn fn = find_function(mod, "detect_arbitrage");
      exprtk_value_t result = fn(2, args, NULL, &arena);

      check_float_eq(result.data.number, 0.0, EPSILON);  // No arbitrage

      mem_destroy(&arena);
    }
  }
}
