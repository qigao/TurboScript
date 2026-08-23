/**
 * @file exprtk_nash.c
 * @brief TurboScript-only normal-form game helpers inspired by Nashpy.
 */

#include "exprtk_module.h"
#include "fin.h"
#include "simd_helpers.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const double *row_payoffs;
  const double *col_payoffs;
  size_t n_rows;
  size_t n_cols;
} nash_game_t;

typedef struct {
  uint64_t state;
} nash_rng_t;

typedef struct {
  double *data;
  size_t rows;
  size_t cols;
  size_t label_count;
  size_t *original_basic_labels;
  size_t original_basic_count;
  size_t *slack_labels;
  size_t slack_count;
  unsigned char *non_basic;
  int lexicographic;
  double *scratch_a;
  double *scratch_b;
  double *ratios;
  size_t *tie_rows;
} nash_tableau_t;

static size_t best_response_row(const nash_game_t *game, const double *col_strategy);
static size_t best_response_col(const nash_game_t *game, const double *row_strategy);

static double clamp_exp_arg(double v) {
  if (v > 20.0) return 20.0;
  if (v < -20.0) return -20.0;
  return v;
}

static void nash_rng_seed(nash_rng_t *rng, uint64_t seed) {
  rng->state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

static uint64_t nash_rng_next_u64(nash_rng_t *rng) {
  uint64_t x = rng->state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng->state = x;
  return x * 2685821657736338717ULL;
}

static double nash_rng_next_uniform(nash_rng_t *rng) {
  uint64_t v = nash_rng_next_u64(rng);
  return (double)(v >> 11) * (1.0 / 9007199254740992.0);
}

static size_t nash_rng_choice_uniform(nash_rng_t *rng, size_t n) {
  if (n == 0) return 0;
  return (size_t)(nash_rng_next_uniform(rng) * (double)n) % n;
}

static size_t nash_rng_choice_weighted(nash_rng_t *rng, const double *weights, size_t n) {
  double total = 0.0;
  for (size_t i = 0; i < n; i++) total += weights[i];
  if (total <= 1e-15) return nash_rng_choice_uniform(rng, n);
  {
    double draw = nash_rng_next_uniform(rng) * total;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
      acc += weights[i];
      if (draw <= acc || i + 1 == n) return i;
    }
  }
  return n - 1;
}

static int parse_game_args(size_t argc, exprtk_value_t *args, nash_game_t *game) {
  if (!game || argc < 4) return -1;
  if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_VECTOR ||
      args[2].type != EXPRTK_VAL_NUMBER || args[3].type != EXPRTK_VAL_NUMBER)
    return -1;

  size_t n_rows = (size_t)args[2].data.number;
  size_t n_cols = (size_t)args[3].data.number;
  if (n_rows == 0 || n_cols == 0) return -1;
  if (args[0].data.vector.size != n_rows * n_cols ||
      args[1].data.vector.size != n_rows * n_cols)
    return -1;

  game->row_payoffs = args[0].data.vector.data;
  game->col_payoffs = args[1].data.vector.data;
  game->n_rows = n_rows;
  game->n_cols = n_cols;
  return 0;
}

static void normalize_or_uniform(double *vals, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (vals[i] < 0.0) vals[i] = 0.0;
  }
  {
    double sum = simd_sum(vals, n);
  if (sum <= 0.0) {
    double uniform = 1.0 / (double)n;
    for (size_t i = 0; i < n; i++) vals[i] = uniform;
    return;
  }
  for (size_t i = 0; i < n; i++) vals[i] /= sum;
  }
}

static int sanitize_strategy(double *vals, size_t n, double tol) {
  for (size_t i = 0; i < n; i++) {
    if (vals[i] < -tol) return -1;
    if (fabs(vals[i]) <= tol) vals[i] = 0.0;
  }
  normalize_or_uniform(vals, n);
  return 0;
}

static double row_expected_payoff(const nash_game_t *game, size_t row_idx, const double *col_strategy) {
  const double *row = game->row_payoffs + row_idx * game->n_cols;
  return simd_dot(row, col_strategy, game->n_cols);
}

static double col_expected_payoff(const nash_game_t *game, size_t col_idx, const double *row_strategy) {
  double payoff = 0.0;
  for (size_t i = 0; i < game->n_rows; i++) {
    payoff += game->col_payoffs[i * game->n_cols + col_idx] * row_strategy[i];
  }
  return payoff;
}

static double mixed_row_payoff(const nash_game_t *game, const double *row_strategy, const double *col_strategy) {
  double payoff = 0.0;
  for (size_t i = 0; i < game->n_rows; i++) {
    payoff += row_strategy[i] * row_expected_payoff(game, i, col_strategy);
  }
  return payoff;
}

static double mixed_col_payoff(const nash_game_t *game, const double *row_strategy, const double *col_strategy) {
  double payoff = 0.0;
  for (size_t j = 0; j < game->n_cols; j++) {
    payoff += col_strategy[j] * col_expected_payoff(game, j, row_strategy);
  }
  return payoff;
}

static void belief_from_counts(const double *counts, size_t n, double *out) {
  memcpy(out, counts, n * sizeof(double));
  normalize_or_uniform(out, n);
}

static void stochastic_best_response_row(const nash_game_t *game,
                                         const double *col_strategy,
                                         double temperature,
                                         nash_rng_t *rng,
                                         size_t *out_action,
                                         double *probabilities) {
  double max_payoff = -INFINITY;
  for (size_t i = 0; i < game->n_rows; i++) {
    double payoff = row_expected_payoff(game, i, col_strategy);
    probabilities[i] = payoff;
    if (payoff > max_payoff) max_payoff = payoff;
  }
  if (temperature <= 1e-12) {
    *out_action = best_response_row(game, col_strategy);
    return;
  }
  for (size_t i = 0; i < game->n_rows; i++) {
    probabilities[i] = exp(clamp_exp_arg((probabilities[i] - max_payoff) / temperature));
  }
  normalize_or_uniform(probabilities, game->n_rows);
  {
    double draw = nash_rng_next_uniform(rng);
    double acc = 0.0;
    for (size_t i = 0; i < game->n_rows; i++) {
      acc += probabilities[i];
      if (draw <= acc || i + 1 == game->n_rows) {
        *out_action = i;
        return;
      }
    }
  }
}

static void stochastic_best_response_col(const nash_game_t *game,
                                         const double *row_strategy,
                                         double temperature,
                                         nash_rng_t *rng,
                                         size_t *out_action,
                                         double *probabilities) {
  double max_payoff = -INFINITY;
  for (size_t j = 0; j < game->n_cols; j++) {
    double payoff = col_expected_payoff(game, j, row_strategy);
    probabilities[j] = payoff;
    if (payoff > max_payoff) max_payoff = payoff;
  }
  if (temperature <= 1e-12) {
    *out_action = best_response_col(game, row_strategy);
    return;
  }
  for (size_t j = 0; j < game->n_cols; j++) {
    probabilities[j] = exp(clamp_exp_arg((probabilities[j] - max_payoff) / temperature));
  }
  normalize_or_uniform(probabilities, game->n_cols);
  {
    double draw = nash_rng_next_uniform(rng);
    double acc = 0.0;
    for (size_t j = 0; j < game->n_cols; j++) {
      acc += probabilities[j];
      if (draw <= acc || j + 1 == game->n_cols) {
        *out_action = j;
        return;
      }
    }
  }
}

static void matrix_vector_mul(const double *mat, size_t rows, size_t cols,
                              const double *vec, double *out) {
  for (size_t r = 0; r < rows; r++) {
    out[r] = simd_dot(mat + r * cols, vec, cols);
  }
}

static void vector_matrix_mul(const double *vec, const double *mat, size_t rows, size_t cols,
                              double *out) {
  for (size_t c = 0; c < cols; c++) {
    double sum = 0.0;
    for (size_t r = 0; r < rows; r++) sum += vec[r] * mat[r * cols + c];
    out[c] = sum;
  }
}

static size_t best_response_row(const nash_game_t *game, const double *col_strategy) {
  size_t best_idx = 0;
  double best_payoff = row_expected_payoff(game, 0, col_strategy);
  for (size_t i = 1; i < game->n_rows; i++) {
    double payoff = row_expected_payoff(game, i, col_strategy);
    if (payoff > best_payoff + 1e-12) {
      best_payoff = payoff;
      best_idx = i;
    }
  }
  return best_idx;
}

static size_t best_response_col(const nash_game_t *game, const double *row_strategy) {
  size_t best_idx = 0;
  double best_payoff = col_expected_payoff(game, 0, row_strategy);
  for (size_t j = 1; j < game->n_cols; j++) {
    double payoff = col_expected_payoff(game, j, row_strategy);
    if (payoff > best_payoff + 1e-12) {
      best_payoff = payoff;
      best_idx = j;
    }
  }
  return best_idx;
}

static int next_combination(size_t *combo, size_t k, size_t n) {
  if (k == 0 || k > n) return 0;
  for (ptrdiff_t idx = (ptrdiff_t)k - 1; idx >= 0; idx--) {
    size_t max_val = n - (k - (size_t)idx);
    if (combo[idx] < max_val) {
      combo[idx]++;
      for (size_t j = (size_t)idx + 1; j < k; j++) combo[j] = combo[j - 1] + 1;
      return 1;
    }
  }
  return 0;
}

static int solve_linear_system(size_t n, double *mat, double *rhs, double *out, double tol) {
  for (size_t col = 0; col < n; col++) {
    size_t pivot = col;
    double pivot_abs = fabs(mat[pivot * n + col]);
    for (size_t row = col + 1; row < n; row++) {
      double candidate = fabs(mat[row * n + col]);
      if (candidate > pivot_abs) {
        pivot = row;
        pivot_abs = candidate;
      }
    }
    if (pivot_abs <= tol) return -1;

    if (pivot != col) {
      for (size_t j = col; j < n; j++) {
        double tmp = mat[col * n + j];
        mat[col * n + j] = mat[pivot * n + j];
        mat[pivot * n + j] = tmp;
      }
      {
        double tmp = rhs[col];
        rhs[col] = rhs[pivot];
        rhs[pivot] = tmp;
      }
    }

    {
      double piv = mat[col * n + col];
      for (size_t j = col; j < n; j++) mat[col * n + j] /= piv;
      rhs[col] /= piv;
    }

    for (size_t row = 0; row < n; row++) {
      if (row == col) continue;
      {
        double factor = mat[row * n + col];
        if (fabs(factor) <= tol) continue;
        for (size_t j = col; j < n; j++) mat[row * n + j] -= factor * mat[col * n + j];
        rhs[row] -= factor * rhs[col];
      }
    }
  }

  for (size_t i = 0; i < n; i++) out[i] = rhs[i];
  return 0;
}

static int build_support_solution(const nash_game_t *game,
                                  const size_t *row_support,
                                  const size_t *col_support,
                                  size_t k,
                                  double tol,
                                  double *row_strategy,
                                  double *col_strategy,
                                  double *work_mat,
                                  double *work_rhs,
                                  double *work_out) {
  memset(row_strategy, 0, game->n_rows * sizeof(double));
  memset(col_strategy, 0, game->n_cols * sizeof(double));

  for (size_t r = 0; r + 1 < k; r++) {
    size_t ref_row = row_support[0];
    size_t cur_row = row_support[r + 1];
    for (size_t c = 0; c < k; c++) {
      size_t col = col_support[c];
      work_mat[r * k + c] =
          game->row_payoffs[cur_row * game->n_cols + col] -
          game->row_payoffs[ref_row * game->n_cols + col];
    }
    work_rhs[r] = 0.0;
  }
  for (size_t c = 0; c < k; c++) work_mat[(k - 1) * k + c] = 1.0;
  work_rhs[k - 1] = 1.0;
  if (solve_linear_system(k, work_mat, work_rhs, work_out, tol) != 0) return -1;
  for (size_t c = 0; c < k; c++) col_strategy[col_support[c]] = work_out[c];
  if (sanitize_strategy(col_strategy, game->n_cols, tol) != 0) return -1;
  for (size_t c = 0; c < k; c++) {
    if (col_strategy[col_support[c]] <= tol) return -1;
  }

  for (size_t c = 0; c + 1 < k; c++) {
    size_t ref_col = col_support[0];
    size_t cur_col = col_support[c + 1];
    for (size_t r = 0; r < k; r++) {
      size_t row = row_support[r];
      work_mat[c * k + r] =
          game->col_payoffs[row * game->n_cols + cur_col] -
          game->col_payoffs[row * game->n_cols + ref_col];
    }
    work_rhs[c] = 0.0;
  }
  for (size_t r = 0; r < k; r++) work_mat[(k - 1) * k + r] = 1.0;
  work_rhs[k - 1] = 1.0;
  if (solve_linear_system(k, work_mat, work_rhs, work_out, tol) != 0) return -1;
  for (size_t r = 0; r < k; r++) row_strategy[row_support[r]] = work_out[r];
  if (sanitize_strategy(row_strategy, game->n_rows, tol) != 0) return -1;
  for (size_t r = 0; r < k; r++) {
    if (row_strategy[row_support[r]] <= tol) return -1;
  }

  return 0;
}

static int verify_equilibrium(const nash_game_t *game,
                              const double *row_strategy,
                              const double *col_strategy,
                              double tol,
                              double *out_row_payoff,
                              double *out_col_payoff) {
  double row_value = mixed_row_payoff(game, row_strategy, col_strategy);
  double col_value = mixed_col_payoff(game, row_strategy, col_strategy);

  for (size_t i = 0; i < game->n_rows; i++) {
    double payoff = row_expected_payoff(game, i, col_strategy);
    if (row_strategy[i] > tol) {
      if (fabs(payoff - row_value) > 1e-4) return -1;
    } else if (payoff > row_value + 1e-4) {
      return -1;
    }
  }

  for (size_t j = 0; j < game->n_cols; j++) {
    double payoff = col_expected_payoff(game, j, row_strategy);
    if (col_strategy[j] > tol) {
      if (fabs(payoff - col_value) > 1e-4) return -1;
    } else if (payoff > col_value + 1e-4) {
      return -1;
    }
  }

  if (out_row_payoff) *out_row_payoff = row_value;
  if (out_col_payoff) *out_col_payoff = col_value;
  return 0;
}

static int equilibrium_exists(const exprtk_value_t *list,
                              const double *row_strategy,
                              const double *col_strategy,
                              double tol) {
  if (!list || list->type != EXPRTK_VAL_LIST) return 0;
  for (size_t idx = 0; idx < list->data.list.count; idx++) {
    exprtk_value_t item = list->data.list.items[idx];
    exprtk_value_t row_vec = exprtk_map_get(&item, "row_strategy");
    exprtk_value_t col_vec = exprtk_map_get(&item, "col_strategy");
    if (row_vec.type != EXPRTK_VAL_VECTOR || col_vec.type != EXPRTK_VAL_VECTOR) continue;
    if (row_vec.data.vector.size == 0 || col_vec.data.vector.size == 0) continue;

    {
      int same = 1;
      for (size_t i = 0; i < row_vec.data.vector.size; i++) {
        if (fabs(row_vec.data.vector.data[i] - row_strategy[i]) > tol) {
          same = 0;
          break;
        }
      }
      if (!same) continue;
      for (size_t j = 0; j < col_vec.data.vector.size; j++) {
        if (fabs(col_vec.data.vector.data[j] - col_strategy[j]) > tol) {
          same = 0;
          break;
        }
      }
      if (same) return 1;
    }
  }
  return 0;
}

static exprtk_value_t make_equilibrium_map(const double *row_strategy,
                                           const double *col_strategy,
                                           size_t n_rows,
                                           size_t n_cols,
                                           double row_payoff,
                                           double col_payoff,
                                           mem_pool_t *arena) {
  exprtk_value_t eq = exprtk_val_map();
  double *row_copy = MEM_ALLOC_ARRAY(arena, double, n_rows);
  double *col_copy = MEM_ALLOC_ARRAY(arena, double, n_cols);
  if (!row_copy || !col_copy) return exprtk_val_num(NAN);
  memcpy(row_copy, row_strategy, n_rows * sizeof(double));
  memcpy(col_copy, col_strategy, n_cols * sizeof(double));
  exprtk_map_set(&eq, "row_strategy", exprtk_val_vec(row_copy, n_rows));
  exprtk_map_set(&eq, "col_strategy", exprtk_val_vec(col_copy, n_cols));
  exprtk_map_set(&eq, "row_payoff", exprtk_val_num(row_payoff));
  exprtk_map_set(&eq, "col_payoff", exprtk_val_num(col_payoff));
  return eq;
}

static exprtk_value_t clone_equilibrium_map(const exprtk_value_t *eq, mem_pool_t *arena) {
  exprtk_value_t row_vec = exprtk_map_get(eq, "row_strategy");
  exprtk_value_t col_vec = exprtk_map_get(eq, "col_strategy");
  exprtk_value_t row_payoff = exprtk_map_get(eq, "row_payoff");
  exprtk_value_t col_payoff = exprtk_map_get(eq, "col_payoff");

  if (row_vec.type != EXPRTK_VAL_VECTOR || col_vec.type != EXPRTK_VAL_VECTOR) {
    return exprtk_val_num(NAN);
  }

  return make_equilibrium_map(row_vec.data.vector.data, col_vec.data.vector.data,
                              row_vec.data.vector.size, col_vec.data.vector.size,
                              row_payoff.data.number, col_payoff.data.number, arena);
}

static exprtk_value_t enumerate_equilibria(const nash_game_t *game, double tol, mem_pool_t *arena) {
  exprtk_value_t equilibria = exprtk_val_list_empty();
  size_t max_support = (game->n_rows < game->n_cols) ? game->n_rows : game->n_cols;
  size_t *row_support = MEM_ALLOC_ARRAY(arena, size_t, max_support);
  size_t *col_support = MEM_ALLOC_ARRAY(arena, size_t, max_support);
  double *row_strategy = MEM_ALLOC_ARRAY(arena, double, game->n_rows);
  double *col_strategy = MEM_ALLOC_ARRAY(arena, double, game->n_cols);
  double *mat = MEM_ALLOC_ARRAY(arena, double, max_support * max_support);
  double *rhs = MEM_ALLOC_ARRAY(arena, double, max_support);
  double *out = MEM_ALLOC_ARRAY(arena, double, max_support);

  if (!row_support || !col_support || !row_strategy || !col_strategy || !mat || !rhs || !out) {
    return exprtk_val_num(NAN);
  }

  for (size_t k = 1; k <= max_support; k++) {
    for (size_t i = 0; i < k; i++) row_support[i] = i;
    do {
      for (size_t j = 0; j < k; j++) col_support[j] = j;
      do {
        double row_payoff = 0.0;
        double col_payoff = 0.0;
        if (build_support_solution(game, row_support, col_support, k, tol,
                                   row_strategy, col_strategy, mat, rhs, out) != 0) {
          continue;
        }
        if (verify_equilibrium(game, row_strategy, col_strategy, tol,
                               &row_payoff, &col_payoff) != 0) {
          continue;
        }
        if (!equilibrium_exists(&equilibria, row_strategy, col_strategy, 1e-5)) {
          exprtk_value_t eq = make_equilibrium_map(row_strategy, col_strategy,
                                                   game->n_rows, game->n_cols,
                                                   row_payoff, col_payoff, arena);
          if (eq.type != EXPRTK_VAL_MAP) {
            exprtk_map_free(&equilibria);
            return exprtk_val_num(NAN);
          }
          exprtk_list_push(&equilibria, eq);
        }
      } while (next_combination(col_support, k, game->n_cols));
    } while (next_combination(row_support, k, game->n_rows));
  }

  {
    exprtk_value_t result = exprtk_val_map();
    exprtk_map_set(&result, "equilibria", equilibria);
    exprtk_map_set(&result, "count", exprtk_val_num((double)equilibria.data.list.count));
    return result;
  }
}

static double positive_shift(const double *data, size_t n) {
  double min_val = simd_min(data, n);
  return (min_val <= 0.0) ? (1.0 - min_val) : 0.0;
}

static int tableau_init(nash_tableau_t *tableau,
                        const double *payoffs,
                        size_t rows,
                        size_t cols,
                        int shifted,
                        int lexicographic,
                        mem_pool_t *arena) {
  size_t total_cols = rows + cols + 1;
  double shift = positive_shift(payoffs, rows * cols);

  memset(tableau, 0, sizeof(*tableau));
  tableau->data = MEM_ALLOC_ARRAY(arena, double, rows * total_cols);
  tableau->original_basic_labels = MEM_ALLOC_ARRAY(arena, size_t, cols);
  tableau->slack_labels = MEM_ALLOC_ARRAY(arena, size_t, rows);
  tableau->non_basic = MEM_ALLOC_ARRAY(arena, unsigned char, rows + cols);
  tableau->scratch_a = MEM_ALLOC_ARRAY(arena, double, total_cols);
  tableau->scratch_b = MEM_ALLOC_ARRAY(arena, double, total_cols);
  tableau->ratios = MEM_ALLOC_ARRAY(arena, double, rows);
  tableau->tie_rows = MEM_ALLOC_ARRAY(arena, size_t, rows);
  if (!tableau->data || !tableau->original_basic_labels || !tableau->slack_labels ||
      !tableau->non_basic || !tableau->scratch_a || !tableau->scratch_b ||
      !tableau->ratios || !tableau->tie_rows) {
    return -1;
  }

  tableau->rows = rows;
  tableau->cols = total_cols;
  tableau->label_count = rows + cols;
  tableau->original_basic_count = cols;
  tableau->slack_count = rows;
  tableau->lexicographic = lexicographic;
  memset(tableau->data, 0, rows * total_cols * sizeof(double));
  memset(tableau->non_basic, 0, (rows + cols) * sizeof(unsigned char));

  for (size_t r = 0; r < rows; r++) {
    double *row = tableau->data + r * total_cols;
    if (shifted) {
      row[r] = 1.0;
      for (size_t c = 0; c < cols; c++) row[rows + c] = payoffs[r * cols + c] + shift;
    } else {
      for (size_t c = 0; c < cols; c++) row[c] = payoffs[r * cols + c] + shift;
      row[cols + r] = 1.0;
    }
    row[total_cols - 1] = 1.0;
  }

  if (shifted) {
    for (size_t i = 0; i < cols; i++) {
      tableau->original_basic_labels[i] = rows + i;
      tableau->non_basic[rows + i] = 1;
    }
    for (size_t i = 0; i < rows; i++) tableau->slack_labels[i] = i;
  } else {
    for (size_t i = 0; i < cols; i++) {
      tableau->original_basic_labels[i] = i;
      tableau->non_basic[i] = 1;
    }
    for (size_t i = 0; i < rows; i++) tableau->slack_labels[i] = cols + i;
  }

  return 0;
}

static size_t tableau_extract_label_value(const nash_tableau_t *tableau, size_t label) {
  for (size_t r = 0; r < tableau->rows; r++) {
    const double *row = tableau->data + r * tableau->cols;
    if (fabs(row[label]) > 1e-12) return r;
  }
  return SIZE_MAX;
}

static void tableau_to_strategy(const nash_tableau_t *tableau,
                                const unsigned char *other_non_basic,
                                double *out) {
  memset(out, 0, tableau->original_basic_count * sizeof(double));
  for (size_t i = 0; i < tableau->original_basic_count; i++) {
    size_t label = tableau->original_basic_labels[i];
    if (!other_non_basic[label]) continue;
    {
      size_t row_idx = tableau_extract_label_value(tableau, label);
      if (row_idx != SIZE_MAX) {
        const double *row = tableau->data + row_idx * tableau->cols;
        out[i] = row[tableau->cols - 1] / row[label];
      }
    }
  }
  normalize_or_uniform(out, tableau->original_basic_count);
}

static size_t tableau_find_dropped_label(const nash_tableau_t *tableau,
                                         size_t pivot_row,
                                         const unsigned char *prev_non_basic) {
  const double *row = tableau->data + pivot_row * tableau->cols;
  for (size_t label = 0; label < tableau->label_count; label++) {
    if (prev_non_basic[label]) continue;
    if (fabs(row[label]) > 1e-12) return label;
  }
  return SIZE_MAX;
}

static int tableau_ratio_lex_greater(const double *lhs, const double *rhs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    double delta = lhs[i] - rhs[i];
    if (fabs(delta) <= 1e-12) continue;
    return delta > 0.0;
  }
  return 0;
}

static size_t tableau_choose_pivot_row(const nash_tableau_t *tableau, size_t entering_label) {
  double max_ratio = -INFINITY;
  size_t pivot_row = SIZE_MAX;
  size_t tied_count = 0;
  size_t *tied_rows = tableau->tie_rows;

  for (size_t r = 0; r < tableau->rows; r++) {
    const double *row = tableau->data + r * tableau->cols;
    double denom = row[tableau->cols - 1];
    double ratio = (fabs(denom) > 1e-12) ? (row[entering_label] / denom) : -INFINITY;
    tableau->ratios[r] = ratio;
    if (ratio > max_ratio + 1e-12) {
      max_ratio = ratio;
      pivot_row = r;
      tied_count = 0;
      tied_rows[tied_count++] = r;
    } else if (fabs(ratio - max_ratio) <= 1e-12) {
      tied_rows[tied_count++] = r;
    }
  }

  if (!tableau->lexicographic || tied_count <= 1) return pivot_row;

  {
    size_t best_row = tied_rows[0];
    double *lhs = tableau->scratch_a;
    double *rhs = tableau->scratch_b;
    for (size_t idx = 1; idx < tied_count; idx++) {
      size_t candidate = tied_rows[idx];
      const double *best = tableau->data + best_row * tableau->cols;
      const double *cand = tableau->data + candidate * tableau->cols;
      double best_pivot = best[entering_label];
      double cand_pivot = cand[entering_label];
      for (size_t s = 0; s < tableau->slack_count; s++) {
        size_t slack = tableau->slack_labels[s];
        lhs[s] = (fabs(cand_pivot) > 1e-12) ? (cand[slack] / cand_pivot) : -INFINITY;
        rhs[s] = (fabs(best_pivot) > 1e-12) ? (best[slack] / best_pivot) : -INFINITY;
      }
      if (tableau_ratio_lex_greater(lhs, rhs, tableau->slack_count)) best_row = candidate;
    }
    return best_row;
  }
}

static size_t tableau_pivot_and_drop(nash_tableau_t *tableau, size_t entering_label) {
  size_t pivot_row = tableau_choose_pivot_row(tableau, entering_label);
  if (pivot_row == SIZE_MAX) return SIZE_MAX;

  {
    unsigned char prev_flags_stack[256];
    unsigned char *prev_flags = (tableau->label_count <= 256)
                                    ? prev_flags_stack
                                    : (unsigned char *)malloc(tableau->label_count * sizeof(unsigned char));
    if (!prev_flags) return SIZE_MAX;
    memcpy(prev_flags, tableau->non_basic, tableau->label_count * sizeof(unsigned char));

    {
      const double *pivot = tableau->data + pivot_row * tableau->cols;
      double pivot_value = pivot[entering_label];
      if (fabs(pivot_value) <= 1e-12) {
        if (prev_flags != prev_flags_stack) free(prev_flags);
        return SIZE_MAX;
      }

      for (size_t r = 0; r < tableau->rows; r++) {
        double *row = tableau->data + r * tableau->cols;
        if (r == pivot_row) continue;
        {
          double row_value = row[entering_label];
          simd_scale(row, tableau->scratch_a, pivot_value, tableau->cols);
          simd_scale(pivot, tableau->scratch_b, row_value, tableau->cols);
          simd_sub(tableau->scratch_a, tableau->scratch_b, row, tableau->cols);
        }
      }

      {
        size_t dropped = tableau_find_dropped_label(tableau, pivot_row, prev_flags);
        if (prev_flags != prev_flags_stack) free(prev_flags);
        if (dropped == SIZE_MAX) return SIZE_MAX;
        tableau->non_basic[dropped] = 1;
        tableau->non_basic[entering_label] = 0;
        return dropped;
      }
    }
  }
}

static int tableau_all_labels_covered(const nash_tableau_t *lhs, const nash_tableau_t *rhs) {
  for (size_t label = 0; label < lhs->label_count; label++) {
    if (!lhs->non_basic[label] && !rhs->non_basic[label]) return 0;
  }
  return 1;
}

static int lemke_howson_tableau(const nash_game_t *game,
                                size_t label,
                                int lexicographic,
                                double *row_strategy,
                                double *col_strategy,
                                mem_pool_t *arena) {
  nash_tableau_t row_tableau;
  nash_tableau_t col_tableau;
  double *col_transpose = MEM_ALLOC_ARRAY(arena, double, game->n_rows * game->n_cols);
  nash_tableau_t *current;
  nash_tableau_t *other;
  size_t entering;
  size_t steps = 0;
  size_t max_steps = (game->n_rows + game->n_cols) * (game->n_rows + game->n_cols + 8);

  if (!col_transpose) return -1;
  exprtk_transpose(game->col_payoffs, game->n_rows, game->n_cols, col_transpose);

  if (tableau_init(&col_tableau, game->row_payoffs, game->n_rows, game->n_cols, 1, lexicographic, arena) != 0)
    return -1;
  if (tableau_init(&row_tableau, col_transpose, game->n_cols, game->n_rows, 0, lexicographic, arena) != 0)
    return -1;

  current = row_tableau.non_basic[label] ? &row_tableau : &col_tableau;
  other = (current == &row_tableau) ? &col_tableau : &row_tableau;
  entering = label;

  do {
    size_t dropped = tableau_pivot_and_drop(current, entering);
    if (dropped == SIZE_MAX) return -1;
    entering = dropped;
    {
      nash_tableau_t *tmp = current;
      current = other;
      other = tmp;
    }
  } while (!tableau_all_labels_covered(&row_tableau, &col_tableau) && steps++ < max_steps);

  if (!tableau_all_labels_covered(&row_tableau, &col_tableau)) return -1;
  tableau_to_strategy(&row_tableau, col_tableau.non_basic, row_strategy);
  tableau_to_strategy(&col_tableau, row_tableau.non_basic, col_strategy);
  return 0;
}

static int parse_square_matrix_arg(const exprtk_value_t *arg, size_t n, const double **out) {
  if (!arg || !out) return -1;
  if (arg->type != EXPRTK_VAL_VECTOR) return -1;
  if (arg->data.vector.size != n * n) return -1;
  *out = arg->data.vector.data;
  return 0;
}

static void replicator_derivative(const double *payoffs, const double *state, size_t n,
                                  const double *mutation_matrix, double *out,
                                  double *fitness, double *weighted) {
  matrix_vector_mul(payoffs, n, n, state, fitness);
  simd_mul(state, fitness, weighted, n);
  if (mutation_matrix) {
    vector_matrix_mul(weighted, mutation_matrix, n, n, out);
  } else {
    memcpy(out, weighted, n * sizeof(double));
  }
  {
    double phi = simd_dot(state, fitness, n);
    for (size_t i = 0; i < n; i++) out[i] -= state[i] * phi;
  }
}

static void asymmetric_replicator_derivative(const nash_game_t *game,
                                             const double *row_state,
                                             const double *col_state,
                                             double *row_out,
                                             double *col_out,
                                             double *row_fitness,
                                             double *col_fitness) {
  matrix_vector_mul(game->row_payoffs, game->n_rows, game->n_cols, col_state, row_fitness);
  vector_matrix_mul(row_state, game->col_payoffs, game->n_rows, game->n_cols, col_fitness);
  {
    double row_phi = simd_dot(row_fitness, row_state, game->n_rows);
    double col_phi = simd_dot(col_fitness, col_state, game->n_cols);
    for (size_t i = 0; i < game->n_rows; i++) row_out[i] = row_state[i] * (row_fitness[i] - row_phi);
    for (size_t j = 0; j < game->n_cols; j++) col_out[j] = col_state[j] * (col_fitness[j] - col_phi);
  }
}

static size_t pow_size(size_t base, size_t exp) {
  size_t out = 1;
  for (size_t i = 0; i < exp; i++) out *= base;
  return out;
}

static size_t repeated_state_count(size_t n_rows, size_t n_cols, size_t repetitions) {
  size_t mn = n_rows * n_cols;
  size_t total = 0;
  size_t term = 1;
  for (size_t i = 0; i < repetitions; i++) {
    total += term;
    term *= mn;
  }
  return total;
}

static size_t repeated_offset(size_t n_rows, size_t n_cols, size_t depth) {
  size_t total = 0;
  size_t term = 1;
  for (size_t i = 0; i < depth; i++) {
    total += term;
    term *= (n_rows * n_cols);
  }
  return total;
}

static void decode_strategy_index(size_t index, size_t base, size_t state_count, size_t *actions) {
  for (size_t i = 0; i < state_count; i++) {
    actions[i] = index % base;
    index /= base;
  }
}

static exprtk_value_t make_action_history_list(const size_t *history, size_t n, mem_pool_t *arena) {
  exprtk_value_t *items = MEM_ALLOC_ARRAY(arena, exprtk_value_t, n);
  if (!items && n > 0) return exprtk_val_num(NAN);
  for (size_t i = 0; i < n; i++) items[i] = exprtk_val_num((double)history[i]);
  return exprtk_val_list_ex(items, n, 0);
}

static exprtk_value_t make_population_list(const size_t *population, size_t n, mem_pool_t *arena) {
  return make_action_history_list(population, n, arena);
}

static int population_not_fixed(const size_t *population, size_t n) {
  if (n == 0) return 0;
  for (size_t i = 1; i < n; i++) {
    if (population[i] != population[0]) return 1;
  }
  return 0;
}

static void complete_graph_adjacency(size_t n, double *out) {
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) out[i * n + j] = (i == j) ? 0.0 : 1.0;
  }
}

static int moran_scores(const double *payoffs, size_t n_actions,
                        const size_t *population, size_t population_size,
                        const double *adjacency, double *scores) {
  if (simd_min(payoffs, n_actions * n_actions) < 0.0) return -1;
  for (size_t i = 0; i < population_size; i++) {
    double total = 0.0;
    for (size_t j = 0; j < population_size; j++) {
      if (adjacency[i * population_size + j] > 0.5) {
        total += payoffs[population[i] * n_actions + population[j]];
      }
    }
    scores[i] = total;
  }
  return 0;
}

static void moran_step(const double *scores, size_t population_size, nash_rng_t *rng,
                       const size_t *population, size_t *next_population,
                       const size_t *original_strategies, size_t original_count,
                       double mutation_probability, const double *replacement_matrix) {
  double *weights = (double *)scores;
  size_t birth_index = nash_rng_choice_weighted(rng, weights, population_size);
  size_t death_index;

  memcpy(next_population, population, population_size * sizeof(size_t));
  if (replacement_matrix) {
    death_index = nash_rng_choice_weighted(rng,
                                           replacement_matrix + birth_index * population_size,
                                           population_size);
  } else {
    death_index = nash_rng_choice_uniform(rng, population_size);
  }

  if (mutation_probability > 0.0 && nash_rng_next_uniform(rng) < mutation_probability && original_count > 0) {
    size_t pick = nash_rng_choice_uniform(rng, original_count);
    next_population[death_index] = original_strategies[pick];
  } else {
    next_population[death_index] = population[birth_index];
  }
}

static void greenwood_quantize(const double *counts, size_t n, size_t target_total,
                               double *rounded, double *errors, unsigned char *used) {
  double rounded_total = 0.0;
  for (size_t i = 0; i < n; i++) {
    rounded[i] = floor(counts[i] + 0.5);
    errors[i] = rounded[i] - counts[i];
    used[i] = 0;
    rounded_total += rounded[i];
  }

  {
    long adjust = (long)llround(rounded_total) - (long)target_total;
    while (adjust > 0) {
      size_t pick = SIZE_MAX;
      double best = -INFINITY;
      for (size_t i = 0; i < n; i++) {
        if (used[i]) continue;
        if (errors[i] > best) {
          best = errors[i];
          pick = i;
        }
      }
      if (pick == SIZE_MAX) break;
      rounded[pick] -= 1.0;
      used[pick] = 1;
      adjust--;
    }
    memset(used, 0, n * sizeof(unsigned char));
    while (adjust < 0) {
      size_t pick = SIZE_MAX;
      double best = INFINITY;
      for (size_t i = 0; i < n; i++) {
        if (used[i]) continue;
        if (errors[i] < best) {
          best = errors[i];
          pick = i;
        }
      }
      if (pick == SIZE_MAX) break;
      rounded[pick] += 1.0;
      used[pick] = 1;
      adjust++;
    }
  }
}

static void discrete_replicator_step_type_1(const double *payoffs, const double *state, size_t n,
                                            double *next, double *fitness) {
  double mean_payoff;
  matrix_vector_mul(payoffs, n, n, state, fitness);
  mean_payoff = simd_dot(fitness, state, n);
  for (size_t i = 0; i < n; i++) {
    next[i] = state[i] + state[i] * (fitness[i] - mean_payoff);
  }
}

static int discrete_replicator_step_type_2(const double *payoffs, const double *state, size_t n,
                                           double *next, double *fitness) {
  double mean_payoff;
  matrix_vector_mul(payoffs, n, n, state, fitness);
  mean_payoff = simd_dot(fitness, state, n);
  if (fabs(mean_payoff) <= 1e-15) return -1;
  for (size_t i = 0; i < n; i++) {
    next[i] = state[i] * (fitness[i] / mean_payoff);
  }
  return 0;
}

static void nash_rng_dirichlet_ones(nash_rng_t *rng, size_t n, double *out) {
  double total = 0.0;
  for (size_t i = 0; i < n; i++) {
    double u = nash_rng_next_uniform(rng);
    if (u <= 1e-12) u = 1e-12;
    out[i] = -log(u);
    total += out[i];
  }
  if (total <= 1e-15) {
    double uniform = 1.0 / (double)n;
    for (size_t i = 0; i < n; i++) out[i] = uniform;
    return;
  }
  for (size_t i = 0; i < n; i++) out[i] /= total;
}

static exprtk_value_t make_vector_copy(const double *values, size_t n, mem_pool_t *arena) {
  double *copy = MEM_ALLOC_ARRAY(arena, double, n);
  if (!copy && n > 0) return exprtk_val_num(NAN);
  if (n > 0) memcpy(copy, values, n * sizeof(double));
  return exprtk_val_vec(copy, n);
}

static void threshold_strategy(double *values, size_t n, double threshold) {
  for (size_t i = 0; i < n; i++) {
    values[i] = (values[i] >= threshold) ? 1.0 : 0.0;
  }
}

static void absolute_best_strategy(const double *strategy, size_t n, double *out) {
  double best = simd_max(strategy, n);
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    if (fabs(strategy[i] - best) <= 1e-12) count++;
  }
  if (count == 0) {
    for (size_t i = 0; i < n; i++) out[i] = 1.0 / (double)n;
    normalize_or_uniform(out, n);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    out[i] = (fabs(strategy[i] - best) <= 1e-12) ? (1.0 / (double)count) : 0.0;
  }
}

static size_t support_size(const exprtk_value_t *eq, const char *key, double tol) {
  exprtk_value_t vec = exprtk_map_get(eq, key);
  size_t count = 0;
  if (vec.type != EXPRTK_VAL_VECTOR) return 0;
  for (size_t i = 0; i < vec.data.vector.size; i++) {
    if (vec.data.vector.data[i] > tol) count++;
  }
  return count;
}

static int strategy_lex_less(const exprtk_value_t *lhs, const exprtk_value_t *rhs, const char *key) {
  exprtk_value_t lhs_vec = exprtk_map_get(lhs, key);
  exprtk_value_t rhs_vec = exprtk_map_get(rhs, key);
  if (lhs_vec.type != EXPRTK_VAL_VECTOR || rhs_vec.type != EXPRTK_VAL_VECTOR) return 0;
  for (size_t i = 0; i < lhs_vec.data.vector.size && i < rhs_vec.data.vector.size; i++) {
    double delta = lhs_vec.data.vector.data[i] - rhs_vec.data.vector.data[i];
    if (fabs(delta) <= 1e-12) continue;
    return delta < 0.0;
  }
  return lhs_vec.data.vector.size < rhs_vec.data.vector.size;
}

static int equilibrium_lex_less(const exprtk_value_t *lhs, const exprtk_value_t *rhs) {
  if (strategy_lex_less(lhs, rhs, "row_strategy")) return 1;
  if (strategy_lex_less(rhs, lhs, "row_strategy")) return 0;
  return strategy_lex_less(lhs, rhs, "col_strategy");
}

static size_t choose_lemke_howson_index(const exprtk_value_t *equilibria,
                                        size_t label,
                                        size_t num_labels) {
  size_t fallback_pure = SIZE_MAX;
  size_t fallback_mixed = SIZE_MAX;

  for (size_t i = 0; i < equilibria->data.list.count; i++) {
    exprtk_value_t eq = equilibria->data.list.items[i];
    size_t row_supp = support_size(&eq, "row_strategy", 1e-9);
    size_t col_supp = support_size(&eq, "col_strategy", 1e-9);
    if (row_supp == 1 && col_supp == 1) {
      if (fallback_pure == SIZE_MAX ||
          equilibrium_lex_less(&eq, &equilibria->data.list.items[fallback_pure])) {
        fallback_pure = i;
      }
    }
    if (row_supp > 1 || col_supp > 1) {
      if (fallback_mixed == SIZE_MAX ||
          equilibrium_lex_less(&eq, &equilibria->data.list.items[fallback_mixed])) {
        fallback_mixed = i;
      }
    }
  }

  if (equilibria->data.list.count == 0) return SIZE_MAX;
  if (equilibria->data.list.count == 1) return 0;

  /* Compatibility fallback until a tableau-based pivoter lands. */
  if (fallback_mixed != SIZE_MAX && (label == 1 || label + 1 == num_labels)) return fallback_mixed;
  if (fallback_pure != SIZE_MAX) return fallback_pure;
  if (fallback_mixed != SIZE_MAX) return fallback_mixed;
  return label % equilibria->data.list.count;
}

exprtk_value_t exprtk_nash_game(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;
  (void)arena;

  nash_game_t game;
  if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);

  double row_min = game.row_payoffs[0];
  double row_max = game.row_payoffs[0];
  double col_min = game.col_payoffs[0];
  double col_max = game.col_payoffs[0];
  int zero_sum = 1;
  int symmetric = (game.n_rows == game.n_cols);

  for (size_t i = 0; i < game.n_rows * game.n_cols; i++) {
    if (game.row_payoffs[i] < row_min) row_min = game.row_payoffs[i];
    if (game.row_payoffs[i] > row_max) row_max = game.row_payoffs[i];
    if (game.col_payoffs[i] < col_min) col_min = game.col_payoffs[i];
    if (game.col_payoffs[i] > col_max) col_max = game.col_payoffs[i];
    if (fabs(game.row_payoffs[i] + game.col_payoffs[i]) > 1e-9) zero_sum = 0;
  }

  if (symmetric) {
    for (size_t i = 0; i < game.n_rows && symmetric; i++) {
      for (size_t j = 0; j < game.n_cols; j++) {
        double a = game.row_payoffs[i * game.n_cols + j];
        double bt = game.col_payoffs[j * game.n_cols + i];
        if (fabs(a - bt) > 1e-9) {
          symmetric = 0;
          break;
        }
      }
    }
  }

  {
    exprtk_value_t out = exprtk_val_map();
    exprtk_map_set(&out, "rows", exprtk_val_num((double)game.n_rows));
    exprtk_map_set(&out, "cols", exprtk_val_num((double)game.n_cols));
    exprtk_map_set(&out, "zero_sum", exprtk_val_num((double)zero_sum));
    exprtk_map_set(&out, "symmetric", exprtk_val_num((double)symmetric));
    exprtk_map_set(&out, "row_min", exprtk_val_num(row_min));
    exprtk_map_set(&out, "row_max", exprtk_val_num(row_max));
    exprtk_map_set(&out, "col_min", exprtk_val_num(col_min));
    exprtk_map_set(&out, "col_max", exprtk_val_num(col_max));
    return out;
  }
}

exprtk_value_t exprtk_nash_support_enumeration(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  nash_game_t game;
  double tol = 1e-8;
  if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
  if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
    tol = args[4].data.number;
  }
  return enumerate_equilibria(&game, tol, arena);
}

exprtk_value_t exprtk_nash_vertex_enumeration(size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  nash_game_t game;
  double tol = 1e-8;
  if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
  if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
    tol = args[4].data.number;
  }
  return enumerate_equilibria(&game, tol, arena);
}

exprtk_value_t exprtk_nash_lemke_howson(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  nash_game_t game;
  size_t label = 0;
  size_t num_labels;
  double *row_strategy;
  double *col_strategy;

  if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
  if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number >= 0.0) {
    label = (size_t)args[4].data.number;
  }
  num_labels = game.n_rows + game.n_cols;
  row_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
  col_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
  if (!row_strategy || !col_strategy) return exprtk_val_num(NAN);

  if (lemke_howson_tableau(&game, label % num_labels, 1, row_strategy, col_strategy, arena) == 0) {
    exprtk_value_t eq = make_equilibrium_map(row_strategy, col_strategy,
                                             game.n_rows, game.n_cols,
                                             mixed_row_payoff(&game, row_strategy, col_strategy),
                                             mixed_col_payoff(&game, row_strategy, col_strategy),
                                             arena);
    exprtk_map_set(&eq, "label", exprtk_val_num((double)(label % num_labels)));
    exprtk_map_set(&eq, "engine", exprtk_val_str((vstr){ .data = "tableau", .len = 7 }));
    return eq;
  }

  {
    exprtk_value_t result = enumerate_equilibria(&game, 1e-8, arena);
    exprtk_value_t equilibria;
    size_t index;
    if (result.type != EXPRTK_VAL_MAP) return exprtk_val_num(NAN);
    equilibria = exprtk_map_get(&result, "equilibria");
    if (equilibria.type != EXPRTK_VAL_LIST || equilibria.data.list.count == 0) {
      exprtk_map_free(&result);
      return exprtk_val_num(NAN);
    }
    index = choose_lemke_howson_index(&equilibria, label % num_labels, num_labels);
    if (index == SIZE_MAX || index >= equilibria.data.list.count) {
      exprtk_map_free(&result);
      return exprtk_val_num(NAN);
    }
    {
      exprtk_value_t eq = clone_equilibrium_map(&equilibria.data.list.items[index], arena);
      exprtk_map_set(&eq, "label", exprtk_val_num((double)(label % num_labels)));
      exprtk_map_set(&eq, "engine", exprtk_val_str((vstr){ .data = "fallback", .len = 8 }));
      exprtk_map_free(&result);
      return eq;
    }
  }
}

exprtk_value_t exprtk_nash_fictitious_play(size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  nash_game_t game;
  if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);

  {
    size_t iterations = 1000;
    double *row_counts = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_counts = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    double *row_belief = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_belief = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    double *row_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_cols);

    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
      iterations = (size_t)args[4].data.number;
    }

    if (!row_counts || !col_counts || !row_belief || !col_belief ||
        !row_strategy || !col_strategy) {
      return exprtk_val_num(NAN);
    }

    memset(row_counts, 0, game.n_rows * sizeof(double));
    memset(col_counts, 0, game.n_cols * sizeof(double));

    for (size_t iter = 0; iter < iterations; iter++) {
      size_t row_action;
      size_t col_action;
      belief_from_counts(col_counts, game.n_cols, col_belief);
      row_action = best_response_row(&game, col_belief);
      row_counts[row_action] += 1.0;

      belief_from_counts(row_counts, game.n_rows, row_belief);
      col_action = best_response_col(&game, row_belief);
      col_counts[col_action] += 1.0;
    }

    belief_from_counts(row_counts, game.n_rows, row_strategy);
    belief_from_counts(col_counts, game.n_cols, col_strategy);

    {
      exprtk_value_t result = exprtk_val_map();
      exprtk_map_set(&result, "row_strategy", exprtk_val_vec(row_strategy, game.n_rows));
      exprtk_map_set(&result, "col_strategy", exprtk_val_vec(col_strategy, game.n_cols));
      exprtk_map_set(&result, "row_payoff",
                     exprtk_val_num(mixed_row_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&result, "col_payoff",
                     exprtk_val_num(mixed_col_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&result, "iterations", exprtk_val_num((double)iterations));
      return result;
    }
  }
}

exprtk_value_t exprtk_nash_stochastic_fictitious_play(size_t argc, exprtk_value_t *args,
                                                      exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  nash_game_t game;
  if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);

  {
    size_t iterations = 1000;
    double temperature = 0.1;
    uint64_t seed = 1;
    double *row_counts = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_counts = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    double *row_belief = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_belief = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    double *row_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    double *row_probs = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    double *col_probs = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    nash_rng_t rng;

    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
      iterations = (size_t)args[4].data.number;
    }
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number > 0.0) {
      temperature = args[5].data.number;
    }
    if (argc > 6 && args[6].type == EXPRTK_VAL_NUMBER && args[6].data.number >= 0.0) {
      seed = (uint64_t)args[6].data.number;
    }

    if (!row_counts || !col_counts || !row_belief || !col_belief ||
        !row_strategy || !col_strategy || !row_probs || !col_probs) {
      return exprtk_val_num(NAN);
    }

    memset(row_counts, 0, game.n_rows * sizeof(double));
    memset(col_counts, 0, game.n_cols * sizeof(double));
    nash_rng_seed(&rng, seed);

    for (size_t iter = 0; iter < iterations; iter++) {
      size_t row_action;
      size_t col_action;

      belief_from_counts(col_counts, game.n_cols, col_belief);
      stochastic_best_response_row(&game, col_belief, temperature, &rng, &row_action, row_probs);
      row_counts[row_action] += 1.0;

      belief_from_counts(row_counts, game.n_rows, row_belief);
      stochastic_best_response_col(&game, row_belief, temperature, &rng, &col_action, col_probs);
      col_counts[col_action] += 1.0;
    }

    belief_from_counts(row_counts, game.n_rows, row_strategy);
    belief_from_counts(col_counts, game.n_cols, col_strategy);

    {
      exprtk_value_t result = exprtk_val_map();
      exprtk_map_set(&result, "row_strategy", exprtk_val_vec(row_strategy, game.n_rows));
      exprtk_map_set(&result, "col_strategy", exprtk_val_vec(col_strategy, game.n_cols));
      exprtk_map_set(&result, "row_payoff",
                     exprtk_val_num(mixed_row_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&result, "col_payoff",
                     exprtk_val_num(mixed_col_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&result, "iterations", exprtk_val_num((double)iterations));
      exprtk_map_set(&result, "temperature", exprtk_val_num(temperature));
      exprtk_map_set(&result, "seed", exprtk_val_num((double)seed));
      return result;
    }
  }
}

exprtk_value_t exprtk_nash_discrete_replicator_dynamics(size_t argc, exprtk_value_t *args,
                                                        exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  if (argc < 3) return exprtk_val_num(NAN);
  if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER ||
      args[2].type != EXPRTK_VAL_VECTOR) {
    return exprtk_val_num(NAN);
  }

  {
    size_t n = (size_t)args[1].data.number;
    size_t steps = 1;
    int quantize = 0;
    size_t step_type = 2;
    double total_population;
    size_t quantized_total;
    const double *payoffs = args[0].data.vector.data;
    const double *initial = args[2].data.vector.data;
    double *state;
    double *next;
    double *fitness;
    double *scaled;
    double *rounded;
    double *errors;
    unsigned char *used;
    exprtk_value_t history = exprtk_val_list_empty();

    if (n == 0 || args[0].data.vector.size != n * n || args[2].data.vector.size != n) {
      return exprtk_val_num(NAN);
    }
    if (argc > 3 && args[3].type == EXPRTK_VAL_NUMBER && args[3].data.number > 0.0) {
      steps = (size_t)args[3].data.number;
    }
    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER) {
      quantize = (args[4].data.number != 0.0);
    }
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number > 0.0) {
      step_type = (size_t)args[5].data.number;
    }

    total_population = simd_sum(initial, n);
    if (total_population <= 0.0) return exprtk_val_num(NAN);
    quantized_total = (size_t)llround(total_population);
    if (quantized_total == 0) return exprtk_val_num(NAN);

    state = MEM_ALLOC_ARRAY(arena, double, n);
    next = MEM_ALLOC_ARRAY(arena, double, n);
    fitness = MEM_ALLOC_ARRAY(arena, double, n);
    scaled = MEM_ALLOC_ARRAY(arena, double, n);
    rounded = MEM_ALLOC_ARRAY(arena, double, n);
    errors = MEM_ALLOC_ARRAY(arena, double, n);
    used = MEM_ALLOC_ARRAY(arena, unsigned char, n);
    if (!state || !next || !fitness || !scaled || !rounded || !errors || !used) {
      return exprtk_val_num(NAN);
    }

    simd_scale(initial, state, 1.0 / total_population, n);
    for (size_t step = 0; step < steps; step++) {
      int rc;
      if (step_type == 1) {
        discrete_replicator_step_type_1(payoffs, state, n, next, fitness);
        rc = 0;
      } else {
        rc = discrete_replicator_step_type_2(payoffs, state, n, next, fitness);
      }
      if (rc != 0) return exprtk_val_num(NAN);

      if (quantize) {
        simd_scale(next, scaled, (double)quantized_total, n);
        greenwood_quantize(scaled, n, quantized_total, rounded, errors, used);
        simd_scale(rounded, state, 1.0 / (double)quantized_total, n);
      } else {
        memcpy(state, next, n * sizeof(double));
      }

      {
        double *snapshot = MEM_ALLOC_ARRAY(arena, double, n);
        if (!snapshot) return exprtk_val_num(NAN);
        simd_scale(state, snapshot, total_population, n);
        exprtk_list_push(&history, exprtk_val_vec(snapshot, n));
      }
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "history", history);
      exprtk_map_set(&out, "population", history.data.list.count ? history.data.list.items[history.data.list.count - 1]
                                                                 : make_vector_copy(initial, n, arena));
      exprtk_map_set(&out, "steps", exprtk_val_num((double)steps));
      exprtk_map_set(&out, "quantized", exprtk_val_num((double)quantize));
      exprtk_map_set(&out, "step_type", exprtk_val_num((double)step_type));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_imitation_dynamics(size_t argc, exprtk_value_t *args,
                                              exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    size_t population_size = 100;
    size_t iterations = 1000;
    uint64_t seed = 1;
    double threshold = 0.5;
    double *population_a;
    double *population_b;
    double *payoffs_a;
    double *payoffs_b;
    double *raw_row;
    double *raw_col;
    double *row_strategy;
    double *col_strategy;
    nash_rng_t rng;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
      population_size = (size_t)args[4].data.number;
    }
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number > 0.0) {
      iterations = (size_t)args[5].data.number;
    }
    if (argc > 6 && args[6].type == EXPRTK_VAL_NUMBER && args[6].data.number >= 0.0) {
      seed = (uint64_t)args[6].data.number;
    }
    if (argc > 7 && args[7].type == EXPRTK_VAL_NUMBER) {
      threshold = args[7].data.number;
    }

    population_a = MEM_ALLOC_ARRAY(arena, double, population_size * game.n_rows);
    population_b = MEM_ALLOC_ARRAY(arena, double, population_size * game.n_cols);
    payoffs_a = MEM_ALLOC_ARRAY(arena, double, population_size);
    payoffs_b = MEM_ALLOC_ARRAY(arena, double, population_size);
    raw_row = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    raw_col = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    if (!population_a || !population_b || !payoffs_a || !payoffs_b ||
        !raw_row || !raw_col ||
        !row_strategy || !col_strategy) {
      return exprtk_val_num(NAN);
    }

    nash_rng_seed(&rng, seed);
    for (size_t i = 0; i < population_size; i++) {
      nash_rng_dirichlet_ones(&rng, game.n_rows, population_a + i * game.n_rows);
      nash_rng_dirichlet_ones(&rng, game.n_cols, population_b + i * game.n_cols);
    }

    for (size_t generation = 0; generation < iterations; generation++) {
      size_t best_a = 0;
      size_t best_b = 0;
      for (size_t i = 0; i < population_size; i++) {
        const double *strategy_a = population_a + i * game.n_rows;
        const double *strategy_b = population_b + i * game.n_cols;
        payoffs_a[i] = mixed_row_payoff(&game, strategy_a, strategy_b);
        payoffs_b[i] = mixed_col_payoff(&game, strategy_a, strategy_b);
        if (payoffs_a[i] > payoffs_a[best_a]) best_a = i;
        if (payoffs_b[i] > payoffs_b[best_b]) best_b = i;
      }

      for (size_t i = 0; i < population_size; i++) {
        memcpy(population_a + i * game.n_rows,
               population_a + best_a * game.n_rows,
               game.n_rows * sizeof(double));
        memcpy(population_b + i * game.n_cols,
               population_b + best_b * game.n_cols,
               game.n_cols * sizeof(double));
      }
    }

    simd_fill(raw_row, 0.0, game.n_rows);
    simd_fill(raw_col, 0.0, game.n_cols);
    for (size_t i = 0; i < population_size; i++) {
      for (size_t j = 0; j < game.n_rows; j++) raw_row[j] += population_a[i * game.n_rows + j];
      for (size_t j = 0; j < game.n_cols; j++) raw_col[j] += population_b[i * game.n_cols + j];
    }
    simd_scale(raw_row, raw_row, 1.0 / (double)population_size, game.n_rows);
    simd_scale(raw_col, raw_col, 1.0 / (double)population_size, game.n_cols);

    memcpy(row_strategy, raw_row, game.n_rows * sizeof(double));
    memcpy(col_strategy, raw_col, game.n_cols * sizeof(double));
    threshold_strategy(row_strategy, game.n_rows, threshold);
    threshold_strategy(col_strategy, game.n_cols, threshold);

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "row_strategy", exprtk_val_vec(row_strategy, game.n_rows));
      exprtk_map_set(&out, "col_strategy", exprtk_val_vec(col_strategy, game.n_cols));
      exprtk_map_set(&out, "raw_row_strategy", exprtk_val_vec(raw_row, game.n_rows));
      exprtk_map_set(&out, "raw_col_strategy", exprtk_val_vec(raw_col, game.n_cols));
      exprtk_map_set(&out, "row_payoff", exprtk_val_num(mixed_row_payoff(&game, raw_row, raw_col)));
      exprtk_map_set(&out, "col_payoff", exprtk_val_num(mixed_col_payoff(&game, raw_row, raw_col)));
      exprtk_map_set(&out, "population_size", exprtk_val_num((double)population_size));
      exprtk_map_set(&out, "iterations", exprtk_val_num((double)iterations));
      exprtk_map_set(&out, "seed", exprtk_val_num((double)seed));
      exprtk_map_set(&out, "threshold", exprtk_val_num(threshold));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_regret_minimization(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    double learning_rate = 0.1;
    size_t iterations = 100;
    double *row_strategy;
    double *col_strategy;
    double *raw_row;
    double *raw_col;
    double *row_utilities;
    double *col_utilities;
    double *row_regrets;
    double *col_regrets;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
      learning_rate = args[4].data.number;
    }
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number > 0.0) {
      iterations = (size_t)args[5].data.number;
    }

    row_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    raw_row = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    raw_col = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_utilities = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_utilities = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_regrets = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_regrets = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    if (!row_strategy || !col_strategy || !raw_row || !raw_col ||
        !row_utilities || !col_utilities || !row_regrets || !col_regrets) {
      return exprtk_val_num(NAN);
    }

    for (size_t i = 0; i < game.n_rows; i++) raw_row[i] = 1.0 / (double)game.n_rows;
    for (size_t j = 0; j < game.n_cols; j++) raw_col[j] = 1.0 / (double)game.n_cols;

    for (size_t iter = 0; iter < iterations; iter++) {
      matrix_vector_mul(game.row_payoffs, game.n_rows, game.n_cols, raw_col, row_utilities);
      vector_matrix_mul(raw_row, game.col_payoffs, game.n_rows, game.n_cols, col_utilities);

      for (size_t i = 0; i < game.n_rows; i++) {
        row_regrets[i] = row_utilities[i] - raw_row[i];
        if (row_regrets[i] < 0.0) row_regrets[i] = 0.0;
        row_strategy[i] = raw_row[i] + learning_rate * row_regrets[i];
      }
      normalize_or_uniform(row_strategy, game.n_rows);

      for (size_t j = 0; j < game.n_cols; j++) {
        col_regrets[j] = col_utilities[j] - raw_col[j];
        if (col_regrets[j] < 0.0) col_regrets[j] = 0.0;
        col_strategy[j] = raw_col[j] + learning_rate * col_regrets[j];
      }
      normalize_or_uniform(col_strategy, game.n_cols);

      memcpy(raw_row, row_strategy, game.n_rows * sizeof(double));
      memcpy(raw_col, col_strategy, game.n_cols * sizeof(double));
    }

    absolute_best_strategy(raw_row, game.n_rows, row_strategy);
    absolute_best_strategy(raw_col, game.n_cols, col_strategy);

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "row_strategy", exprtk_val_vec(row_strategy, game.n_rows));
      exprtk_map_set(&out, "col_strategy", exprtk_val_vec(col_strategy, game.n_cols));
      exprtk_map_set(&out, "raw_row_strategy", exprtk_val_vec(raw_row, game.n_rows));
      exprtk_map_set(&out, "raw_col_strategy", exprtk_val_vec(raw_col, game.n_cols));
      exprtk_map_set(&out, "row_payoff", exprtk_val_num(mixed_row_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&out, "col_payoff", exprtk_val_num(mixed_col_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&out, "learning_rate", exprtk_val_num(learning_rate));
      exprtk_map_set(&out, "iterations", exprtk_val_num((double)iterations));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_introspection_dynamics(size_t argc, exprtk_value_t *args,
                                                  exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    size_t iterations;
    double beta;
    uint64_t seed = 1;
    size_t row_action;
    size_t col_action;
    exprtk_value_t history = exprtk_val_list_empty();
    nash_rng_t rng;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc < 6) return exprtk_val_num(NAN);
    if (args[4].type != EXPRTK_VAL_NUMBER || args[5].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(NAN);
    iterations = (size_t)args[4].data.number;
    beta = args[5].data.number;
    if (argc > 6 && args[6].type == EXPRTK_VAL_NUMBER && args[6].data.number >= 0.0) {
      seed = (uint64_t)args[6].data.number;
    }

    nash_rng_seed(&rng, seed);
    if (argc > 7) {
      if (args[7].type != EXPRTK_VAL_VECTOR || args[7].data.vector.size != 2) return exprtk_val_num(NAN);
      row_action = (size_t)args[7].data.vector.data[0];
      col_action = (size_t)args[7].data.vector.data[1];
    } else {
      row_action = nash_rng_choice_uniform(&rng, game.n_rows);
      col_action = nash_rng_choice_uniform(&rng, game.n_cols);
    }

    for (size_t step = 0; step <= iterations; step++) {
      exprtk_value_t *items = MEM_ALLOC_ARRAY(arena, exprtk_value_t, 2);
      if (!items) return exprtk_val_num(NAN);
      items[0] = exprtk_val_num((double)row_action);
      items[1] = exprtk_val_num((double)col_action);
      exprtk_list_push(&history, exprtk_val_list_ex(items, 2, 0));

      if (step == iterations) break;

      {
        size_t player = nash_rng_choice_uniform(&rng, 2);
        size_t current_action = (player == 0) ? row_action : col_action;
        size_t action_space = (player == 0) ? game.n_rows : game.n_cols;
        size_t candidate;
        double current_score;
        double candidate_score;
        double delta;
        double probability;

        if (action_space <= 1) continue;
        candidate = nash_rng_choice_uniform(&rng, action_space - 1);
        if (candidate >= current_action) candidate++;
        current_score = (player == 0)
                            ? game.row_payoffs[row_action * game.n_cols + col_action]
                            : game.col_payoffs[row_action * game.n_cols + col_action];
        candidate_score = (player == 0)
                              ? game.row_payoffs[candidate * game.n_cols + col_action]
                              : game.col_payoffs[row_action * game.n_cols + candidate];
        delta = candidate_score - current_score;
        probability = 1.0 / (1.0 + exp(clamp_exp_arg(-beta * delta)));
        if (nash_rng_next_uniform(&rng) < probability) {
          if (player == 0) row_action = candidate;
          else col_action = candidate;
        }
      }
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "history", history);
      exprtk_map_set(&out, "steps", exprtk_val_num((double)history.data.list.count));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_repeated_game(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    size_t repetitions;
    size_t state_count;
    size_t row_strategy_count;
    size_t col_strategy_count;
    double *row_matrix;
    double *col_matrix;
    size_t *row_actions;
    size_t *col_actions;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc < 5 || args[4].type != EXPRTK_VAL_NUMBER) return exprtk_val_num(NAN);
    repetitions = (size_t)args[4].data.number;
    state_count = repeated_state_count(game.n_rows, game.n_cols, repetitions);
    row_strategy_count = pow_size(game.n_rows, state_count);
    col_strategy_count = pow_size(game.n_cols, state_count);
    if (row_strategy_count == 0 || col_strategy_count == 0) return exprtk_val_num(NAN);
    if (row_strategy_count > 4096 || col_strategy_count > 4096) return exprtk_val_num(NAN);

    row_matrix = MEM_ALLOC_ARRAY(arena, double, row_strategy_count * col_strategy_count);
    col_matrix = MEM_ALLOC_ARRAY(arena, double, row_strategy_count * col_strategy_count);
    row_actions = MEM_ALLOC_ARRAY(arena, size_t, state_count);
    col_actions = MEM_ALLOC_ARRAY(arena, size_t, state_count);
    if (!row_matrix || !col_matrix || !row_actions || !col_actions) return exprtk_val_num(NAN);

    for (size_t r = 0; r < row_strategy_count; r++) {
      decode_strategy_index(r, game.n_rows, state_count, row_actions);
      for (size_t c = 0; c < col_strategy_count; c++) {
        double row_payoff = 0.0;
        double col_payoff = 0.0;
        size_t row_code = 0;
        size_t col_code = 0;
        size_t state_idx = 0;
        decode_strategy_index(c, game.n_cols, state_count, col_actions);

        for (size_t depth = 0; depth < repetitions; depth++) {
          size_t row_action = row_actions[state_idx];
          size_t col_action = col_actions[state_idx];
          row_payoff += game.row_payoffs[row_action * game.n_cols + col_action];
          col_payoff += game.col_payoffs[row_action * game.n_cols + col_action];
          row_code = row_code * game.n_rows + row_action;
          col_code = col_code * game.n_cols + col_action;
          if (depth + 1 < repetitions) {
            state_idx = repeated_offset(game.n_rows, game.n_cols, depth + 1) +
                        row_code * pow_size(game.n_cols, depth + 1) + col_code;
          }
        }

        row_matrix[r * col_strategy_count + c] = row_payoff;
        col_matrix[r * col_strategy_count + c] = col_payoff;
      }
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "row_payoffs", exprtk_val_vec(row_matrix, row_strategy_count * col_strategy_count));
      exprtk_map_set(&out, "col_payoffs", exprtk_val_vec(col_matrix, row_strategy_count * col_strategy_count));
      exprtk_map_set(&out, "rows", exprtk_val_num((double)row_strategy_count));
      exprtk_map_set(&out, "cols", exprtk_val_num((double)col_strategy_count));
      exprtk_map_set(&out, "states", exprtk_val_num((double)state_count));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_moran_process(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  if (argc < 4) return exprtk_val_num(NAN);
  if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER ||
      args[2].type != EXPRTK_VAL_LIST || args[3].type != EXPRTK_VAL_NUMBER) {
    return exprtk_val_num(NAN);
  }

  {
    size_t n_actions = (size_t)args[1].data.number;
    size_t population_size = args[2].data.list.count;
    size_t max_steps = (size_t)args[3].data.number;
    double mutation_probability = 0.0;
    uint64_t seed = 1;
    const double *adjacency = NULL;
    const double *replacement = NULL;
    size_t *population;
    size_t *next_population;
    size_t *original_strategies;
    size_t original_count = 0;
    double *scores;
    double *default_adjacency;
    exprtk_value_t history = exprtk_val_list_empty();
    nash_rng_t rng;

    if (n_actions == 0 || args[0].data.vector.size != n_actions * n_actions) return exprtk_val_num(NAN);
    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number >= 0.0) {
      mutation_probability = args[4].data.number;
    }
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number >= 0.0) {
      seed = (uint64_t)args[5].data.number;
    }
    if (argc > 6 && args[6].type == EXPRTK_VAL_VECTOR &&
        args[6].data.vector.size == population_size * population_size) {
      replacement = args[6].data.vector.data;
    }
    if (argc > 7 && args[7].type == EXPRTK_VAL_VECTOR &&
        args[7].data.vector.size == population_size * population_size) {
      adjacency = args[7].data.vector.data;
    }

    population = MEM_ALLOC_ARRAY(arena, size_t, population_size);
    next_population = MEM_ALLOC_ARRAY(arena, size_t, population_size);
    original_strategies = MEM_ALLOC_ARRAY(arena, size_t, population_size);
    scores = MEM_ALLOC_ARRAY(arena, double, population_size);
    default_adjacency = MEM_ALLOC_ARRAY(arena, double, population_size * population_size);
    if (!population || !next_population || !original_strategies || !scores || !default_adjacency) {
      return exprtk_val_num(NAN);
    }

    for (size_t i = 0; i < population_size; i++) population[i] = (size_t)args[2].data.list.items[i].data.number;
    for (size_t i = 0; i < population_size; i++) {
      int seen = 0;
      for (size_t j = 0; j < original_count; j++) {
        if (original_strategies[j] == population[i]) {
          seen = 1;
          break;
        }
      }
      if (!seen) original_strategies[original_count++] = population[i];
    }

    if (!adjacency) {
      complete_graph_adjacency(population_size, default_adjacency);
      adjacency = default_adjacency;
    }

    nash_rng_seed(&rng, seed);
    for (size_t step = 0; step <= max_steps; step++) {
      exprtk_list_push(&history, make_population_list(population, population_size, arena));
      if (step == max_steps) break;
      if (mutation_probability <= 0.0 && !population_not_fixed(population, population_size)) break;
      if (moran_scores(args[0].data.vector.data, n_actions, population, population_size, adjacency, scores) != 0) {
        return exprtk_val_num(NAN);
      }
      moran_step(scores, population_size, &rng, population, next_population,
                 original_strategies, original_count, mutation_probability, replacement);
      memcpy(population, next_population, population_size * sizeof(size_t));
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "history", history);
      exprtk_map_set(&out, "steps", exprtk_val_num((double)history.data.list.count));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_replicator_mutation(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  if (argc < 2) return exprtk_val_num(NAN);
  if (args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_NUMBER) {
    return exprtk_val_num(NAN);
  }

  {
    size_t n = (size_t)args[1].data.number;
    size_t steps = 1000;
    double dt = 0.01;
    const double *mutation_matrix = NULL;
    const double *initial = NULL;
    double *state;
    double *derivative;
    double *fitness;
    double *weighted;

    if (n == 0 || args[0].data.vector.size != n * n) return exprtk_val_num(NAN);
    if (argc > 2 && args[2].type == EXPRTK_VAL_NUMBER && args[2].data.number > 0.0) {
      steps = (size_t)args[2].data.number;
    }
    if (argc > 3 && args[3].type == EXPRTK_VAL_NUMBER && args[3].data.number > 0.0) {
      dt = args[3].data.number;
    }
    if (argc > 4 && parse_square_matrix_arg(&args[4], n, &mutation_matrix) != 0) {
      mutation_matrix = NULL;
    }
    if (argc > 5) {
      if (args[5].type != EXPRTK_VAL_VECTOR || args[5].data.vector.size != n) return exprtk_val_num(NAN);
      initial = args[5].data.vector.data;
    }

    state = MEM_ALLOC_ARRAY(arena, double, n);
    derivative = MEM_ALLOC_ARRAY(arena, double, n);
    fitness = MEM_ALLOC_ARRAY(arena, double, n);
    weighted = MEM_ALLOC_ARRAY(arena, double, n);
    if (!state || !derivative || !fitness || !weighted) return exprtk_val_num(NAN);

    if (initial) memcpy(state, initial, n * sizeof(double));
    else for (size_t i = 0; i < n; i++) state[i] = 1.0 / (double)n;
    normalize_or_uniform(state, n);

    for (size_t step = 0; step < steps; step++) {
      replicator_derivative(args[0].data.vector.data, state, n, mutation_matrix,
                            derivative, fitness, weighted);
      for (size_t i = 0; i < n; i++) state[i] += dt * derivative[i];
      normalize_or_uniform(state, n);
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "strategy", exprtk_val_vec(state, n));
      exprtk_map_set(&out, "payoff", exprtk_val_num(simd_dot(state, fitness, n)));
      exprtk_map_set(&out, "steps", exprtk_val_num((double)steps));
      exprtk_map_set(&out, "dt", exprtk_val_num(dt));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_asymmetric_replicator(size_t argc, exprtk_value_t *args,
                                                 exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    size_t steps = 1000;
    double dt = 0.01;
    const double *initial_row = NULL;
    const double *initial_col = NULL;
    double *row_state;
    double *col_state;
    double *row_derivative;
    double *col_derivative;
    double *row_fitness;
    double *col_fitness;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0) {
      steps = (size_t)args[4].data.number;
    }
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number > 0.0) {
      dt = args[5].data.number;
    }
    if (argc > 6) {
      if (args[6].type != EXPRTK_VAL_VECTOR || args[6].data.vector.size != game.n_rows) return exprtk_val_num(NAN);
      initial_row = args[6].data.vector.data;
    }
    if (argc > 7) {
      if (args[7].type != EXPRTK_VAL_VECTOR || args[7].data.vector.size != game.n_cols) return exprtk_val_num(NAN);
      initial_col = args[7].data.vector.data;
    }

    row_state = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_state = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_derivative = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_derivative = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_fitness = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_fitness = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    if (!row_state || !col_state || !row_derivative || !col_derivative ||
        !row_fitness || !col_fitness) {
      return exprtk_val_num(NAN);
    }

    if (initial_row) memcpy(row_state, initial_row, game.n_rows * sizeof(double));
    else for (size_t i = 0; i < game.n_rows; i++) row_state[i] = 1.0 / (double)game.n_rows;
    if (initial_col) memcpy(col_state, initial_col, game.n_cols * sizeof(double));
    else for (size_t j = 0; j < game.n_cols; j++) col_state[j] = 1.0 / (double)game.n_cols;
    normalize_or_uniform(row_state, game.n_rows);
    normalize_or_uniform(col_state, game.n_cols);

    for (size_t step = 0; step < steps; step++) {
      asymmetric_replicator_derivative(&game, row_state, col_state,
                                       row_derivative, col_derivative,
                                       row_fitness, col_fitness);
      for (size_t i = 0; i < game.n_rows; i++) row_state[i] += dt * row_derivative[i];
      for (size_t j = 0; j < game.n_cols; j++) col_state[j] += dt * col_derivative[j];
      normalize_or_uniform(row_state, game.n_rows);
      normalize_or_uniform(col_state, game.n_cols);
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "row_strategy", exprtk_val_vec(row_state, game.n_rows));
      exprtk_map_set(&out, "col_strategy", exprtk_val_vec(col_state, game.n_cols));
      exprtk_map_set(&out, "row_payoff", exprtk_val_num(mixed_row_payoff(&game, row_state, col_state)));
      exprtk_map_set(&out, "col_payoff", exprtk_val_num(mixed_col_payoff(&game, row_state, col_state)));
      exprtk_map_set(&out, "steps", exprtk_val_num((double)steps));
      exprtk_map_set(&out, "dt", exprtk_val_num(dt));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_asymmetric_replicator_derivative(size_t argc, exprtk_value_t *args,
                                                            exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    double *row_derivative;
    double *col_derivative;
    double *row_fitness;
    double *col_fitness;
    const double *row_state;
    const double *col_state;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc < 6) return exprtk_val_num(NAN);
    if (args[4].type != EXPRTK_VAL_VECTOR || args[4].data.vector.size != game.n_rows) return exprtk_val_num(NAN);
    if (args[5].type != EXPRTK_VAL_VECTOR || args[5].data.vector.size != game.n_cols) return exprtk_val_num(NAN);
    row_state = args[4].data.vector.data;
    col_state = args[5].data.vector.data;

    row_derivative = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_derivative = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_fitness = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_fitness = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    if (!row_derivative || !col_derivative || !row_fitness || !col_fitness) {
      return exprtk_val_num(NAN);
    }

    asymmetric_replicator_derivative(&game, row_state, col_state,
                                     row_derivative, col_derivative,
                                     row_fitness, col_fitness);
    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "row_derivative", exprtk_val_vec(row_derivative, game.n_rows));
      exprtk_map_set(&out, "col_derivative", exprtk_val_vec(col_derivative, game.n_cols));
      return out;
    }
  }
}

exprtk_value_t exprtk_nash_replicator(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;

  {
    nash_game_t game;
    size_t max_iters = 1000;
    double lr = 0.05;
    double tol = 1e-6;
    double *row_strategy;
    double *col_strategy;
    double *row_prev;
    double *col_prev;
    double *row_rewards;
    double *col_rewards;
    size_t iter = 0;
    int converged = 0;

    if (parse_game_args(argc, args, &game) != 0) return exprtk_val_num(NAN);
    if (argc > 4 && args[4].type == EXPRTK_VAL_NUMBER && args[4].data.number > 0.0)
      max_iters = (size_t)args[4].data.number;
    if (argc > 5 && args[5].type == EXPRTK_VAL_NUMBER && args[5].data.number > 0.0)
      lr = args[5].data.number;
    if (argc > 6 && args[6].type == EXPRTK_VAL_NUMBER && args[6].data.number > 0.0)
      tol = args[6].data.number;

    row_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_strategy = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_prev = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_prev = MEM_ALLOC_ARRAY(arena, double, game.n_cols);
    row_rewards = MEM_ALLOC_ARRAY(arena, double, game.n_rows);
    col_rewards = MEM_ALLOC_ARRAY(arena, double, game.n_cols);

    if (!row_strategy || !col_strategy || !row_prev || !col_prev ||
        !row_rewards || !col_rewards) {
      return exprtk_val_num(NAN);
    }

    for (size_t i = 0; i < game.n_rows; i++) row_strategy[i] = 1.0 / (double)game.n_rows;
    for (size_t j = 0; j < game.n_cols; j++) col_strategy[j] = 1.0 / (double)game.n_cols;

    for (; iter < max_iters; iter++) {
      double row_mean = 0.0;
      double col_mean = 0.0;

      for (size_t i = 0; i < game.n_rows; i++) {
        row_rewards[i] = row_expected_payoff(&game, i, col_strategy);
        row_mean += row_strategy[i] * row_rewards[i];
      }
      for (size_t j = 0; j < game.n_cols; j++) {
        col_rewards[j] = col_expected_payoff(&game, j, row_strategy);
        col_mean += col_strategy[j] * col_rewards[j];
      }

      memcpy(row_prev, row_strategy, game.n_rows * sizeof(double));
      memcpy(col_prev, col_strategy, game.n_cols * sizeof(double));

      {
        double row_norm = 0.0;
        for (size_t i = 0; i < game.n_rows; i++) {
          double delta = clamp_exp_arg(lr * (row_rewards[i] - row_mean));
          row_strategy[i] *= exp(delta);
          row_norm += row_strategy[i];
        }
        if (row_norm <= 0.0) row_norm = 1e-15;
        for (size_t i = 0; i < game.n_rows; i++) row_strategy[i] /= row_norm;
      }

      {
        double col_norm = 0.0;
        for (size_t j = 0; j < game.n_cols; j++) {
          double delta = clamp_exp_arg(lr * (col_rewards[j] - col_mean));
          col_strategy[j] *= exp(delta);
          col_norm += col_strategy[j];
        }
        if (col_norm <= 0.0) col_norm = 1e-15;
        for (size_t j = 0; j < game.n_cols; j++) col_strategy[j] /= col_norm;
      }

      {
        double diff = 0.0;
        for (size_t i = 0; i < game.n_rows; i++) diff += fabs(row_strategy[i] - row_prev[i]);
        for (size_t j = 0; j < game.n_cols; j++) diff += fabs(col_strategy[j] - col_prev[j]);
        if (diff <= tol) {
          converged = 1;
          break;
        }
      }
    }

    {
      exprtk_value_t out = exprtk_val_map();
      exprtk_map_set(&out, "row_strategy", exprtk_val_vec(row_strategy, game.n_rows));
      exprtk_map_set(&out, "col_strategy", exprtk_val_vec(col_strategy, game.n_cols));
      exprtk_map_set(&out, "row_payoff",
                     exprtk_val_num(mixed_row_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&out, "col_payoff",
                     exprtk_val_num(mixed_col_payoff(&game, row_strategy, col_strategy)));
      exprtk_map_set(&out, "iterations", exprtk_val_num((double)(iter + 1)));
      exprtk_map_set(&out, "converged", exprtk_val_num((double)converged));
      return out;
    }
  }
}
