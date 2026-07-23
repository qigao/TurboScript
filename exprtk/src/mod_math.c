/**
 * @file math.c
 * @brief Core Math implementation and TurboScript Math Module.
 * Consolidates pure math functions and ExprTk module registration.
 */

#include "exprtk_module.h"
#include "simd_helpers.h"
#include "exprtk_internal.h"
#include "linalg.h"
#include "turbo_buffer.h"
#include <math.h>
#include <limits.h>
#include <simde/x86/avx2.h>
#include <simde/x86/fma.h>
#include <stdlib.h>
#include <string.h>

static const double TS_MATH_PI = 3.141592653589793238462643383279502884;

static int math_is_number(exprtk_value_t v) {
  return v.type == EXPRTK_VAL_NUMBER || v.type == EXPRTK_VAL_INTEGER;
}

static double math_number_value(exprtk_value_t v) {
  return v.type == EXPRTK_VAL_INTEGER ? (double)v.data.integer : v.data.number;
}

typedef enum {
  MATH_MATRIX_COL_MAJOR = 0,
  MATH_MATRIX_ROW_MAJOR = 1
} math_matrix_layout_t;

typedef enum {
  MATH_MATRIX_NORM_FROBENIUS = 0,
  MATH_MATRIX_NORM_L1 = 1,
  MATH_MATRIX_NORM_INF = 2
} math_matrix_norm_kind_t;

typedef enum {
  MATH_MATRIX_AXIS_COL = 0,
  MATH_MATRIX_AXIS_ROW = 1
} math_matrix_axis_t;

static int math_string_equals(exprtk_value_t v, const char *s) {
  size_t len = strlen(s);
  return v.type == EXPRTK_VAL_STRING && v.data.string.len == len &&
         memcmp(v.data.string.data, s, len) == 0;
}

static int math_matrix_layout_arg(exprtk_value_t v, math_matrix_layout_t *layout) {
  if (math_is_number(v)) {
    *layout = fabs(math_number_value(v)) > 1e-9 ? MATH_MATRIX_ROW_MAJOR : MATH_MATRIX_COL_MAJOR;
    return 1;
  }
  if (math_string_equals(v, "column-major") || math_string_equals(v, "ColMajor")) {
    *layout = MATH_MATRIX_COL_MAJOR;
    return 1;
  }
  if (math_string_equals(v, "row-major") || math_string_equals(v, "RowMajor")) {
    *layout = MATH_MATRIX_ROW_MAJOR;
    return 1;
  }
  return 0;
}

static int math_matrix_axis_arg(exprtk_value_t v, math_matrix_axis_t *axis) {
  if (math_string_equals(v, "col") || math_string_equals(v, "cols") ||
      math_string_equals(v, "column") || math_string_equals(v, "columns") ||
      math_string_equals(v, "0")) {
    *axis = MATH_MATRIX_AXIS_COL;
    return 1;
  }
  if (math_string_equals(v, "row") || math_string_equals(v, "rows") ||
      math_string_equals(v, "1")) {
    *axis = MATH_MATRIX_AXIS_ROW;
    return 1;
  }
  if (math_is_number(v)) {
    double n = math_number_value(v);
    if (fabs(n) < 1e-9) {
      *axis = MATH_MATRIX_AXIS_COL;
      return 1;
    }
    if (fabs(n - 1.0) < 1e-9) {
      *axis = MATH_MATRIX_AXIS_ROW;
      return 1;
    }
  }
  return 0;
}

static int math_matrix_norm_kind_arg(exprtk_value_t v, math_matrix_norm_kind_t *kind) {
  if (math_string_equals(v, "fro") || math_string_equals(v, "frob") ||
      math_string_equals(v, "frobenius")) {
    *kind = MATH_MATRIX_NORM_FROBENIUS;
    return 1;
  }
  if (math_string_equals(v, "l1") || math_string_equals(v, "one") ||
      math_string_equals(v, "1")) {
    *kind = MATH_MATRIX_NORM_L1;
    return 1;
  }
  if (math_string_equals(v, "inf") || math_string_equals(v, "infinity")) {
    *kind = MATH_MATRIX_NORM_INF;
    return 1;
  }
  if (math_is_number(v)) {
    double n = math_number_value(v);
    if (fabs(n - 1.0) < 1e-9) {
      *kind = MATH_MATRIX_NORM_L1;
      return 1;
    }
    if (fabs(n - 2.0) < 1e-9) {
      *kind = MATH_MATRIX_NORM_FROBENIUS;
      return 1;
    }
  }
  return 0;
}

static void math_double_to_float_matrix(const double *src, size_t rows, size_t cols,
                                        math_matrix_layout_t layout, float *dst) {
  if (layout == MATH_MATRIX_COL_MAJOR) {
    for (size_t i = 0; i < rows * cols; ++i) dst[i] = (float)src[i];
    return;
  }
  for (size_t r = 0; r < rows; ++r)
    for (size_t c = 0; c < cols; ++c)
      dst[r + c * rows] = (float)src[r * cols + c];
}

static void math_float_to_double_matrix(const float *src, size_t rows, size_t cols,
                                        math_matrix_layout_t layout, double *dst) {
  if (layout == MATH_MATRIX_COL_MAJOR) {
    for (size_t i = 0; i < rows * cols; ++i) dst[i] = (double)src[i];
    return;
  }
  for (size_t r = 0; r < rows; ++r)
    for (size_t c = 0; c < cols; ++c)
      dst[r * cols + c] = (double)src[r + c * rows];
}

static void math_double_to_row_major_matrix(const double *src, size_t rows, size_t cols,
                                            math_matrix_layout_t layout, double *dst) {
  if (layout == MATH_MATRIX_ROW_MAJOR) {
    memcpy(dst, src, rows * cols * sizeof(double));
    return;
  }
  for (size_t r = 0; r < rows; ++r)
    for (size_t c = 0; c < cols; ++c)
      dst[r * cols + c] = src[r + c * rows];
}

static void math_row_major_to_double_matrix(const double *src, size_t rows, size_t cols,
                                            math_matrix_layout_t layout, double *dst) {
  if (layout == MATH_MATRIX_ROW_MAJOR) {
    memcpy(dst, src, rows * cols * sizeof(double));
    return;
  }
  for (size_t r = 0; r < rows; ++r)
    for (size_t c = 0; c < cols; ++c)
      dst[r + c * rows] = src[r * cols + c];
}

static exprtk_value_t math_vec_from_float_matrix(mem_pool_t *arena, const float *src,
                                                 size_t rows, size_t cols,
                                                 math_matrix_layout_t layout) {
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  math_float_to_double_matrix(src, rows, cols, layout, out);
  return exprtk_val_vec(out, rows * cols);
}

static exprtk_value_t math_matmul_backend(const double *lhs, const double *rhs, size_t rows,
                                          size_t inner, size_t cols, float alpha, float beta,
                                          math_matrix_layout_t layout, mem_pool_t *arena) {
  if (!lhs || !rhs || !arena || rows == 0 || inner == 0 || cols == 0 ||
      rows > (size_t)INT_MAX || inner > (size_t)INT_MAX || cols > (size_t)INT_MAX)
    return exprtk_val_num(0);

  float *a = (float *)mem_alloc(arena, rows * inner * sizeof(float));
  float *b = (float *)mem_alloc(arena, inner * cols * sizeof(float));
  float *c = (float *)mem_alloc(arena, rows * cols * sizeof(float));
  if (!a || !b || !c) return exprtk_val_num(0);

  math_double_to_float_matrix(lhs, rows, inner, layout, a);
  math_double_to_float_matrix(rhs, inner, cols, layout, b);
  memset(c, 0, rows * cols * sizeof(float));
  matmul("N", "N", (int)rows, (int)cols, (int)inner, alpha, a, b, beta, c);
  return math_vec_from_float_matrix(arena, c, rows, cols, layout);
}

static int math_matrix_dims_from_args(size_t argc, exprtk_value_t *args, size_t min_argc,
                                      size_t *rows, size_t *cols,
                                      math_matrix_layout_t *layout) {
  if ((argc != min_argc && argc != min_argc + 1) ||
      args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]) ||
      !math_is_number(args[2]))
    return 0;
  *layout = MATH_MATRIX_COL_MAJOR;
  if (argc == min_argc + 1 && !math_matrix_layout_arg(args[min_argc], layout)) return 0;
  *rows = (size_t)math_number_value(args[1]);
  *cols = (size_t)math_number_value(args[2]);
  return *rows > 0 && *cols > 0 && args[0].data.vector.size >= (*rows) * (*cols);
}

static double math_matrix_get(const double *data, size_t rows, size_t cols,
                              math_matrix_layout_t layout, size_t row, size_t col) {
  size_t index = layout == MATH_MATRIX_ROW_MAJOR ? row * cols + col : row + col * rows;
  return data[index];
}

static void math_swap_size_t(size_t *a, size_t *b) {
  size_t tmp = *a;
  *a = *b;
  *b = tmp;
}

static int math_lu_decompose_row_major(double *a, size_t n, size_t *pivots, int *swap_count) {
  if (!a || !pivots || n == 0) return 0;
  for (size_t i = 0; i < n; ++i) pivots[i] = i;
  if (swap_count) *swap_count = 0;

  for (size_t k = 0; k < n; ++k) {
    size_t pivot = k;
    double pivot_abs = fabs(a[k * n + k]);
    for (size_t r = k + 1; r < n; ++r) {
      double v = fabs(a[r * n + k]);
      if (v > pivot_abs) {
        pivot = r;
        pivot_abs = v;
      }
    }
    if (pivot_abs < 1e-12) return 0;

    if (pivot != k) {
      for (size_t c = 0; c < n; ++c) {
        double tmp = a[k * n + c];
        a[k * n + c] = a[pivot * n + c];
        a[pivot * n + c] = tmp;
      }
      math_swap_size_t(&pivots[k], &pivots[pivot]);
      if (swap_count) ++(*swap_count);
    }

    double pivot_value = a[k * n + k];
    for (size_t r = k + 1; r < n; ++r) {
      a[r * n + k] /= pivot_value;
      double factor = a[r * n + k];
      for (size_t c = k + 1; c < n; ++c)
        a[r * n + c] -= factor * a[k * n + c];
    }
  }
  return 1;
}

static void math_sort_desc(double *values, size_t n) {
  for (size_t i = 1; i < n; ++i) {
    double v = values[i];
    size_t j = i;
    while (j > 0 && values[j - 1] < v) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = v;
  }
}

static void math_sort_asc(double *values, size_t n) {
  for (size_t i = 1; i < n; ++i) {
    double v = values[i];
    size_t j = i;
    while (j > 0 && values[j - 1] > v) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = v;
  }
}

static int math_symmetric_eigenvalues_jacobi(double *a, size_t n, double *eigenvalues) {
  if (!a || !eigenvalues || n == 0) return 0;
  const size_t max_iter = n * n * 64;
  for (size_t iter = 0; iter < max_iter; ++iter) {
    size_t p = 0;
    size_t q = n > 1 ? 1 : 0;
    double max_offdiag = 0.0;
    for (size_t i = 0; i < n; ++i) {
      for (size_t j = i + 1; j < n; ++j) {
        double v = fabs(a[i * n + j]);
        if (v > max_offdiag) {
          max_offdiag = v;
          p = i;
          q = j;
        }
      }
    }
    if (max_offdiag < 1e-12) break;

    double app = a[p * n + p];
    double aqq = a[q * n + q];
    double apq = a[p * n + q];
    double theta = 0.5 * atan2(2.0 * apq, aqq - app);
    double c = cos(theta);
    double s = sin(theta);

    for (size_t k = 0; k < n; ++k) {
      if (k == p || k == q) continue;
      double akp = a[k * n + p];
      double akq = a[k * n + q];
      double np = c * akp - s * akq;
      double nq = s * akp + c * akq;
      a[k * n + p] = np;
      a[p * n + k] = np;
      a[k * n + q] = nq;
      a[q * n + k] = nq;
    }

    double c2 = c * c;
    double s2 = s * s;
    double cs = c * s;
    a[p * n + p] = c2 * app - 2.0 * cs * apq + s2 * aqq;
    a[q * n + q] = s2 * app + 2.0 * cs * apq + c2 * aqq;
    a[p * n + q] = 0.0;
    a[q * n + p] = 0.0;
  }

  for (size_t i = 0; i < n; ++i) eigenvalues[i] = a[i * n + i];
  return 1;
}

/* ========================================================================= */
/* 2. TurboScript Module Wrappers                                           */
/* ========================================================================= */

/* ========================================================================= */
/* 2. TurboScript Module Wrappers                                           */
/* ========================================================================= */

static exprtk_value_t fn_integrate(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  if ((argc == 3 || argc == 4) && args[0].type == EXPRTK_VAL_STRING) {
    char f_name[256];
    size_t len = args[0].data.string.len;
    if (len > 255)
      len = 255;
    memcpy(f_name, args[0].data.string.data, len);
    f_name[len] = '\0';

    double a = args[1].data.number;
    double b = args[2].data.number;
    int n = (argc == 4) ? (int)args[3].data.number : 1000;
    if (n <= 0)
      n = 1000;
    if (n % 2 != 0)
      n++;

    double h = (b - a) / n;
    double sum = 0;

    exprtk_value_t vx_a = exprtk_val_num(a);
    exprtk_value_t vx_b = exprtk_val_num(b);
    exprtk_value_t v_a = exprtk_call_internal(f_name, 1, &vx_a, env);
    exprtk_value_t v_b = exprtk_call_internal(f_name, 1, &vx_b, env);
    sum = v_a.data.number + v_b.data.number;

    for (int i = 1; i < n; ++i) {
      if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL))
        break;
      double x = a + i * h;
      exprtk_value_t vx_val = exprtk_val_num(x);
      exprtk_value_t v_x = exprtk_call_internal(f_name, 1, &vx_val, env);
      sum += (i % 2 == 0 ? 2.0 : 4.0) * v_x.data.number;
    }
    return exprtk_val_num((h / 3.0) * sum);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_derivative(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_STRING) {
    char f_name[256];
    size_t len = args[0].data.string.len;
    if (len > 255)
      len = 255;
    memcpy(f_name, args[0].data.string.data, len);
    f_name[len] = '\0';

    double x = args[1].data.number;
    double h = (argc == 3) ? args[2].data.number : 1e-6;

    exprtk_value_t v1_arg = exprtk_val_num(x + h);
    exprtk_value_t v2_arg = exprtk_val_num(x - h);
    exprtk_value_t v1 = exprtk_call_internal(f_name, 1, &v1_arg, env);
    exprtk_value_t v2 = exprtk_call_internal(f_name, 1, &v2_arg, env);

    return exprtk_val_num((v1.data.number - v2.data.number) / (2.0 * h));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_det2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4)
    return exprtk_val_num(exprtk_det2(args[0].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_det3(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 9)
    return exprtk_val_num(exprtk_det3(args[0].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4) {
    double *res = MEM_ALLOC_ARRAY(arena, double, 4);
    if (res && exprtk_inv2(args[0].data.vector.data, res))
      return exprtk_val_vec(res, 4);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv3(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 9) {
    double *res = MEM_ALLOC_ARRAY(arena, double, 9);
    if (res && exprtk_inv3(args[0].data.vector.data, res))
      return exprtk_val_vec(res, 9);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_matmul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  if ((argc != 5 && argc != 6) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[2]) || !math_is_number(args[3]) || !math_is_number(args[4]))
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 6 && !math_matrix_layout_arg(args[5], &layout)) return exprtk_val_num(0);

  size_t rows = (size_t)math_number_value(args[2]);
  size_t inner = (size_t)math_number_value(args[3]);
  size_t cols = (size_t)math_number_value(args[4]);
  if (args[0].data.vector.size < rows * inner || args[1].data.vector.size < inner * cols)
    return exprtk_val_num(0);

  return math_matmul_backend(args[0].data.vector.data, args[1].data.vector.data, rows, inner,
                             cols, 1.0f, 0.0f, layout, arena);
}

static exprtk_value_t fn_mat_eye(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if ((argc != 1 && argc != 2) || !math_is_number(args[0])) return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 2 && !math_matrix_layout_arg(args[1], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[0]);
  if (n == 0 || n > (size_t)INT_MAX) return exprtk_val_num(0);
  float *tmp = (float *)mem_alloc(arena, n * n * sizeof(float));
  if (!tmp) return exprtk_val_num(0);
  mateye(tmp, (int)n);
  return math_vec_from_float_matrix(arena, tmp, n, n, layout);
}

static exprtk_value_t fn_mat_zeros(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || !math_is_number(args[0]) || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  (void)layout;

  size_t rows = (size_t)math_number_value(args[0]);
  size_t cols = (size_t)math_number_value(args[1]);
  if (rows == 0 || cols == 0 || rows > (size_t)INT_MAX || cols > (size_t)INT_MAX)
    return exprtk_val_num(0);

  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  memset(out, 0, rows * cols * sizeof(double));
  return exprtk_val_vec(out, rows * cols);
}

static exprtk_value_t fn_eig2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena);

static exprtk_value_t fn_mat_full(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if ((argc != 3 && argc != 4) || !math_is_number(args[0]) || !math_is_number(args[1]) ||
      !math_is_number(args[2]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 4 && !math_matrix_layout_arg(args[3], &layout)) return exprtk_val_num(0);
  (void)layout;

  size_t rows = (size_t)math_number_value(args[0]);
  size_t cols = (size_t)math_number_value(args[1]);
  if (rows == 0 || cols == 0 || rows > (size_t)INT_MAX || cols > (size_t)INT_MAX)
    return exprtk_val_num(0);

  double value = math_number_value(args[2]);
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  for (size_t i = 0; i < rows * cols; ++i)
    out[i] = value;
  return exprtk_val_vec(out, rows * cols);
}

static exprtk_value_t fn_mat_ones(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  if (argc != 2 && argc != 3) return exprtk_val_num(0);
  exprtk_value_t full_args[4];
  full_args[0] = args[0];
  full_args[1] = args[1];
  full_args[2] = exprtk_val_num(1.0);
  if (argc == 3) full_args[3] = args[2];
  return fn_mat_full(argc + 1, full_args, env, arena);
}

typedef enum {
  MATH_MATRIX_EWISE_ADD = 0,
  MATH_MATRIX_EWISE_SUB = 1,
  MATH_MATRIX_EWISE_MUL = 2
} math_matrix_ewise_op_t;

static exprtk_value_t math_mat_ewise_binary(size_t argc, exprtk_value_t *args,
                                            mem_pool_t *arena,
                                            math_matrix_ewise_op_t op) {
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  (void)layout;

  size_t rows = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  if (rows == 0 || cols == 0 || rows > (size_t)INT_MAX || cols > (size_t)INT_MAX ||
      args[0].data.vector.size < rows * cols || args[1].data.vector.size < rows * cols)
    return exprtk_val_num(0);

  size_t n = rows * cols;
  double *out = MEM_ALLOC_ARRAY(arena, double, n);
  if (!out) return exprtk_val_num(0);
  double *a = args[0].data.vector.data;
  double *b = args[1].data.vector.data;
  if (op == MATH_MATRIX_EWISE_ADD) {
    simd_add(a, b, out, n);
  } else if (op == MATH_MATRIX_EWISE_SUB) {
    simd_sub(a, b, out, n);
  } else {
    simd_mul(a, b, out, n);
  }
  return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_mat_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_mat_ewise_binary(argc, args, arena, MATH_MATRIX_EWISE_ADD);
}

static exprtk_value_t fn_mat_sub(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_mat_ewise_binary(argc, args, arena, MATH_MATRIX_EWISE_SUB);
}

static exprtk_value_t fn_mat_hadamard(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  return math_mat_ewise_binary(argc, args, arena, MATH_MATRIX_EWISE_MUL);
}

static exprtk_value_t fn_mat_scale(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]) || !math_is_number(args[3]))
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  (void)layout;

  size_t rows = (size_t)math_number_value(args[1]);
  size_t cols = (size_t)math_number_value(args[2]);
  if (rows == 0 || cols == 0 || rows > (size_t)INT_MAX || cols > (size_t)INT_MAX ||
      args[0].data.vector.size < rows * cols)
    return exprtk_val_num(0);

  size_t n = rows * cols;
  double *out = MEM_ALLOC_ARRAY(arena, double, n);
  if (!out) return exprtk_val_num(0);
  simd_scale(args[0].data.vector.data, out, math_number_value(args[3]), n);
  return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_mat_gemm(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if ((argc != 5 && argc != 6 && argc != 7 && argc != 8) ||
      args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]) || !math_is_number(args[4]))
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  float alpha = 1.0f;
  float beta = 0.0f;
  if (argc == 6) {
    if (!math_matrix_layout_arg(args[5], &layout)) return exprtk_val_num(0);
  } else if (argc == 7 || argc == 8) {
    if (!math_is_number(args[5]) || !math_is_number(args[6])) return exprtk_val_num(0);
    alpha = (float)math_number_value(args[5]);
    beta = (float)math_number_value(args[6]);
    if (argc == 8 && !math_matrix_layout_arg(args[7], &layout)) return exprtk_val_num(0);
  }

  size_t rows = (size_t)math_number_value(args[2]);
  size_t inner = (size_t)math_number_value(args[3]);
  size_t cols = (size_t)math_number_value(args[4]);
  if (rows == 0 || inner == 0 || cols == 0 || rows > (size_t)INT_MAX ||
      inner > (size_t)INT_MAX || cols > (size_t)INT_MAX)
    return exprtk_val_num(0);
  if (args[0].data.vector.size < rows * inner || args[1].data.vector.size < inner * cols)
    return exprtk_val_num(0);

  return math_matmul_backend(args[0].data.vector.data, args[1].data.vector.data, rows, inner,
                             cols, alpha, beta, layout, arena);
}

static exprtk_value_t fn_mat_cholesky(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || n > (size_t)INT_MAX || args[0].data.vector.size < n * n)
    return exprtk_val_num(0);
  float *a = (float *)mem_alloc(arena, n * n * sizeof(float));
  if (!a) return exprtk_val_num(0);
  math_double_to_float_matrix(args[0].data.vector.data, n, n, layout, a);
  if (cholesky(a, (int)n, 0) != 0) return exprtk_val_num(0);
  return math_vec_from_float_matrix(arena, a, n, n, layout);
}

static exprtk_value_t fn_mat_solve_tri(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5 && argc != 6) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  int transpose = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5) {
    if (!math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  } else if (argc == 6) {
    if (!math_is_number(args[4]) || !math_matrix_layout_arg(args[5], &layout))
      return exprtk_val_num(0);
    transpose = fabs(math_number_value(args[4])) > 1e-9;
  }
  if (n == 0 || cols == 0 || n > (size_t)INT_MAX || cols > (size_t)INT_MAX ||
      args[0].data.vector.size < n * n || args[1].data.vector.size < n * cols)
    return exprtk_val_num(0);
  float *l = (float *)mem_alloc(arena, n * n * sizeof(float));
  float *b = (float *)mem_alloc(arena, n * cols * sizeof(float));
  if (!l || !b) return exprtk_val_num(0);
  math_double_to_float_matrix(args[0].data.vector.data, n, n, layout, l);
  math_double_to_float_matrix(args[1].data.vector.data, n, cols, layout, b);
  trisolve(l, b, (int)n, (int)cols, transpose ? "T" : "N");
  return math_vec_from_float_matrix(arena, b, n, cols, layout);
}

static exprtk_value_t fn_mat_udu(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || n > (size_t)INT_MAX || args[0].data.vector.size < n * n)
    return exprtk_val_num(0);
  float *a = (float *)mem_alloc(arena, n * n * sizeof(float));
  float *u = (float *)mem_alloc(arena, n * n * sizeof(float));
  float *d = (float *)mem_alloc(arena, n * sizeof(float));
  if (!a || !u || !d) return exprtk_val_num(0);
  math_double_to_float_matrix(args[0].data.vector.data, n, n, layout, a);
  if (udu(a, u, d, (int)n) != 0) return exprtk_val_num(0);

  exprtk_value_t u_vec = math_vec_from_float_matrix(arena, u, n, n, layout);
  if (u_vec.type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0);
  double *d_out = MEM_ALLOC_ARRAY(arena, double, n);
  if (!d_out) return exprtk_val_num(0);
  for (size_t i = 0; i < n; ++i) d_out[i] = (double)d[i];
  exprtk_value_t d_vec = exprtk_val_vec(d_out, n);

  exprtk_value_t list = exprtk_val_list_empty();
  exprtk_list_push(&list, u_vec);
  exprtk_list_push(&list, d_vec);
  return list;
}

static exprtk_value_t fn_mat_shape(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  (void)layout;
  double *shape = MEM_ALLOC_ARRAY(arena, double, 2);
  if (!shape) return exprtk_val_num(0);
  shape[0] = (double)rows;
  shape[1] = (double)cols;
  return exprtk_val_vec(shape, 2);
}

static exprtk_value_t fn_mat_info(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);

  exprtk_value_t info = exprtk_val_map();
  exprtk_map_set(&info, "rows", exprtk_val_num((double)rows));
  exprtk_map_set(&info, "cols", exprtk_val_num((double)cols));
  exprtk_map_set(&info, "size", exprtk_val_num((double)(rows * cols)));
  exprtk_map_set(&info, "layout", exprtk_val_num((double)layout));
  exprtk_map_set(&info, "order",
                 exprtk_val_str(tstr_v_from_cstr(layout == MATH_MATRIX_ROW_MAJOR ? "row" : "col")));
  exprtk_map_set(&info, "dtype", exprtk_val_str(tstr_v_from_cstr("f64")));
  exprtk_map_set(&info, "backend_dtype", exprtk_val_str(tstr_v_from_cstr("f32")));
  return info;
}

static exprtk_value_t fn_mat_rows(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  (void)cols;
  (void)layout;
  return exprtk_val_num((double)rows);
}

static exprtk_value_t fn_mat_cols(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  (void)rows;
  (void)layout;
  return exprtk_val_num((double)cols);
}

static exprtk_value_t fn_mat_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || args[0].data.vector.size < n * n) return exprtk_val_num(0);
  double *row = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *out = MEM_ALLOC_ARRAY(arena, double, n * n);
  if (!row || !out) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, row);
  if (!gauss_jordan_invert(row, n, arena)) return exprtk_val_num(0);
  math_row_major_to_double_matrix(row, n, n, layout, out);
  return exprtk_val_vec(out, n * n);
}

static exprtk_value_t fn_mat_solve(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  if (n == 0 || cols == 0 || args[0].data.vector.size < n * n ||
      args[1].data.vector.size < n * cols)
    return exprtk_val_num(0);

  double *a = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *b = MEM_ALLOC_ARRAY(arena, double, n * cols);
  double *x_row = MEM_ALLOC_ARRAY(arena, double, n * cols);
  double *out = MEM_ALLOC_ARRAY(arena, double, n * cols);
  if (!a || !b || !x_row || !out) return exprtk_val_num(0);

  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, a);
  math_double_to_row_major_matrix(args[1].data.vector.data, n, cols, layout, b);
  if (!gauss_jordan_invert(a, n, arena)) return exprtk_val_num(0);

  for (size_t r = 0; r < n; ++r) {
    for (size_t c = 0; c < cols; ++c) {
      double sum = 0.0;
      for (size_t k = 0; k < n; ++k)
        sum += a[r * n + k] * b[k * cols + c];
      x_row[r * cols + c] = sum;
    }
  }
  math_row_major_to_double_matrix(x_row, n, cols, layout, out);
  return exprtk_val_vec(out, n * cols);
}

static exprtk_value_t fn_mat_det(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || n > (size_t)INT_MAX || args[0].data.vector.size < n * n)
    return exprtk_val_num(0);

  double *a = MEM_ALLOC_ARRAY(arena, double, n * n);
  if (!a) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, a);

  double det = 1.0;
  int sign = 1;
  for (size_t i = 0; i < n; ++i) {
    size_t pivot = i;
    double pivot_abs = fabs(a[i * n + i]);
    for (size_t r = i + 1; r < n; ++r) {
      double v = fabs(a[r * n + i]);
      if (v > pivot_abs) {
        pivot = r;
        pivot_abs = v;
      }
    }
    if (pivot_abs < 1e-12) return exprtk_val_num(0);

    if (pivot != i) {
      for (size_t c = 0; c < n; ++c) {
        double tmp = a[i * n + c];
        a[i * n + c] = a[pivot * n + c];
        a[pivot * n + c] = tmp;
      }
      sign = -sign;
    }

    double pivot_value = a[i * n + i];
    det *= pivot_value;
    for (size_t r = i + 1; r < n; ++r) {
      double factor = a[r * n + i] / pivot_value;
      a[r * n + i] = 0.0;
      for (size_t c = i + 1; c < n; ++c)
        a[r * n + c] -= factor * a[i * n + c];
    }
  }

  return exprtk_val_num(sign < 0 ? -det : det);
}

static exprtk_value_t fn_mat_norm(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if ((argc < 3 || argc > 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]))
    return exprtk_val_num(0);

  size_t rows = (size_t)math_number_value(args[1]);
  size_t cols = (size_t)math_number_value(args[2]);
  if (rows == 0 || cols == 0 || rows > (size_t)INT_MAX || cols > (size_t)INT_MAX ||
      args[0].data.vector.size < rows * cols)
    return exprtk_val_num(0);

  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  math_matrix_norm_kind_t kind = MATH_MATRIX_NORM_FROBENIUS;
  if (argc == 4) {
    if (args[3].type == EXPRTK_VAL_STRING) {
      if (!math_matrix_norm_kind_arg(args[3], &kind)) return exprtk_val_num(0);
    } else if (!math_matrix_layout_arg(args[3], &layout)) {
      return exprtk_val_num(0);
    }
  } else if (argc == 5) {
    if (!math_matrix_norm_kind_arg(args[3], &kind) || !math_matrix_layout_arg(args[4], &layout))
      return exprtk_val_num(0);
  }

  if (kind == MATH_MATRIX_NORM_FROBENIUS) {
    double sum_sq = 0.0;
    for (size_t i = 0; i < rows * cols; ++i)
      sum_sq += args[0].data.vector.data[i] * args[0].data.vector.data[i];
    return exprtk_val_num(sqrt(sum_sq));
  }

  double *a = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!a) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, rows, cols, layout, a);

  double best = 0.0;
  if (kind == MATH_MATRIX_NORM_L1) {
    for (size_t c = 0; c < cols; ++c) {
      double sum = 0.0;
      for (size_t r = 0; r < rows; ++r)
        sum += fabs(a[r * cols + c]);
      if (sum > best) best = sum;
    }
  } else {
    for (size_t r = 0; r < rows; ++r) {
      double sum = 0.0;
      for (size_t c = 0; c < cols; ++c)
        sum += fabs(a[r * cols + c]);
      if (sum > best) best = sum;
    }
  }
  return exprtk_val_num(best);
}

static exprtk_value_t fn_mat_trace(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  (void)arena;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);

  size_t n = rows < cols ? rows : cols;
  double trace = 0.0;
  for (size_t i = 0; i < n; ++i) {
    size_t index = layout == MATH_MATRIX_ROW_MAJOR ? i * cols + i : i + i * rows;
    trace += args[0].data.vector.data[index];
  }
  return exprtk_val_num(trace);
}

static exprtk_value_t fn_mat_diag(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);

  size_t n = rows < cols ? rows : cols;
  double *diag = MEM_ALLOC_ARRAY(arena, double, n);
  if (!diag) return exprtk_val_num(0);
  for (size_t i = 0; i < n; ++i) {
    size_t index = layout == MATH_MATRIX_ROW_MAJOR ? i * cols + i : i + i * rows;
    diag[i] = args[0].data.vector.data[index];
  }
  return exprtk_val_vec(diag, n);
}

static exprtk_value_t fn_mat_transpose(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);

  double *src_row = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  double *dst_row = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!src_row || !dst_row || !out) return exprtk_val_num(0);

  math_double_to_row_major_matrix(args[0].data.vector.data, rows, cols, layout, src_row);
  for (size_t r = 0; r < rows; ++r)
    for (size_t c = 0; c < cols; ++c)
      dst_row[c * rows + r] = src_row[r * cols + c];
  math_row_major_to_double_matrix(dst_row, cols, rows, layout, out);
  return exprtk_val_vec(out, rows * cols);
}

typedef enum {
  MATH_MATRIX_REDUCE_SUM = 0,
  MATH_MATRIX_REDUCE_MEAN = 1,
  MATH_MATRIX_REDUCE_MIN = 2,
  MATH_MATRIX_REDUCE_MAX = 3
} math_matrix_reduce_op_t;

static int math_matrix_reduce_args(size_t argc, exprtk_value_t *args, size_t *rows,
                                   size_t *cols, math_matrix_layout_t *layout,
                                   int *has_axis, math_matrix_axis_t *axis) {
  if ((argc < 3 || argc > 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]))
    return 0;

  *rows = (size_t)math_number_value(args[1]);
  *cols = (size_t)math_number_value(args[2]);
  *layout = MATH_MATRIX_COL_MAJOR;
  *has_axis = 0;
  *axis = MATH_MATRIX_AXIS_COL;

  if (*rows == 0 || *cols == 0 || *rows > (size_t)INT_MAX || *cols > (size_t)INT_MAX ||
      args[0].data.vector.size < (*rows) * (*cols))
    return 0;

  if (argc == 4) {
    if (!math_matrix_axis_arg(args[3], axis)) return 0;
    *has_axis = 1;
  } else if (argc == 5) {
    if (!math_matrix_axis_arg(args[3], axis) || !math_matrix_layout_arg(args[4], layout))
      return 0;
    *has_axis = 1;
  }

  return 1;
}

static double math_matrix_reduce_update(double acc, double value, math_matrix_reduce_op_t op) {
  if (op == MATH_MATRIX_REDUCE_SUM || op == MATH_MATRIX_REDUCE_MEAN) return acc + value;
  if (op == MATH_MATRIX_REDUCE_MIN) return value < acc ? value : acc;
  return value > acc ? value : acc;
}

static exprtk_value_t math_matrix_reduce(size_t argc, exprtk_value_t *args, mem_pool_t *arena,
                                         math_matrix_reduce_op_t op) {
  size_t rows = 0;
  size_t cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  int has_axis = 0;
  math_matrix_axis_t axis = MATH_MATRIX_AXIS_COL;
  if (!math_matrix_reduce_args(argc, args, &rows, &cols, &layout, &has_axis, &axis))
    return exprtk_val_num(0);

  const double *data = args[0].data.vector.data;
  if (!has_axis) {
    if (op == MATH_MATRIX_REDUCE_SUM || op == MATH_MATRIX_REDUCE_MEAN) {
      double sum = simd_sum((double *)data, rows * cols);
      return exprtk_val_num(op == MATH_MATRIX_REDUCE_MEAN ? sum / (double)(rows * cols) : sum);
    }
    if (op == MATH_MATRIX_REDUCE_MIN)
      return exprtk_val_num(simd_min((double *)data, rows * cols));
    return exprtk_val_num(simd_max((double *)data, rows * cols));
  }

  size_t out_size = axis == MATH_MATRIX_AXIS_COL ? cols : rows;
  size_t inner = axis == MATH_MATRIX_AXIS_COL ? rows : cols;
  double *out = MEM_ALLOC_ARRAY(arena, double, out_size);
  if (!out) return exprtk_val_num(0);

  for (size_t i = 0; i < out_size; ++i) {
    double acc = 0.0;
    for (size_t j = 0; j < inner; ++j) {
      size_t row = axis == MATH_MATRIX_AXIS_COL ? j : i;
      size_t col = axis == MATH_MATRIX_AXIS_COL ? i : j;
      double value = math_matrix_get(data, rows, cols, layout, row, col);
      if (j == 0 && (op == MATH_MATRIX_REDUCE_MIN || op == MATH_MATRIX_REDUCE_MAX))
        acc = value;
      else
        acc = math_matrix_reduce_update(acc, value, op);
    }
    out[i] = op == MATH_MATRIX_REDUCE_MEAN ? acc / (double)inner : acc;
  }

  return exprtk_val_vec(out, out_size);
}

static exprtk_value_t fn_mat_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_matrix_reduce(argc, args, arena, MATH_MATRIX_REDUCE_SUM);
}

static exprtk_value_t fn_mat_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  return math_matrix_reduce(argc, args, arena, MATH_MATRIX_REDUCE_MEAN);
}

static exprtk_value_t fn_mat_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_matrix_reduce(argc, args, arena, MATH_MATRIX_REDUCE_MIN);
}

static exprtk_value_t fn_mat_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_matrix_reduce(argc, args, arena, MATH_MATRIX_REDUCE_MAX);
}

static exprtk_value_t fn_mat_row(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]) || !math_is_number(args[3]))
    return exprtk_val_num(0);

  size_t rows = (size_t)math_number_value(args[1]);
  size_t cols = (size_t)math_number_value(args[2]);
  size_t row = (size_t)math_number_value(args[3]);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  if (rows == 0 || cols == 0 || row >= rows || rows > (size_t)INT_MAX ||
      cols > (size_t)INT_MAX || args[0].data.vector.size < rows * cols)
    return exprtk_val_num(0);

  double *out = MEM_ALLOC_ARRAY(arena, double, cols);
  if (!out) return exprtk_val_num(0);
  for (size_t c = 0; c < cols; ++c)
    out[c] = math_matrix_get(args[0].data.vector.data, rows, cols, layout, row, c);
  return exprtk_val_vec(out, cols);
}

static exprtk_value_t fn_mat_col(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]) || !math_is_number(args[3]))
    return exprtk_val_num(0);

  size_t rows = (size_t)math_number_value(args[1]);
  size_t cols = (size_t)math_number_value(args[2]);
  size_t col = (size_t)math_number_value(args[3]);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  if (rows == 0 || cols == 0 || col >= cols || rows > (size_t)INT_MAX ||
      cols > (size_t)INT_MAX || args[0].data.vector.size < rows * cols)
    return exprtk_val_num(0);

  double *out = MEM_ALLOC_ARRAY(arena, double, rows);
  if (!out) return exprtk_val_num(0);
  for (size_t r = 0; r < rows; ++r)
    out[r] = math_matrix_get(args[0].data.vector.data, rows, cols, layout, r, col);
  return exprtk_val_vec(out, rows);
}

static exprtk_value_t fn_mat_copy(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  size_t rows = 0, cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  (void)layout;
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  memcpy(out, args[0].data.vector.data, rows * cols * sizeof(double));
  return exprtk_val_vec(out, rows * cols);
}

static exprtk_value_t fn_mat_flatten(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  return fn_mat_copy(argc, args, env, arena);
}

static exprtk_value_t fn_mat_reshape(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if ((argc != 5 && argc != 6) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]) ||
      !math_is_number(args[3]) || !math_is_number(args[4]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 6 && !math_matrix_layout_arg(args[5], &layout)) return exprtk_val_num(0);
  (void)layout;
  size_t old_rows = (size_t)math_number_value(args[1]);
  size_t old_cols = (size_t)math_number_value(args[2]);
  size_t new_rows = (size_t)math_number_value(args[3]);
  size_t new_cols = (size_t)math_number_value(args[4]);
  if (old_rows == 0 || old_cols == 0 || new_rows == 0 || new_cols == 0 ||
      old_rows * old_cols != new_rows * new_cols ||
      args[0].data.vector.size < old_rows * old_cols)
    return exprtk_val_num(0);
  double *out = MEM_ALLOC_ARRAY(arena, double, old_rows * old_cols);
  if (!out) return exprtk_val_num(0);
  memcpy(out, args[0].data.vector.data, old_rows * old_cols * sizeof(double));
  return exprtk_val_vec(out, old_rows * old_cols);
}

static exprtk_value_t fn_mat_slice(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if ((argc != 7 && argc != 8) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]) ||
      !math_is_number(args[3]) || !math_is_number(args[4]) ||
      !math_is_number(args[5]) || !math_is_number(args[6]))
    return exprtk_val_num(0);
  size_t rows = (size_t)math_number_value(args[1]);
  size_t cols = (size_t)math_number_value(args[2]);
  size_t row0 = (size_t)math_number_value(args[3]);
  size_t row_count = (size_t)math_number_value(args[4]);
  size_t col0 = (size_t)math_number_value(args[5]);
  size_t col_count = (size_t)math_number_value(args[6]);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 8 && !math_matrix_layout_arg(args[7], &layout)) return exprtk_val_num(0);
  if (rows == 0 || cols == 0 || row_count == 0 || col_count == 0 ||
      row0 + row_count > rows || col0 + col_count > cols ||
      args[0].data.vector.size < rows * cols)
    return exprtk_val_num(0);
  double *out = MEM_ALLOC_ARRAY(arena, double, row_count * col_count);
  if (!out) return exprtk_val_num(0);
  for (size_t r = 0; r < row_count; ++r) {
    for (size_t c = 0; c < col_count; ++c) {
      double value = math_matrix_get(args[0].data.vector.data, rows, cols, layout,
                                     row0 + r, col0 + c);
      size_t index = layout == MATH_MATRIX_ROW_MAJOR ? r * col_count + c : r + c * row_count;
      out[index] = value;
    }
  }
  return exprtk_val_vec(out, row_count * col_count);
}

static exprtk_value_t fn_mat_identity(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  return fn_mat_eye(argc, args, env, arena);
}

static exprtk_value_t fn_mat_eye_like(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  size_t rows = 0, cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  memset(out, 0, rows * cols * sizeof(double));
  size_t n = rows < cols ? rows : cols;
  for (size_t i = 0; i < n; ++i) {
    size_t index = layout == MATH_MATRIX_ROW_MAJOR ? i * cols + i : i + i * rows;
    out[index] = 1.0;
  }
  return exprtk_val_vec(out, rows * cols);
}

static exprtk_value_t fn_mat_dot(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc != 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_VECTOR)
    return exprtk_val_num(0);
  if (args[0].data.vector.size != args[1].data.vector.size)
    return exprtk_val_num(0);
  return exprtk_val_num(simd_dot(args[0].data.vector.data, args[1].data.vector.data,
                                 args[0].data.vector.size));
}

static exprtk_value_t fn_mat_outer(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR)
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t rows = args[0].data.vector.size;
  size_t cols = args[1].data.vector.size;
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  for (size_t r = 0; r < rows; ++r) {
    for (size_t c = 0; c < cols; ++c) {
      size_t index = layout == MATH_MATRIX_ROW_MAJOR ? r * cols + c : r + c * rows;
      out[index] = args[0].data.vector.data[r] * args[1].data.vector.data[c];
    }
  }
  return exprtk_val_vec(out, rows * cols);
}

typedef enum {
  MATH_MATRIX_BCAST_ADD = 0,
  MATH_MATRIX_BCAST_SUB = 1,
  MATH_MATRIX_BCAST_MUL = 2,
  MATH_MATRIX_BCAST_DIV = 3
} math_matrix_bcast_op_t;

static double math_matrix_bcast_apply(double a, double b, math_matrix_bcast_op_t op) {
  if (op == MATH_MATRIX_BCAST_ADD) return a + b;
  if (op == MATH_MATRIX_BCAST_SUB) return a - b;
  if (op == MATH_MATRIX_BCAST_MUL) return a * b;
  return fabs(b) < 1e-15 ? 0.0 : a / b;
}

static exprtk_value_t math_matrix_broadcast(size_t argc, exprtk_value_t *args,
                                            mem_pool_t *arena, int by_row,
                                            math_matrix_bcast_op_t op) {
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);
  size_t rows = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  size_t expected = by_row ? cols : rows;
  if (rows == 0 || cols == 0 || args[0].data.vector.size < rows * cols ||
      args[1].data.vector.size < expected)
    return exprtk_val_num(0);
  double *out = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!out) return exprtk_val_num(0);
  for (size_t r = 0; r < rows; ++r) {
    for (size_t c = 0; c < cols; ++c) {
      size_t index = layout == MATH_MATRIX_ROW_MAJOR ? r * cols + c : r + c * rows;
      double b = by_row ? args[1].data.vector.data[c] : args[1].data.vector.data[r];
      out[index] = math_matrix_bcast_apply(args[0].data.vector.data[index], b, op);
    }
  }
  return exprtk_val_vec(out, rows * cols);
}

#define DEFINE_MAT_BCAST_FN(name, by_row, op)                                      \
static exprtk_value_t name(size_t argc, exprtk_value_t *args, exprtk_env_t *env,   \
                           mem_pool_t *arena) {                                    \
  (void)env;                                                                       \
  return math_matrix_broadcast(argc, args, arena, by_row, op);                     \
}

DEFINE_MAT_BCAST_FN(fn_mat_add_row, 1, MATH_MATRIX_BCAST_ADD)
DEFINE_MAT_BCAST_FN(fn_mat_sub_row, 1, MATH_MATRIX_BCAST_SUB)
DEFINE_MAT_BCAST_FN(fn_mat_mul_row, 1, MATH_MATRIX_BCAST_MUL)
DEFINE_MAT_BCAST_FN(fn_mat_div_row, 1, MATH_MATRIX_BCAST_DIV)
DEFINE_MAT_BCAST_FN(fn_mat_add_col, 0, MATH_MATRIX_BCAST_ADD)
DEFINE_MAT_BCAST_FN(fn_mat_sub_col, 0, MATH_MATRIX_BCAST_SUB)
DEFINE_MAT_BCAST_FN(fn_mat_mul_col, 0, MATH_MATRIX_BCAST_MUL)
DEFINE_MAT_BCAST_FN(fn_mat_div_col, 0, MATH_MATRIX_BCAST_DIV)

#undef DEFINE_MAT_BCAST_FN

typedef enum {
  MATH_MATRIX_STAT_VAR = 0,
  MATH_MATRIX_STAT_STD = 1,
  MATH_MATRIX_STAT_ARGMIN = 2,
  MATH_MATRIX_STAT_ARGMAX = 3
} math_matrix_stat_op_t;

static double math_matrix_stat_vector(const double *values, size_t n, math_matrix_stat_op_t op) {
  if (n == 0) return 0.0;
  if (op == MATH_MATRIX_STAT_ARGMIN || op == MATH_MATRIX_STAT_ARGMAX) {
    size_t best = 0;
    for (size_t i = 1; i < n; ++i) {
      if ((op == MATH_MATRIX_STAT_ARGMIN && values[i] < values[best]) ||
          (op == MATH_MATRIX_STAT_ARGMAX && values[i] > values[best]))
        best = i;
    }
    return (double)best;
  }
  double mean = simd_sum((double *)values, n) / (double)n;
  double var = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double d = values[i] - mean;
    var += d * d;
  }
  var /= (double)n;
  return op == MATH_MATRIX_STAT_STD ? sqrt(var) : var;
}

static exprtk_value_t math_matrix_stat(size_t argc, exprtk_value_t *args, mem_pool_t *arena,
                                       math_matrix_stat_op_t op) {
  size_t rows = 0, cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  int has_axis = 0;
  math_matrix_axis_t axis = MATH_MATRIX_AXIS_COL;
  if (!math_matrix_reduce_args(argc, args, &rows, &cols, &layout, &has_axis, &axis))
    return exprtk_val_num(0);
  const double *data = args[0].data.vector.data;
  if (!has_axis) return exprtk_val_num(math_matrix_stat_vector(data, rows * cols, op));

  size_t out_size = axis == MATH_MATRIX_AXIS_COL ? cols : rows;
  size_t inner = axis == MATH_MATRIX_AXIS_COL ? rows : cols;
  double *tmp = MEM_ALLOC_ARRAY(arena, double, inner);
  double *out = MEM_ALLOC_ARRAY(arena, double, out_size);
  if (!tmp || !out) return exprtk_val_num(0);
  for (size_t i = 0; i < out_size; ++i) {
    for (size_t j = 0; j < inner; ++j) {
      size_t row = axis == MATH_MATRIX_AXIS_COL ? j : i;
      size_t col = axis == MATH_MATRIX_AXIS_COL ? i : j;
      tmp[j] = math_matrix_get(data, rows, cols, layout, row, col);
    }
    out[i] = math_matrix_stat_vector(tmp, inner, op);
  }
  return exprtk_val_vec(out, out_size);
}

static exprtk_value_t fn_mat_var(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_matrix_stat(argc, args, arena, MATH_MATRIX_STAT_VAR);
}

static exprtk_value_t fn_mat_std(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  return math_matrix_stat(argc, args, arena, MATH_MATRIX_STAT_STD);
}

static exprtk_value_t fn_mat_argmin(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  return math_matrix_stat(argc, args, arena, MATH_MATRIX_STAT_ARGMIN);
}

static exprtk_value_t fn_mat_argmax(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  return math_matrix_stat(argc, args, arena, MATH_MATRIX_STAT_ARGMAX);
}

static exprtk_value_t fn_linalg_qr(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  size_t rows = 0, cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  double *a = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  double *q = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  double *rmat = MEM_ALLOC_ARRAY(arena, double, cols * cols);
  double *out_q = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!a || !q || !rmat || !out_q) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, rows, cols, layout, a);
  memset(q, 0, rows * cols * sizeof(double));
  memset(rmat, 0, cols * cols * sizeof(double));
  for (size_t j = 0; j < cols; ++j) {
    for (size_t rr = 0; rr < rows; ++rr)
      q[rr * cols + j] = a[rr * cols + j];
    for (size_t k = 0; k < j; ++k) {
      double dot = 0.0;
      for (size_t rr = 0; rr < rows; ++rr)
        dot += q[rr * cols + k] * a[rr * cols + j];
      rmat[k * cols + j] = dot;
      for (size_t rr = 0; rr < rows; ++rr)
        q[rr * cols + j] -= dot * q[rr * cols + k];
    }
    double norm = 0.0;
    for (size_t rr = 0; rr < rows; ++rr)
      norm += q[rr * cols + j] * q[rr * cols + j];
    norm = sqrt(norm);
    if (norm < 1e-12) return exprtk_val_num(0);
    rmat[j * cols + j] = norm;
    for (size_t rr = 0; rr < rows; ++rr)
      q[rr * cols + j] /= norm;
  }
  math_row_major_to_double_matrix(q, rows, cols, layout, out_q);
  double *out_r = MEM_ALLOC_ARRAY(arena, double, cols * cols);
  if (!out_r) return exprtk_val_num(0);
  math_row_major_to_double_matrix(rmat, cols, cols, layout, out_r);
  exprtk_value_t list = exprtk_val_list_empty();
  exprtk_list_push(&list, exprtk_val_vec(out_q, rows * cols));
  exprtk_list_push(&list, exprtk_val_vec(out_r, cols * cols));
  return list;
}

static exprtk_value_t fn_linalg_pinv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  size_t rows = 0, cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);
  if (rows == cols) {
    exprtk_value_t inv_args[3] = {args[0], exprtk_val_num((double)rows), exprtk_val_num((double)layout)};
    return fn_mat_inv(3, inv_args, env, arena);
  }
  if (rows < cols) return exprtk_val_num(0);
  double *a = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  double *ata = MEM_ALLOC_ARRAY(arena, double, cols * cols);
  double *ata_inv = MEM_ALLOC_ARRAY(arena, double, cols * cols);
  double *pinv_row = MEM_ALLOC_ARRAY(arena, double, cols * rows);
  double *out = MEM_ALLOC_ARRAY(arena, double, cols * rows);
  if (!a || !ata || !ata_inv || !pinv_row || !out) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, rows, cols, layout, a);
  for (size_t i = 0; i < cols; ++i) {
    for (size_t j = 0; j < cols; ++j) {
      double sum = 0.0;
      for (size_t r = 0; r < rows; ++r)
        sum += a[r * cols + i] * a[r * cols + j];
      ata[i * cols + j] = sum;
      ata_inv[i * cols + j] = sum;
    }
  }
  if (!gauss_jordan_invert(ata_inv, cols, arena)) return exprtk_val_num(0);
  for (size_t i = 0; i < cols; ++i) {
    for (size_t r = 0; r < rows; ++r) {
      double sum = 0.0;
      for (size_t k = 0; k < cols; ++k)
        sum += ata_inv[i * cols + k] * a[r * cols + k];
      pinv_row[i * rows + r] = sum;
    }
  }
  math_row_major_to_double_matrix(pinv_row, cols, rows, layout, out);
  return exprtk_val_vec(out, cols * rows);
}

static exprtk_value_t fn_linalg_lstsq(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);
  size_t rows = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  if (rows == 0 || cols == 0 || args[0].data.vector.size < rows * cols ||
      args[1].data.vector.size < rows)
    return exprtk_val_num(0);
  exprtk_value_t pinv_args[4] = {args[0], exprtk_val_num((double)rows),
                                 exprtk_val_num((double)cols), exprtk_val_num((double)layout)};
  exprtk_value_t pinv = fn_linalg_pinv(4, pinv_args, env, arena);
  if (pinv.type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0);
  double *p = MEM_ALLOC_ARRAY(arena, double, cols * rows);
  double *b = MEM_ALLOC_ARRAY(arena, double, rows);
  double *x = MEM_ALLOC_ARRAY(arena, double, cols);
  if (!p || !b || !x) return exprtk_val_num(0);
  math_double_to_row_major_matrix(pinv.data.vector.data, cols, rows, layout, p);
  for (size_t r = 0; r < rows; ++r)
    b[r] = args[1].data.vector.data[r];
  for (size_t c = 0; c < cols; ++c) {
    double sum = 0.0;
    for (size_t r = 0; r < rows; ++r)
      sum += p[c * rows + r] * b[r];
    x[c] = sum;
  }
  return exprtk_val_vec(x, cols);
}

static exprtk_value_t fn_linalg_lu(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || n > (size_t)INT_MAX || args[0].data.vector.size < n * n)
    return exprtk_val_num(0);

  double *lu = MEM_ALLOC_ARRAY(arena, double, n * n);
  size_t *pivots = MEM_ALLOC_ARRAY(arena, size_t, n);
  double *l_row = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *u_row = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *l_out = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *u_out = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *p_out = MEM_ALLOC_ARRAY(arena, double, n);
  if (!lu || !pivots || !l_row || !u_row || !l_out || !u_out || !p_out)
    return exprtk_val_num(0);

  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, lu);
  if (!math_lu_decompose_row_major(lu, n, pivots, NULL)) return exprtk_val_num(0);

  for (size_t r = 0; r < n; ++r) {
    for (size_t c = 0; c < n; ++c) {
      if (r > c) {
        l_row[r * n + c] = lu[r * n + c];
        u_row[r * n + c] = 0.0;
      } else if (r == c) {
        l_row[r * n + c] = 1.0;
        u_row[r * n + c] = lu[r * n + c];
      } else {
        l_row[r * n + c] = 0.0;
        u_row[r * n + c] = lu[r * n + c];
      }
    }
    p_out[r] = (double)pivots[r];
  }

  math_row_major_to_double_matrix(l_row, n, n, layout, l_out);
  math_row_major_to_double_matrix(u_row, n, n, layout, u_out);
  exprtk_value_t list = exprtk_val_list_empty();
  exprtk_list_push(&list, exprtk_val_vec(l_out, n * n));
  exprtk_list_push(&list, exprtk_val_vec(u_out, n * n));
  exprtk_list_push(&list, exprtk_val_vec(p_out, n));
  return list;
}

static exprtk_value_t fn_linalg_lu_solve(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  if (n == 0 || cols == 0 || n > (size_t)INT_MAX || cols > (size_t)INT_MAX ||
      args[0].data.vector.size < n * n || args[1].data.vector.size < n * cols)
    return exprtk_val_num(0);

  double *lu = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *b = MEM_ALLOC_ARRAY(arena, double, n * cols);
  double *x = MEM_ALLOC_ARRAY(arena, double, n * cols);
  double *out = MEM_ALLOC_ARRAY(arena, double, n * cols);
  size_t *pivots = MEM_ALLOC_ARRAY(arena, size_t, n);
  if (!lu || !b || !x || !out || !pivots) return exprtk_val_num(0);

  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, lu);
  math_double_to_row_major_matrix(args[1].data.vector.data, n, cols, layout, b);
  if (!math_lu_decompose_row_major(lu, n, pivots, NULL)) return exprtk_val_num(0);

  for (size_t r = 0; r < n; ++r)
    for (size_t c = 0; c < cols; ++c)
      x[r * cols + c] = b[pivots[r] * cols + c];

  for (size_t i = 0; i < n; ++i) {
    for (size_t c = 0; c < cols; ++c) {
      double sum = x[i * cols + c];
      for (size_t k = 0; k < i; ++k)
        sum -= lu[i * n + k] * x[k * cols + c];
      x[i * cols + c] = sum;
    }
  }

  for (size_t ii = n; ii-- > 0;) {
    for (size_t c = 0; c < cols; ++c) {
      double sum = x[ii * cols + c];
      for (size_t k = ii + 1; k < n; ++k)
        sum -= lu[ii * n + k] * x[k * cols + c];
      double diag = lu[ii * n + ii];
      if (fabs(diag) < 1e-12) return exprtk_val_num(0);
      x[ii * cols + c] = sum / diag;
    }
  }

  math_row_major_to_double_matrix(x, n, cols, layout, out);
  return exprtk_val_vec(out, n * cols);
}

static exprtk_value_t fn_mat_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if ((argc < 3 || argc > 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      !math_is_number(args[1]) || !math_is_number(args[2]))
    return exprtk_val_num(0);
  size_t rows = (size_t)math_number_value(args[1]);
  size_t cols = (size_t)math_number_value(args[2]);
  if (rows == 0 || cols == 0 || args[0].data.vector.size < rows * cols)
    return exprtk_val_num(0);

  double tol = 1e-10;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 4) {
    if ((args[3].type == EXPRTK_VAL_STRING) ||
        (math_is_number(args[3]) && (fabs(math_number_value(args[3])) < 1e-12 ||
                                     fabs(math_number_value(args[3]) - 1.0) < 1e-12))) {
      if (!math_matrix_layout_arg(args[3], &layout)) return exprtk_val_num(0);
    } else if (math_is_number(args[3])) {
      tol = fabs(math_number_value(args[3]));
    } else {
      return exprtk_val_num(0);
    }
  } else if (argc == 5) {
    if (!math_is_number(args[3]) || !math_matrix_layout_arg(args[4], &layout))
      return exprtk_val_num(0);
    tol = fabs(math_number_value(args[3]));
  }

  double *a = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  if (!a) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, rows, cols, layout, a);

  size_t rank = 0;
  size_t pivot_row = 0;
  for (size_t c = 0; c < cols && pivot_row < rows; ++c) {
    size_t pivot = pivot_row;
    double pivot_abs = fabs(a[pivot * cols + c]);
    for (size_t r = pivot_row + 1; r < rows; ++r) {
      double v = fabs(a[r * cols + c]);
      if (v > pivot_abs) {
        pivot = r;
        pivot_abs = v;
      }
    }
    if (pivot_abs <= tol) continue;
    if (pivot != pivot_row) {
      for (size_t cc = 0; cc < cols; ++cc) {
        double tmp = a[pivot_row * cols + cc];
        a[pivot_row * cols + cc] = a[pivot * cols + cc];
        a[pivot * cols + cc] = tmp;
      }
    }
    double pivot_value = a[pivot_row * cols + c];
    for (size_t r = pivot_row + 1; r < rows; ++r) {
      double factor = a[r * cols + c] / pivot_value;
      a[r * cols + c] = 0.0;
      for (size_t cc = c + 1; cc < cols; ++cc)
        a[r * cols + cc] -= factor * a[pivot_row * cols + cc];
    }
    ++rank;
    ++pivot_row;
  }
  return exprtk_val_num((double)rank);
}

static exprtk_value_t fn_linalg_eigh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || args[0].data.vector.size < n * n) return exprtk_val_num(0);

  double *a = MEM_ALLOC_ARRAY(arena, double, n * n);
  double *eigenvalues = MEM_ALLOC_ARRAY(arena, double, n);
  if (!a || !eigenvalues) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, a);
  for (size_t r = 0; r < n; ++r) {
    for (size_t c = r + 1; c < n; ++c) {
      if (fabs(a[r * n + c] - a[c * n + r]) > 1e-8) return exprtk_val_num(0);
    }
  }
  if (!math_symmetric_eigenvalues_jacobi(a, n, eigenvalues)) return exprtk_val_num(0);
  math_sort_asc(eigenvalues, n);
  return exprtk_val_vec(eigenvalues, n);
}

static exprtk_value_t fn_linalg_svd(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  size_t rows = 0, cols = 0;
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (!math_matrix_dims_from_args(argc, args, 3, &rows, &cols, &layout))
    return exprtk_val_num(0);

  double *a = MEM_ALLOC_ARRAY(arena, double, rows * cols);
  double *ata = MEM_ALLOC_ARRAY(arena, double, cols * cols);
  double *evals = MEM_ALLOC_ARRAY(arena, double, cols);
  double *s = MEM_ALLOC_ARRAY(arena, double, cols);
  if (!a || !ata || !evals || !s) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, rows, cols, layout, a);

  for (size_t i = 0; i < cols; ++i) {
    for (size_t j = 0; j < cols; ++j) {
      double sum = 0.0;
      for (size_t r = 0; r < rows; ++r)
        sum += a[r * cols + i] * a[r * cols + j];
      ata[i * cols + j] = sum;
    }
  }
  if (!math_symmetric_eigenvalues_jacobi(ata, cols, evals)) return exprtk_val_num(0);
  for (size_t i = 0; i < cols; ++i)
    s[i] = sqrt(fmax(0.0, evals[i]));
  math_sort_desc(s, cols);
  return exprtk_val_vec(s, cols);
}

static exprtk_value_t fn_mat_cond(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  exprtk_value_t s = fn_linalg_svd(argc, args, env, arena);
  if (s.type != EXPRTK_VAL_VECTOR || s.data.vector.size == 0) return exprtk_val_num(0);
  double max_s = 0.0;
  double min_s = INFINITY;
  for (size_t i = 0; i < s.data.vector.size; ++i) {
    double v = fabs(s.data.vector.data[i]);
    if (v > max_s) max_s = v;
    if (v < min_s) min_s = v;
  }
  if (min_s <= 1e-12 || !isfinite(min_s)) return exprtk_val_num(0);
  return exprtk_val_num(max_s / min_s);
}

static exprtk_value_t fn_linalg_solve_cholesky(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
  (void)env;
  if ((argc != 4 && argc != 5) || args[0].type != EXPRTK_VAL_VECTOR ||
      args[1].type != EXPRTK_VAL_VECTOR || !math_is_number(args[2]) ||
      !math_is_number(args[3]))
    return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[2]);
  size_t cols = (size_t)math_number_value(args[3]);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 5 && !math_matrix_layout_arg(args[4], &layout)) return exprtk_val_num(0);
  if (n == 0 || cols == 0 || args[0].data.vector.size < n * n ||
      args[1].data.vector.size < n * cols)
    return exprtk_val_num(0);

  exprtk_value_t chol_args[3] = {args[0], exprtk_val_num((double)n), exprtk_val_num((double)layout)};
  exprtk_value_t l = fn_mat_cholesky(3, chol_args, env, arena);
  if (l.type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0);
  exprtk_value_t forward_args[5] = {l, args[1], exprtk_val_num((double)n), exprtk_val_num((double)cols),
                                    exprtk_val_num((double)layout)};
  exprtk_value_t y = fn_mat_solve_tri(5, forward_args, env, arena);
  if (y.type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0);
  exprtk_value_t back_args[6] = {l, y, exprtk_val_num((double)n), exprtk_val_num((double)cols),
                                 exprtk_val_num(1.0), exprtk_val_num((double)layout)};
  return fn_mat_solve_tri(6, back_args, env, arena);
}

static exprtk_value_t fn_linalg_det_lu(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_VECTOR || !math_is_number(args[1]))
    return exprtk_val_num(0);
  math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
  if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
  size_t n = (size_t)math_number_value(args[1]);
  if (n == 0 || args[0].data.vector.size < n * n) return exprtk_val_num(0);
  double *lu = MEM_ALLOC_ARRAY(arena, double, n * n);
  size_t *pivots = MEM_ALLOC_ARRAY(arena, size_t, n);
  int swaps = 0;
  if (!lu || !pivots) return exprtk_val_num(0);
  math_double_to_row_major_matrix(args[0].data.vector.data, n, n, layout, lu);
  if (!math_lu_decompose_row_major(lu, n, pivots, &swaps)) return exprtk_val_num(0);
  double det = swaps & 1 ? -1.0 : 1.0;
  for (size_t i = 0; i < n; ++i) det *= lu[i * n + i];
  return exprtk_val_num(det);
}

static exprtk_value_t fn_linalg_rank(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  return fn_mat_rank(argc, args, env, arena);
}

static exprtk_value_t fn_linalg_cond(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  return fn_mat_cond(argc, args, env, arena);
}

static exprtk_value_t fn_mat_slogdet(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  exprtk_value_t det_val = fn_linalg_det_lu(argc, args, env, arena);
  if (!math_is_number(det_val)) return exprtk_val_num(0);
  double det = math_number_value(det_val);
  if (fabs(det) < 1e-15) return exprtk_val_num(0);
  double *out = MEM_ALLOC_ARRAY(arena, double, 2);
  if (!out) return exprtk_val_num(0);
  out[0] = det < 0.0 ? -1.0 : 1.0;
  out[1] = log(fabs(det));
  return exprtk_val_vec(out, 2);
}

static exprtk_value_t fn_linalg_eig2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  return fn_eig2(argc, args, env, arena);
}

static double math_normal_pdf_value(double x, double mu, double sigma) {
  if (sigma <= 0.0) return 0.0;
  double z = (x - mu) / sigma;
  return exp(-0.5 * z * z) / (sigma * sqrt(2.0 * TS_MATH_PI));
}

static double math_normal_cdf_value(double x, double mu, double sigma) {
  if (sigma <= 0.0) return 0.0;
  return 0.5 * (1.0 + erf((x - mu) / (sigma * sqrt(2.0))));
}

static double math_normal_quantile_value(double p, double mu, double sigma) {
  if (p <= 0.0) return -INFINITY;
  if (p >= 1.0) return INFINITY;
  double lo = mu - 12.0 * sigma;
  double hi = mu + 12.0 * sigma;
  for (int i = 0; i < 100; ++i) {
    double mid = (lo + hi) * 0.5;
    if (math_normal_cdf_value(mid, mu, sigma) < p) lo = mid;
    else hi = mid;
  }
  return (lo + hi) * 0.5;
}

static double math_t_pdf_value(double x, double nu) {
  if (nu <= 0.0) return 0.0;
  double c = tgamma((nu + 1.0) * 0.5) / (sqrt(nu * TS_MATH_PI) * tgamma(nu * 0.5));
  return c * pow(1.0 + (x * x) / nu, -(nu + 1.0) * 0.5);
}

static double math_t_cdf_value(double x, double nu) {
  if (nu <= 0.0) return 0.0;
  if (x == 0.0) return 0.5;
  double sign = x < 0.0 ? -1.0 : 1.0;
  double ax = fabs(x);
  double upper = ax > 80.0 ? 80.0 : ax;
  int steps = 800;
  if (steps % 2) ++steps;
  double h = upper / (double)steps;
  double sum = math_t_pdf_value(0.0, nu) + math_t_pdf_value(upper, nu);
  for (int i = 1; i < steps; ++i) {
    double xx = h * (double)i;
    sum += (i % 2 ? 4.0 : 2.0) * math_t_pdf_value(xx, nu);
  }
  double area = sum * h / 3.0;
  double cdf = 0.5 + sign * area;
  if (cdf < 0.0) cdf = 0.0;
  if (cdf > 1.0) cdf = 1.0;
  return cdf;
}

static double math_t_quantile_value(double p, double nu) {
  if (p <= 0.0) return -INFINITY;
  if (p >= 1.0) return INFINITY;
  double lo = -80.0;
  double hi = 80.0;
  for (int i = 0; i < 80; ++i) {
    double mid = (lo + hi) * 0.5;
    if (math_t_cdf_value(mid, nu) < p) lo = mid;
    else hi = mid;
  }
  return (lo + hi) * 0.5;
}

static exprtk_value_t fn_stats_normal_pdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc < 1 || argc > 3 || !math_is_number(args[0])) return exprtk_val_num(0);
  double mu = argc >= 2 && math_is_number(args[1]) ? math_number_value(args[1]) : 0.0;
  double sigma = argc >= 3 && math_is_number(args[2]) ? math_number_value(args[2]) : 1.0;
  return exprtk_val_num(math_normal_pdf_value(math_number_value(args[0]), mu, sigma));
}

static exprtk_value_t fn_stats_normal_cdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc < 1 || argc > 3 || !math_is_number(args[0])) return exprtk_val_num(0);
  double mu = argc >= 2 && math_is_number(args[1]) ? math_number_value(args[1]) : 0.0;
  double sigma = argc >= 3 && math_is_number(args[2]) ? math_number_value(args[2]) : 1.0;
  return exprtk_val_num(math_normal_cdf_value(math_number_value(args[0]), mu, sigma));
}

static exprtk_value_t fn_stats_normal_quantile(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc < 1 || argc > 3 || !math_is_number(args[0])) return exprtk_val_num(0);
  double mu = argc >= 2 && math_is_number(args[1]) ? math_number_value(args[1]) : 0.0;
  double sigma = argc >= 3 && math_is_number(args[2]) ? math_number_value(args[2]) : 1.0;
  return exprtk_val_num(math_normal_quantile_value(math_number_value(args[0]), mu, sigma));
}

static exprtk_value_t fn_stats_t_pdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc != 2 || !math_is_number(args[0]) || !math_is_number(args[1])) return exprtk_val_num(0);
  return exprtk_val_num(math_t_pdf_value(math_number_value(args[0]), math_number_value(args[1])));
}

static exprtk_value_t fn_stats_t_cdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc != 2 || !math_is_number(args[0]) || !math_is_number(args[1])) return exprtk_val_num(0);
  return exprtk_val_num(math_t_cdf_value(math_number_value(args[0]), math_number_value(args[1])));
}

static exprtk_value_t fn_stats_t_quantile(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc != 2 || !math_is_number(args[0]) || !math_is_number(args[1])) return exprtk_val_num(0);
  return exprtk_val_num(math_t_quantile_value(math_number_value(args[0]), math_number_value(args[1])));
}

static exprtk_value_t fn_stats_t_test_1samp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                            mem_pool_t *arena) {
  (void)env; (void)arena;
  if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_VECTOR) return exprtk_val_num(0);
  size_t n = args[0].data.vector.size;
  if (n < 2) return exprtk_val_num(0);
  double mu0 = argc == 2 && math_is_number(args[1]) ? math_number_value(args[1]) : 0.0;
  double mean = simd_sum(args[0].data.vector.data, n) / (double)n;
  double ss = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double d = args[0].data.vector.data[i] - mean;
    ss += d * d;
  }
  double sd = sqrt(ss / (double)(n - 1));
  double t = sd <= 0.0 ? 0.0 : (mean - mu0) / (sd / sqrt((double)n));
  exprtk_value_t map = exprtk_val_map();
  exprtk_map_set(&map, "t", exprtk_val_num(t));
  exprtk_map_set(&map, "df", exprtk_val_num((double)(n - 1)));
  exprtk_map_set(&map, "mean", exprtk_val_num(mean));
  exprtk_map_set(&map, "p_two", exprtk_val_num(2.0 * (1.0 - math_t_cdf_value(fabs(t), (double)(n - 1)))));
  return map;
}

static exprtk_value_t fn_transpose(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t rows = (size_t)args[1].data.number, cols = (size_t)args[2].data.number;
    if (args[0].data.vector.size >= rows * cols) {
      double *res = MEM_ALLOC_ARRAY(arena, double, rows * cols);
      if (res) {
        exprtk_transpose(args[0].data.vector.data, rows, cols, res);
        return exprtk_val_vec(res, rows * cols);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_eig2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4) {
    double *ev = MEM_ALLOC_ARRAY(arena, double, 2);
    if (ev) {
      exprtk_eig2(args[0].data.vector.data, ev);
      return exprtk_val_vec(ev, 2);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_eig3(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[1].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 9 &&
      args[1].data.vector.size >= 3)
    return exprtk_val_num((double)exprtk_eig3(args[0].data.vector.data, args[1].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_trace2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 4)
    return exprtk_val_num(exprtk_trace2(args[0].data.vector.data));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sin(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(sin(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cos(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(cos(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_tan(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(tan(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sqrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(sqrt(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_abs(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_NUMBER)
      return exprtk_val_num(fabs(args[0].data.number));
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = MEM_ALLOC_ARRAY(arena, double, n);
        if (res) {
            simd_abs(args[0].data.vector.data, res, n);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_exp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(exp(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ceil(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(ceil(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_floor(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(floor(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_round(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(round(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_len(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_STRING)
      return exprtk_val_num((double)args[0].data.string.len);
    if (args[0].type == EXPRTK_VAL_VECTOR)
      return exprtk_val_num((double)args[0].data.vector.size);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_min(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(simd_min(args[0].data.vector.data, args[0].data.vector.size));
  }
  if (argc > 0) {
    double res = args[0].data.number;
    for (size_t i = 1; i < argc; ++i)
      if (args[i].data.number < res)
        res = args[i].data.number;
    return exprtk_val_num(res);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_max(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(simd_max(args[0].data.vector.data, args[0].data.vector.size));
  }
  if (argc > 0) {
    double res = args[0].data.number;
    for (size_t i = 1; i < argc; ++i)
      if (args[i].data.number > res)
        res = args[i].data.number;
    return exprtk_val_num(res);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_avg(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)arena;
  if (argc != 1 || args[0].type != EXPRTK_VAL_VECTOR) {
    if (env)
      env->aborted = 1;
    return exprtk_val_num(0);
  }
  if (args[0].data.vector.size == 0)
    return exprtk_val_num(0);
  size_t n = args[0].data.vector.size;
  return exprtk_val_num(simd_sum(args[0].data.vector.data, n) / (double)n);
}

static exprtk_value_t fn_sum(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    return exprtk_val_num(simd_sum(args[0].data.vector.data, args[0].data.vector.size));
  }
  if (argc > 0) {
    double sum = 0;
    for (size_t i = 0; i < argc; ++i)
      sum += args[i].data.number;
    return exprtk_val_num(sum);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_fibonacci(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(exprtk_fibonacci((int)args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_gcd(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(
        (double)exprtk_gcd((long long)args[0].data.number, (long long)args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_normal_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 0 || argc == 2) {
    double mu = (argc == 2) ? args[0].data.number : 0.0;
    double sigma = (argc == 2) ? args[1].data.number : 1.0;
    return exprtk_val_num(exprtk_normal_rand(mu, sigma));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vector_find_value(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                           mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    double target = args[1].data.number;
    for (size_t i = 0; i < args[0].data.vector.size; ++i) {
      if (fabs(args[0].data.vector.data[i] - target) < 1e-9)
        return exprtk_val_num((double)i);
    }
    return exprtk_val_num(-1.0);
  }
  return exprtk_val_num(-1.0);
}

static exprtk_value_t fn_vector_find_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    double target = args[1].data.number;
    size_t n = args[0].data.vector.size;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
      if (fabs(args[0].data.vector.data[i] - target) < 1e-9)
        count++;
    }
    double *res = (double *)mem_alloc(arena, count * sizeof(double));
    if (res) {
      size_t k = 0;
      for (size_t i = 0; i < n; ++i) {
        if (fabs(args[0].data.vector.data[i] - target) < 1e-9)
          res[k++] = (double)i;
      }
      return exprtk_val_vec(res, count);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_median(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_median(args[0].data.vector.data, args[0].data.vector.size, arena));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_percentile(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_percentile(args[0].data.vector.data, args[0].data.vector.size,
                                            args[1].data.number, arena));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_geometric_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(
        exprtk_geometric_mean(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_harmonic_mean(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_harmonic_mean(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_skewness(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_skewness(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_kurtosis(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0)
      return exprtk_val_num(0);
    return exprtk_val_num(exprtk_kurtosis(args[0].data.vector.data, args[0].data.vector.size));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv_normal_cdf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(inv_normal_cdf(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ols_fit(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 0 || args[0].data.vector.size != args[1].data.vector.size)
      return exprtk_val_num(0);
    ols_result_t r =
        ols_fit(args[0].data.vector.data, args[1].data.vector.data, args[0].data.vector.size);
    double *res = MEM_ALLOC_ARRAY(arena, double, 4);
    if (res) {
      res[0] = r.slope;
      res[1] = r.intercept;
      res[2] = r.res_var;
      res[3] = r.t_slope;
      return exprtk_val_vec(res, 4);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = (size_t)args[1].data.number;
    if (args[0].data.vector.size >= n * n) {
      double *res = MEM_ALLOC_ARRAY(arena, double, n * n);
      if (res) {
        memcpy(res, args[0].data.vector.data, n * n * sizeof(double));
        if (gauss_jordan_invert(res, n, arena))
          return exprtk_val_vec(res, n * n);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_dot(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == args[1].data.vector.size) {
      size_t n = args[0].data.vector.size;
      double *a = args[0].data.vector.data;
      double *b = args[1].data.vector.data;
      return exprtk_val_num(simd_dot(a, b, n));
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cross(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
      double *res = MEM_ALLOC_ARRAY(arena, double, 3);
      if (res) {
        double *a = args[0].data.vector.data;
        double *b = args[1].data.vector.data;
        res[0] = a[1] * b[2] - a[2] * b[1];
        res[1] = a[2] * b[0] - a[0] * b[2];
        res[2] = a[0] * b[1] - a[1] * b[0];
        return exprtk_val_vec(res, 3);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_norm(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *a = args[0].data.vector.data;
    return exprtk_val_num(sqrt(simd_norm_sq(a, n)));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_asin(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(asin(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_angle(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
      double *a = args[0].data.vector.data;
      double *b = args[1].data.vector.data;
      double dot = simd_dot(a, b, 3);
      double mag_a = sqrt(simd_norm_sq(a, 3));
      double mag_b = sqrt(simd_norm_sq(b, 3));
      if (mag_a * mag_b > 0)
        return exprtk_val_num(acos(dot / (mag_a * mag_b)));
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_proj(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3) {
      double *res = MEM_ALLOC_ARRAY(arena, double, 3);
      if (res) {
        double *a = args[0].data.vector.data;
        double *b = args[1].data.vector.data;
        double dot = simd_dot(a, b, 3);
        double mag_b_sq = simd_norm_sq(b, 3);
        if (mag_b_sq > 0) {
          double scale = dot / mag_b_sq;
          simd_scale(b, res, scale, 3);
        } else {
          res[0] = res[1] = res[2] = 0;
        }
        return exprtk_val_vec(res, 3);
      }
    }
  }
  return exprtk_val_num(0);
}

/* ========================================================================= */
/* Flex Engine SIMD Types & Operations                                       */
/* ========================================================================= */
static exprtk_value_t fn_vec_add(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = MEM_ALLOC_ARRAY(arena, double, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_add(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sub(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = MEM_ALLOC_ARRAY(arena, double, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_sub(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = MEM_ALLOC_ARRAY(arena, double, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_mul(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_div(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == args[1].data.vector.size) {
      double *res = MEM_ALLOC_ARRAY(arena, double, n);
      double *a = args[0].data.vector.data, *b = args[1].data.vector.data;
      simd_div(a, b, res, n);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sqr(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *res = MEM_ALLOC_ARRAY(arena, double, n);
    simd_sqr(args[0].data.vector.data, res, n);
    return exprtk_val_vec(res, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_reciprocal(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                        mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *res = MEM_ALLOC_ARRAY(arena, double, n);
    simd_reciprocal(args[0].data.vector.data, res, n);
    return exprtk_val_vec(res, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_scale(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER) {
    size_t n = args[0].data.vector.size;
    double *res = MEM_ALLOC_ARRAY(arena, double, n);
    double *a = args[0].data.vector.data;
    double s = args[1].data.number;
    simd_scale(a, res, s, n);
    return exprtk_val_vec(res, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec2_perp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 2) {
    double *res = MEM_ALLOC_ARRAY(arena, double, 2);
    res[0] = -args[0].data.vector.data[1];
    res[1] = args[0].data.vector.data[0];
    return exprtk_val_vec(res, 2);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_normalize(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *a = args[0].data.vector.data;
    double len = sqrt(simd_norm_sq(a, n));
    if (len > 1e-15) {
      double *res = MEM_ALLOC_ARRAY(arena, double, n);
      simd_scale(a, res, 1.0 / len, n);
      return exprtk_val_vec(res, n);
    }
    return exprtk_val_vec(a, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          mem_pool_t *arena) {
  (void)env;
  if (argc >= 5) {
    double x = args[0].data.number, y = args[1].data.number;
    double rot_deg = args[2].data.number;
    double sx = args[3].data.number, sy = args[4].data.number;
    double *res = MEM_ALLOC_ARRAY(arena, double, 6);
    if (rot_deg == 0) {
      res[0] = sx;
      res[1] = 0;
      res[2] = x;
      res[3] = 0;
      res[4] = sy;
      res[5] = y;
    } else {
      double rad = rot_deg * (3.14159265358979323846 / 180.0);
      double c = cos(rad), s = sin(rad);
      res[0] = sx * c;
      res[1] = -sy * s;
      res[2] = x;
      res[3] = sx * s;
      res[4] = sy * c;
      res[5] = y;
    }
    return exprtk_val_vec(res, 6);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_pt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR &&
      args[0].data.vector.size >= 6 && args[1].data.vector.size >= 2) {
    double *m = args[0].data.vector.data;
    double *p = args[1].data.vector.data;
    double *res = MEM_ALLOC_ARRAY(arena, double, 2);
    double px = p[0], py = p[1];
    res[0] = m[0] * px + m[1] * py + m[2];
    res[1] = m[3] * px + m[4] * py + m[5];
    return exprtk_val_vec(res, 2);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR &&
      args[0].data.vector.size >= 6 && args[1].data.vector.size >= 6) {
    double *res = MEM_ALLOC_ARRAY(arena, double, 6);
    double *m = args[0].data.vector.data, *o = args[1].data.vector.data;
    res[0] = m[0] * o[0] + m[1] * o[3];
    res[1] = m[0] * o[1] + m[1] * o[4];
    res[2] = m[0] * o[2] + m[1] * o[5] + m[2];
    res[3] = m[3] * o[0] + m[4] * o[3];
    res[4] = m[3] * o[1] + m[4] * o[4];
    res[5] = m[3] * o[2] + m[4] * o[5] + m[5];
    return exprtk_val_vec(res, 6);
  }
  return exprtk_val_num(0);
}

/* --- Vector Utilities (merged from vec module) --- */

static int cmp_double_asc(const void *a, const void *b) {
  double da = *(const double *)a, db = *(const double *)b;
  return (da > db) - (da < db);
}

static int cmp_double_desc(const void *a, const void *b) {
  double da = *(const double *)a, db = *(const double *)b;
  return (db > da) - (db < da);
}

static exprtk_value_t fn_vec_sort(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0);
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_asc);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_sort_desc(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0);
    memcpy(out, args[0].data.vector.data, n * sizeof(double));
    qsort(out, n, sizeof(double), cmp_double_desc);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_unique(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *tmp = MEM_ALLOC_ARRAY(arena, double, n);
    if (!tmp)
      return exprtk_val_num(0);
    memcpy(tmp, args[0].data.vector.data, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double_asc);
    size_t out_n = 1;
    for (size_t i = 1; i < n; ++i)
      if (tmp[i] != tmp[out_n - 1])
        tmp[out_n++] = tmp[i];
    return exprtk_val_vec(tmp, out_n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_reverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0);
    simd_reverse(args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_concat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR) {
    size_t n1 = args[0].data.vector.size, n2 = args[1].data.vector.size;
    size_t total = n1 + n2;
    if (total == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = MEM_ALLOC_ARRAY(arena, double, total);
    if (!out)
      return exprtk_val_num(0);
    if (n1)
      memcpy(out, args[0].data.vector.data, n1 * sizeof(double));
    if (n2)
      memcpy(out + n1, args[1].data.vector.data, n2 * sizeof(double));
    return exprtk_val_vec(out, total);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_range(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc < 1 || argc > 2)
    return exprtk_val_num(0);

  // Check argument types
  if (args[0].type != EXPRTK_VAL_NUMBER)
    return exprtk_val_num(0);
  if (argc == 2 && args[1].type != EXPRTK_VAL_NUMBER)
    return exprtk_val_num(0);

  double start_d = 0, end_d;
  if (argc == 1) {
    end_d = args[0].data.number;
  } else {
    start_d = args[0].data.number;
    end_d = args[1].data.number;
  }
  if (end_d <= start_d)
    return exprtk_val_vec(NULL, 0);
  size_t n = (size_t)(end_d - start_d);
  double *out = MEM_ALLOC_ARRAY(arena, double, n);
  if (!out)
    return exprtk_val_num(0);
  for (size_t i = 0; i < n; ++i)
    out[i] = start_d + (double)i;
  return exprtk_val_vec(out, n);
}

static exprtk_value_t fn_vec_cumsum(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0)
      return exprtk_val_vec(NULL, 0);
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (!out)
      return exprtk_val_num(0);
    simd_cumsum(args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_diff(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n <= 1)
      return exprtk_val_vec(NULL, 0);
    size_t out_n = n - 1;
    double *out = MEM_ALLOC_ARRAY(arena, double, out_n);
    if (!out)
      return exprtk_val_num(0);
    simd_diff(args[0].data.vector.data, out, n);
    return exprtk_val_vec(out, out_n);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_transform_inv(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR && args[0].data.vector.size >= 6) {
    double *m = args[0].data.vector.data;
    double det = m[0] * m[4] - m[1] * m[3];
    if (fabs(det) < 1e-15)
      return exprtk_val_num(0);
    double *res = MEM_ALLOC_ARRAY(arena, double, 6);
    double idet = 1.0 / det;
    res[0] = m[4] * idet;
    res[1] = -m[1] * idet;
    res[2] = (m[1] * m[5] - m[4] * m[2]) * idet;
    res[3] = -m[3] * idet;
    res[4] = m[0] * idet;
    res[5] = (m[3] * m[2] - m[0] * m[5]) * idet;
    return exprtk_val_vec(res, 6);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_rotate_vec(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_NUMBER &&
      args[2].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[2].data.vector.size == 3) {
      double *res = MEM_ALLOC_ARRAY(arena, double, 3);
      if (res) {
        double *v = args[0].data.vector.data;
        double *axis = args[2].data.vector.data;
        double angle = args[1].data.number;
        double c = cos(angle), s = sin(angle);
        double dot = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
        for (int i = 0; i < 3; i++) {
          res[i] = v[i] * c +
                   (axis[(i + 1) % 3] * v[(i + 2) % 3] - axis[(i + 2) % 3] * v[(i + 1) % 3]) * s +
                   axis[i] * dot * (1.0 - c);
        }
        return exprtk_val_vec(res, 3);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_lookat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR && args[1].type == EXPRTK_VAL_VECTOR &&
      args[2].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 3 && args[1].data.vector.size == 3 &&
        args[2].data.vector.size == 3) {
      double *res = MEM_ALLOC_ARRAY(arena, double, 16);
      if (res) {
        double *eye = args[0].data.vector.data;
        double *center = args[1].data.vector.data;
        double *up = args[2].data.vector.data;
        double f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
        double flen = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (flen > 0) {
          f[0] /= flen;
          f[1] /= flen;
          f[2] /= flen;
        }
        double s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                       f[0] * up[1] - f[1] * up[0]};
        double slen = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (slen > 0) {
          s[0] /= slen;
          s[1] /= slen;
          s[2] /= slen;
        }
        double u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                       s[0] * f[1] - s[1] * f[0]};
        res[0] = s[0];
        res[1] = s[1];
        res[2] = s[2];
        res[3] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
        res[4] = u[0];
        res[5] = u[1];
        res[6] = u[2];
        res[7] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
        res[8] = -f[0];
        res[9] = -f[1];
        res[10] = -f[2];
        res[11] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
        res[12] = 0;
        res[13] = 0;
        res[14] = 0;
        res[15] = 1;
        return exprtk_val_vec(res, 16);
      }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_perspective(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 4) {
    double *res = MEM_ALLOC_ARRAY(arena, double, 16);
    if (res) {
      double fovy = args[0].data.number;
      double aspect = args[1].data.number;
      double zNear = args[2].data.number;
      double zFar = args[3].data.number;
      double f = 1.0 / tan(fovy / 2.0);
      memset(res, 0, 16 * sizeof(double));
      res[0] = f / aspect;
      res[5] = f;
      res[10] = (zFar + zNear) / (zNear - zFar);
      res[11] = (2.0 * zFar * zNear) / (zNear - zFar);
      res[14] = -1.0;
      return exprtk_val_vec(res, 16);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_ortho(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  if (argc == 6) {
    double *res = MEM_ALLOC_ARRAY(arena, double, 16);
    if (res) {
      double l = args[0].data.number, r = args[1].data.number;
      double b = args[2].data.number, t = args[3].data.number;
      double n = args[4].data.number, f = args[5].data.number;
      memset(res, 0, 16 * sizeof(double));
      res[0] = 2.0 / (r - l);
      res[5] = 2.0 / (t - b);
      res[10] = -2.0 / (f - n);
      res[3] = -(r + l) / (r - l);
      res[7] = -(t + b) / (t - b);
      res[11] = -(f + n) / (f - n);
      res[15] = 1.0;
      return exprtk_val_vec(res, 16);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_mat4_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_VECTOR &&
      args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 16 && args[1].data.vector.size == 16) {
      math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
      if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
      return math_matmul_backend(args[0].data.vector.data, args[1].data.vector.data, 4, 4, 4,
                                 1.0f, 0.0f, layout, arena);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_mat3_mul(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_VECTOR &&
      args[1].type == EXPRTK_VAL_VECTOR) {
    if (args[0].data.vector.size == 9 && args[1].data.vector.size == 9) {
      math_matrix_layout_t layout = MATH_MATRIX_COL_MAJOR;
      if (argc == 3 && !math_matrix_layout_arg(args[2], &layout)) return exprtk_val_num(0);
      return math_matmul_backend(args[0].data.vector.data, args[1].data.vector.data, 3, 3, 3,
                                 1.0f, 0.0f, layout, arena);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_acos(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(acos(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_atan(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(atan(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_atan2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(atan2(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sinh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(sinh(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cosh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(cosh(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_tanh(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(tanh(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_pow(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(pow(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_clamp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  if (argc == 3) {
    double lo = args[1].data.number;
    double hi = args[2].data.number;
    if (lo > hi) { double tmp = lo; lo = hi; hi = tmp; }
    
    if (args[0].type == EXPRTK_VAL_NUMBER) {
        double x = args[0].data.number;
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        return exprtk_val_num(x);
    }
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = MEM_ALLOC_ARRAY(arena, double, n);
        if (res) {
            simd_clip(args[0].data.vector.data, res, n, lo, hi);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_lerp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3) {
    double a = args[0].data.number;
    double b = args[1].data.number;
    double t = args[2].data.number;
    return exprtk_val_num(a + (b - a) * t);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_smoothstep(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3) {
    double edge0 = args[0].data.number;
    double edge1 = args[1].data.number;
    double x = args[2].data.number;
    if (fabs(edge1 - edge0) < 1e-15) {
      return exprtk_val_num(x < edge0 ? 0.0 : 1.0);
    }
    double t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0)
      t = 0.0;
    if (t > 1.0)
      t = 1.0;
    return exprtk_val_num(t * t * (3.0 - 2.0 * t));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_radians(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    const double pi = 3.14159265358979323846;
    return exprtk_val_num(args[0].data.number * (pi / 180.0));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_degrees(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    const double pi = 3.14159265358979323846;
    return exprtk_val_num(args[0].data.number * (180.0 / pi));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_is_nan(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num((x != x) ? 1.0 : 0.0);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_is_inf(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num((x == INFINITY || x == -INFINITY) ? 1.0 : 0.0);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_hypot(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(hypot(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log1p(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log1p(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_expm1(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(expm1(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_step(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(args[1].data.number < args[0].data.number ? 0.0 : 1.0);
  return exprtk_val_num(0);
}

static exprtk_value_t fn_fract(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num(x - floor(x));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_saturate(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_NUMBER) {
        double x = args[0].data.number;
        if (x < 0.0) x = 0.0;
        if (x > 1.0) x = 1.0;
        return exprtk_val_num(x);
    }
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = MEM_ALLOC_ARRAY(arena, double, n);
        if (res) {
            simd_clip(args[0].data.vector.data, res, n, 0.0, 1.0);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_inverse_lerp(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 3) {
    double a = args[0].data.number;
    double b = args[1].data.number;
    double x = args[2].data.number;
    double d = b - a;
    if (fabs(d) < 1e-15)
      return exprtk_val_num(0);
    return exprtk_val_num((x - a) / d);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_remap(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 5) {
    double in_min = args[1].data.number;
    double in_max = args[2].data.number;
    double out_min = args[3].data.number;
    double out_max = args[4].data.number;
    double d = in_max - in_min;
    if (fabs(d) < 1e-15)
      return exprtk_val_num(out_min);
    double t = (args[0].data.number - in_min) / d;
    return exprtk_val_num(out_min + (out_max - out_min) * t);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_relu(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    return exprtk_val_num(x > 0.0 ? x : 0.0);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sigmoid(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    if (x >= 0.0) {
      double z = exp(-x);
      return exprtk_val_num(1.0 / (1.0 + z));
    }
    double z = exp(x);
    return exprtk_val_num(z / (1.0 + z));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_softplus(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    if (x > 40.0)
      return exprtk_val_num(x);
    if (x < -40.0)
      return exprtk_val_num(exp(x));
    return exprtk_val_num(log1p(exp(x)));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_copysign(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(copysign(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_cbrt(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1) {
    double x = args[0].data.number;
    if (x == 0.0)
      return exprtk_val_num(0.0);
    return exprtk_val_num(copysign(pow(fabs(x), 1.0 / 3.0), x));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_exp2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(pow(2.0, args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_logn(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2) {
    double x = args[0].data.number;
    double base = args[1].data.number;
    if (x <= 0.0 || base <= 0.0 || fabs(base - 1.0) < 1e-15)
      return exprtk_val_num(0);
    return exprtk_val_num(log(x) / log(base));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log10(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log10(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_log2(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(log2(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_trunc(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 1)
    return exprtk_val_num(trunc(args[0].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_vec_zscore(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_vec(NULL, 0);
    double mean, var;
    simd_mean_variance(args[0].data.vector.data, n, &mean, &var);
    double sd = sqrt(var);
    double *res = MEM_ALLOC_ARRAY(arena, double, n);
    if (res) {
      simd_zscore(args[0].data.vector.data, res, n, mean, sd);
      return exprtk_val_vec(res, n);
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_var(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double mean, var;
    simd_mean_variance(args[0].data.vector.data, args[0].data.vector.size, &mean, &var);
    return exprtk_val_num(var);
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_std(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR) {
    double mean, var;
    simd_mean_variance(args[0].data.vector.data, args[0].data.vector.size, &mean, &var);
    return exprtk_val_num(sqrt(var));
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_sgn(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  if (argc == 1) {
    if (args[0].type == EXPRTK_VAL_NUMBER) {
        double val = args[0].data.number;
        return exprtk_val_num((val > 0) - (val < 0));
    }
    if (args[0].type == EXPRTK_VAL_VECTOR) {
        size_t n = args[0].data.vector.size;
        double *res = MEM_ALLOC_ARRAY(arena, double, n);
        if (res) {
            simd_sign(args[0].data.vector.data, res, n);
            return exprtk_val_vec(res, n);
        }
    }
  }
  return exprtk_val_num(0);
}

static exprtk_value_t fn_any(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
      return exprtk_val_num(simd_any(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env; (void)arena;
  if (argc == 1 && args[0].type == EXPRTK_VAL_VECTOR)
      return exprtk_val_num(simd_all(args[0].data.vector.data, args[0].data.vector.size));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_mod(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2)
    return exprtk_val_num(fmod(args[0].data.number, args[1].data.number));
  return exprtk_val_num(0);
}

static exprtk_value_t fn_rand(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              mem_pool_t *arena) {
  (void)env;
  (void)arena;
  (void)argc;
  (void)args;
  return exprtk_val_num((double)rand() / RAND_MAX);
}

/* ========================================================================= */
/* 3. Module Definition                                                     */
/* ========================================================================= */

static const exprtk_func_entry_t math_entries[] = {
    {"abs", fn_abs},
    {"acos", fn_acos},
    {"all", fn_all},
    {"angle", fn_angle},
    {"any", fn_any},
    {"asin", fn_asin},
    {"avg", fn_avg},
    {"atan2", fn_atan2},
    {"cbrt", fn_cbrt},
    {"ceil", fn_ceil},
    {"clamp", fn_clamp},
    {"copysign", fn_copysign},
    {"cos", fn_cos},
    {"cosh", fn_cosh},
    {"cross", fn_cross},
    {"degrees", fn_degrees},
    {"derivative", fn_derivative},
    {"det2", fn_det2},
    {"det3", fn_det3},
    {"dot", fn_dot},
    {"eig2", fn_eig2},
    {"eig3", fn_eig3},
    {"exp", fn_exp},
    {"exp2", fn_exp2},
    {"expm1", fn_expm1},
    {"fibonacci", fn_fibonacci},
    {"fract", fn_fract},
    {"floor", fn_floor},
    {"gcd", fn_gcd},
    {"geometric_mean", fn_geometric_mean},
    {"harmonic_mean", fn_harmonic_mean},
    {"hypot", fn_hypot},
    {"integrate", fn_integrate},
    {"is_inf", fn_is_inf},
    {"is_nan", fn_is_nan},
    {"inv", fn_inv},
    {"inv_normal_cdf", fn_inv_normal_cdf},
    {"inv2", fn_inv2},
    {"inv3", fn_inv3},
    {"inverse_lerp", fn_inverse_lerp},
    {"kurtosis", fn_kurtosis},
    {"lerp", fn_lerp},
    {"len", fn_len},
    {"log", fn_log},
    {"log10", fn_log10},
    {"log1p", fn_log1p},
    {"log2", fn_log2},
    {"logn", fn_logn},
    {"lookat", fn_lookat},
    {"linalg.cond", fn_linalg_cond},
    {"linalg.det_lu", fn_linalg_det_lu},
    {"linalg.eigh", fn_linalg_eigh},
    {"linalg.eig2", fn_linalg_eig2},
    {"linalg.lu", fn_linalg_lu},
    {"linalg.lu_solve", fn_linalg_lu_solve},
    {"linalg.lstsq", fn_linalg_lstsq},
    {"linalg.pinv", fn_linalg_pinv},
    {"linalg.qr", fn_linalg_qr},
    {"linalg.rank", fn_linalg_rank},
    {"linalg.solve_cholesky", fn_linalg_solve_cholesky},
    {"linalg.svd", fn_linalg_svd},
    {"linalg.svd2", fn_linalg_svd},
    {"mat3_mul", fn_mat3_mul},
    {"mat4_mul", fn_mat4_mul},
    {"mat_add", fn_mat_add},
    {"mat_add_col", fn_mat_add_col},
    {"mat_add_row", fn_mat_add_row},
    {"mat_argmax", fn_mat_argmax},
    {"mat_argmin", fn_mat_argmin},
    {"mat_cholesky", fn_mat_cholesky},
    {"mat_col", fn_mat_col},
    {"mat_cols", fn_mat_cols},
    {"mat_cond", fn_mat_cond},
    {"mat_copy", fn_mat_copy},
    {"mat_det", fn_mat_det},
    {"mat_diag", fn_mat_diag},
    {"mat_div_col", fn_mat_div_col},
    {"mat_div_row", fn_mat_div_row},
    {"mat_dot", fn_mat_dot},
    {"mat_eye", fn_mat_eye},
    {"mat_eye_like", fn_mat_eye_like},
    {"mat_flatten", fn_mat_flatten},
    {"mat_full", fn_mat_full},
    {"mat_gemm", fn_mat_gemm},
    {"mat_hadamard", fn_mat_hadamard},
    {"mat_identity", fn_mat_identity},
    {"mat_info", fn_mat_info},
    {"mat_inv", fn_mat_inv},
    {"mat_max", fn_mat_max},
    {"mat_mean", fn_mat_mean},
    {"mat_min", fn_mat_min},
    {"mat_mul_col", fn_mat_mul_col},
    {"mat_mul_row", fn_mat_mul_row},
    {"mat_norm", fn_mat_norm},
    {"mat_ones", fn_mat_ones},
    {"mat_outer", fn_mat_outer},
    {"mat_rank", fn_mat_rank},
    {"mat_row", fn_mat_row},
    {"mat_rows", fn_mat_rows},
    {"mat_reshape", fn_mat_reshape},
    {"mat_shape", fn_mat_shape},
    {"mat_slice", fn_mat_slice},
    {"mat_solve", fn_mat_solve},
    {"mat_solve_tri", fn_mat_solve_tri},
    {"mat_slogdet", fn_mat_slogdet},
    {"mat_scale", fn_mat_scale},
    {"mat_std", fn_mat_std},
    {"mat_sub", fn_mat_sub},
    {"mat_sub_col", fn_mat_sub_col},
    {"mat_sub_row", fn_mat_sub_row},
    {"mat_sum", fn_mat_sum},
    {"mat_trace", fn_mat_trace},
    {"mat_transpose", fn_mat_transpose},
    {"mat_udu", fn_mat_udu},
    {"mat_var", fn_mat_var},
    {"mat_zeros", fn_mat_zeros},
    {"matmul", fn_matmul},
    {"max", fn_max},
    {"median", fn_median},
    {"min", fn_min},
    {"mod", fn_mod},
    {"norm", fn_norm},
    {"normal_rand", fn_normal_rand},
    {"ols_fit", fn_ols_fit},
    {"ortho", fn_ortho},
    {"percentile", fn_percentile},
    {"perspective", fn_perspective},
    {"pow", fn_pow},
    {"proj", fn_proj},
    {"rand", fn_rand},
    {"relu", fn_relu},
    {"remap", fn_remap},
    {"rotate", fn_rotate_vec},
    {"radians", fn_radians},
    {"round", fn_round},
    {"sgn", fn_sgn},
    {"sin", fn_sin},
    {"sigmoid", fn_sigmoid},
    {"sinh", fn_sinh},
    {"size", fn_len},
    {"saturate", fn_saturate},
    {"skewness", fn_skewness},
    {"softplus", fn_softplus},
    {"smoothstep", fn_smoothstep},
    {"sqrt", fn_sqrt},
    {"step", fn_step},
    {"sum", fn_sum},
    {"std", fn_std},
    {"stats.normal_cdf", fn_stats_normal_cdf},
    {"stats.normal_pdf", fn_stats_normal_pdf},
    {"stats.normal_quantile", fn_stats_normal_quantile},
    {"stats.t_cdf", fn_stats_t_cdf},
    {"stats.t_pdf", fn_stats_t_pdf},
    {"stats.t_quantile", fn_stats_t_quantile},
    {"stats.t_test_1samp", fn_stats_t_test_1samp},
    {"tan", fn_tan},
    {"tanh", fn_tanh},
    {"trace2", fn_trace2},
    {"var", fn_var},
    {"zscore", fn_vec_zscore},
    {"transform_create", fn_transform_create},
    {"transform_inv", fn_transform_inv},
    {"transform_mul", fn_transform_mul},
    {"transform_pt", fn_transform_pt},
    {"transpose", fn_transpose},
    {"trunc", fn_trunc},
    {"vec2_perp", fn_vec2_perp},
    {"vec_add", fn_vec_add},
    {"vec_div", fn_vec_div},
    {"vec_mul", fn_vec_mul},
    {"vec_reciprocal", fn_vec_reciprocal},
    {"vec_normalize", fn_vec_normalize},
    {"vec_scale", fn_vec_scale},
    {"vec_sqr", fn_vec_sqr},
    {"vec_sub", fn_vec_sub},
    {"vector_find_all", fn_vector_find_all},
    {"vector_find_value", fn_vector_find_value},
    {"vec.all", fn_all},
    {"vec.any", fn_any},
    {"vec.avg", fn_avg},
    {"vec.concat", fn_vec_concat},
    {"vec.cumsum", fn_vec_cumsum},
    {"vec.diff", fn_vec_diff},
    {"vec.find", fn_vector_find_value},
    {"vec.len", fn_len},
    {"vec.max", fn_max},
    {"vec.min", fn_min},
    {"vec.range", fn_vec_range},
    {"vec.reverse", fn_vec_reverse},
    {"vec.sort", fn_vec_sort},
    {"vec.sort_desc", fn_vec_sort_desc},
    {"vec.std", fn_std},
    {"vec.sum", fn_sum},
    {"vec.unique", fn_vec_unique},
    {"vec.var", fn_var},
    {"vec.div", fn_vec_div},
    {"vec.mul", fn_vec_mul},
    {"vec.recipro", fn_vec_reciprocal},
    {"vec.sqr", fn_vec_sqr},
    {"vec.zscore", fn_vec_zscore},
    {"matrix.add", fn_mat_add},
    {"matrix.add_col", fn_mat_add_col},
    {"matrix.add_row", fn_mat_add_row},
    {"matrix.argmax", fn_mat_argmax},
    {"matrix.argmin", fn_mat_argmin},
    {"matrix.cholesky", fn_mat_cholesky},
    {"matrix.col", fn_mat_col},
    {"matrix.cond", fn_mat_cond},
    {"matrix.copy", fn_mat_copy},
    {"matrix.det", fn_mat_det},
    {"matrix.det2", fn_det2},
    {"matrix.det3", fn_det3},
    {"matrix.diag", fn_mat_diag},
    {"matrix.div_col", fn_mat_div_col},
    {"matrix.div_row", fn_mat_div_row},
    {"matrix.dot", fn_mat_dot},
    {"matrix.eye", fn_mat_eye},
    {"matrix.eye_like", fn_mat_eye_like},
    {"matrix.flatten", fn_mat_flatten},
    {"matrix.full", fn_mat_full},
    {"matrix.gemm", fn_mat_gemm},
    {"matrix.hadamard", fn_mat_hadamard},
    {"matrix.identity", fn_mat_identity},
    {"matrix.info", fn_mat_info},
    {"matrix.inv", fn_mat_inv},
    {"matrix.max", fn_mat_max},
    {"matrix.mean", fn_mat_mean},
    {"matrix.min", fn_mat_min},
    {"matrix.mul_col", fn_mat_mul_col},
    {"matrix.mul_row", fn_mat_mul_row},
    {"matrix.norm", fn_mat_norm},
    {"matrix.ones", fn_mat_ones},
    {"matrix.outer", fn_mat_outer},
    {"matrix.rank", fn_mat_rank},
    {"matrix.inv2", fn_inv2},
    {"matrix.inv3", fn_inv3},
    {"matrix.mat3_mul", fn_mat3_mul},
    {"matrix.mat4_mul", fn_mat4_mul},
    {"matrix.matmul", fn_matmul},
    {"matrix.mul", fn_mat_hadamard},
    {"matrix.row", fn_mat_row},
    {"matrix.rows", fn_mat_rows},
    {"matrix.cols", fn_mat_cols},
    {"matrix.reshape", fn_mat_reshape},
    {"matrix.shape", fn_mat_shape},
    {"matrix.slice", fn_mat_slice},
    {"matrix.solve", fn_mat_solve},
    {"matrix.solve_tri", fn_mat_solve_tri},
    {"matrix.slogdet", fn_mat_slogdet},
    {"matrix.scale", fn_mat_scale},
    {"matrix.std", fn_mat_std},
    {"matrix.sub", fn_mat_sub},
    {"matrix.sub_col", fn_mat_sub_col},
    {"matrix.sub_row", fn_mat_sub_row},
    {"matrix.sum", fn_mat_sum},
    {"matrix.trace", fn_mat_trace},
    {"matrix.transpose", fn_mat_transpose},
    {"matrix.udu", fn_mat_udu},
    {"matrix.var", fn_mat_var},
    {"matrix.variance", fn_mat_var},
    {"matrix.zeros", fn_mat_zeros},
};

static const exprtk_module_t math_module = {"math", math_entries,
                                            sizeof(math_entries) / sizeof(math_entries[0])};

const exprtk_module_t *exprtk_module_math(void) { return &math_module; }
