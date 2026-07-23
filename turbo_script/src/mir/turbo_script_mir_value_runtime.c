/* Value runtime evaluation helpers - MIR Backend
 * Extracted from turbo_script_mir.c.
 */

#include "../turbo_script_internal.h"
#include "turbo_script_mir_internal.h"
#include "exprtk_class.h"
#include "exprtk_grammar.h"
#include "exprtk_module.h"
#include "turbo_str.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

exprtk_env_t *exprtk_env_snapshot(exprtk_env_t *env);
exprtk_value_t throw_error(exprtk_env_t *env, const exprtk_node_t *node, const char *fmt, ...);
exprtk_value_t throw_method_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                         const char *method_name, exprtk_func_t *method);
exprtk_value_t throw_field_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                        const char *field_name, int access_level);
const char *type_name(int type);
int values_match(exprtk_value_t lhs, exprtk_value_t rhs);
int exprtk_datetime_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_date_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_time_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_duration_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_decimal_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
exprtk_class_t *eval_current_class(exprtk_env_t *env);
exprtk_func_t *eval_find_constructor_typed(exprtk_class_t *klass, size_t argc,
                                           exprtk_value_t *args);
int can_access_method(exprtk_env_t *env, exprtk_func_t *method, const exprtk_node_t *object_node);
int can_access_declared_field(exprtk_env_t *env, exprtk_class_t *owner_class, int access_level,
                              const exprtk_node_t *object_node, const char *field_name);
exprtk_value_t eval_script_function(exprtk_func_t *func, size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *parent_env, exprtk_env_t *caller_env);
exprtk_value_t eval_class_def_node(const exprtk_node_t *node, exprtk_env_t *env);
exprtk_value_t eval_class_instantiation(const char *class_name, exprtk_node_t **arg_nodes,
                                        size_t arg_count, exprtk_env_t *env);
time_t turbo_datetime_to_time(const turbo_datetime_t *dt);
int turbo_datetime_format_rfc822(time_t t, char *buf, size_t buf_size);

static void ts_mir_value_arg_error(exprtk_env_t *env, exprtk_node_t *node);
static int ts_mir_runtime_value_arg(exprtk_node_t *node, exprtk_env_t *env,
                                    exprtk_value_t *out);

static int ts_mir_value_is_numeric(exprtk_value_t val) {
  return val.type == EXPRTK_VAL_INTEGER || val.type == EXPRTK_VAL_NUMBER ||
         val.type == EXPRTK_VAL_BOOL;
}

static int ts_mir_null_compare_value(exprtk_value_t lhs, exprtk_value_t rhs, int op,
                                     exprtk_value_t *out) {
  int equal = 0;

  if (op != exprtk_TOKEN_EQ && op != exprtk_TOKEN_NE) return 0;
  if (lhs.type != EXPRTK_VAL_NULL && rhs.type != EXPRTK_VAL_NULL) return 0;

  equal = lhs.type == EXPRTK_VAL_NULL && rhs.type == EXPRTK_VAL_NULL;
  *out = exprtk_val_num(op == exprtk_TOKEN_EQ ? (double)equal : (double)!equal);
  return 1;
}

/* =========================================================================
 *  Variable Bridge Functions
 * ========================================================================= */

double ts_mir_destructure_var(void *ctx_ptr, void *target_node_ptr, const char *value_name,
                              int64_t is_constant) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *target = (exprtk_node_t *)target_node_ptr;
  exprtk_value_t rhs;

  if (!ctx || !target || !value_name) return 0.0;

  rhs = exprtk_env_get(&ctx->env, value_name);
  eval_destructure(target, rhs, &ctx->env, (int)is_constant);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(rhs);
}

double ts_mir_destructure_vector(void *ctx_ptr, void *target_node_ptr, int64_t is_constant,
                                 int64_t count, double *values) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *target = (exprtk_node_t *)target_node_ptr;
  exprtk_value_t rhs;

  if (!ctx || !target || count < 0) return 0.0;

  rhs = exprtk_val_vec(values, (size_t)count);
  eval_destructure(target, rhs, &ctx->env, (int)is_constant);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(rhs);
}

double ts_mir_map_value_assign(void *ctx_ptr, const char *target_name, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t map;

  if (!ctx || !target_name || !node || node->type != EXPRTK_NODE_MAP_LITERAL) return 0.0;

  map = exprtk_val_map();
  for (size_t i = 0; i < node->data.map_literal.count; ++i) {
    exprtk_value_t value;
    const char *key = node->data.map_literal.keys[i];
    exprtk_node_t *value_node = node->data.map_literal.values[i];

    if (!key) continue;
    if (!value_node || !ts_mir_runtime_value_arg(value_node, &ctx->env, &value)) {
      if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
        ts_mir_value_arg_error(&ctx->env, value_node);
      ts_mir_promote_env_error(ctx);
      exprtk_map_free(&map);
      return 0.0;
    }
    if (exprtk_map_set(&map, key, value) != 0) {
      exprtk_value_destroy(&value);
      exprtk_value_destroy(&map);
      return 0.0;
    }
    exprtk_value_destroy(&value);
  }

  exprtk_env_set(&ctx->env, target_name, map);
  exprtk_map_free(&map);
  return 0.0;
}

double ts_mir_template_assign(void *ctx_ptr, const char *target_name, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t val;

  if (!ctx || !target_name || !node || node->type != EXPRTK_NODE_TEMPLATE_STRING)
    return 0.0;

  if (!ts_mir_runtime_value_arg(node, &ctx->env, &val)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }
  exprtk_env_set(&ctx->env, target_name, val);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(val);
}

/* =========================================================================
 *  Function Call Bridge Functions
 * ========================================================================= */

static int ts_mir_value_array_grow(exprtk_value_t **vals, size_t *cap, size_t needed) {
  exprtk_value_t *new_vals = NULL;
  size_t new_cap = 0;

  if (!vals || !cap) return 0;
  if (needed <= *cap) return 1;

  new_cap = *cap ? *cap : 4;
  while (new_cap < needed) new_cap *= 2;

  new_vals = (exprtk_value_t *)realloc(*vals, new_cap * sizeof(*new_vals));
  if (!new_vals) return 0;
  *vals = new_vals;
  *cap = new_cap;
  return 1;
}

static void ts_mir_value_arg_error(exprtk_env_t *env, exprtk_node_t *node) {
  if (!env) return;
  if (env->error_msg[0] != '\0') return;
  env->aborted = 1;
  if (node && node->type == EXPRTK_NODE_MEMBER_CALL) {
    snprintf(env->error_msg, sizeof(env->error_msg),
             "MIR runtime error: unsupported runtime member call '%s' on node type %d",
             node->data.member_call.method ? node->data.member_call.method : "<null>",
             node->data.member_call.object ? (int)node->data.member_call.object->type : -1);
    return;
  }
  snprintf(env->error_msg, sizeof(env->error_msg),
           "MIR runtime error: unsupported runtime value node type %d",
           node ? (int)node->type : -1);
}

static int ts_mir_runtime_value_arg(exprtk_node_t *node, exprtk_env_t *env,
                                 exprtk_value_t *out);
static exprtk_value_t *ts_mir_runtime_call_value_args(exprtk_node_t *call_node, exprtk_env_t *env,
                                                   size_t *out_count);

static void ts_mir_runtime_value_args_free(exprtk_value_t *args, size_t count) {
  exprtk_values_destroy(args, count);
  free(args);
}

static int ts_mir_runtime_value_text(exprtk_value_t value, char *buf, size_t buf_size,
                                     const char **out_data, size_t *out_len) {
  if (!out_data || !out_len) return 0;
  if (value.type == EXPRTK_VAL_STRING) {
    *out_data = value.data.string.data;
    *out_len = value.data.string.len;
    return 1;
  }
  if (value.type == EXPRTK_VAL_INTEGER) {
    int n = snprintf(buf, buf_size, "%lld", (long long)value.data.integer);
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
  }
  if (value.type == EXPRTK_VAL_BOOL) {
    *out_data = value.data.boolean ? "true" : "false";
    *out_len = value.data.boolean ? 4 : 5;
    return 1;
  }
  if (value.type == EXPRTK_VAL_BYTES) {
    int n = snprintf(buf, buf_size, "bytes(%zu)", value.data.bytes.len);
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
  }
  if (value.type == EXPRTK_VAL_UUID) {
    if (turbo_uuid_format(&value.data.uuid, buf, buf_size) != TURBO_OK) return 0;
    *out_data = buf;
    *out_len = strlen(buf);
    return 1;
  }
  if (value.type == EXPRTK_VAL_DATETIME) {
    time_t ts = turbo_datetime_to_time(&value.data.datetime);
    if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, buf, buf_size) < 0) return 0;
    *out_data = buf;
    *out_len = strlen(buf);
    return 1;
  }
  if (value.type == EXPRTK_VAL_DATE) {
    int n = snprintf(buf, buf_size, "%04d-%02d-%02d", value.data.date.year,
                     value.data.date.month, value.data.date.day);
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
  }
  if (value.type == EXPRTK_VAL_TIME) {
    int n;
    if (value.data.time.millisecond > 0)
      n = snprintf(buf, buf_size, "%02d:%02d:%02d.%03d", value.data.time.hour,
                   value.data.time.minute, value.data.time.second,
                   value.data.time.millisecond);
    else
      n = snprintf(buf, buf_size, "%02d:%02d:%02d", value.data.time.hour,
                   value.data.time.minute, value.data.time.second);
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
  }
  if (value.type == EXPRTK_VAL_DURATION) {
    int64_t rem = value.data.duration_ms < 0 ? -value.data.duration_ms
                                             : value.data.duration_ms;
    int64_t h = rem / 3600000;
    int64_t m;
    int64_t s;
    int n;
    rem %= 3600000;
    m = rem / 60000;
    rem %= 60000;
    s = rem / 1000;
    rem %= 1000;
    n = snprintf(buf, buf_size, "%s%lld:%02lld:%02lld.%03lld",
                 value.data.duration_ms < 0 ? "-" : "", (long long)h,
                 (long long)m, (long long)s, (long long)rem);
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
  }
  if (value.type == EXPRTK_VAL_DECIMAL) {
    exprtk_decimal_t dec = value.data.decimal;
    char digits[32];
    char *p = digits + sizeof(digits);
    uint64_t mag;
    size_t digit_count;
    size_t pos = 0;
    int negative;
    while (dec.scale > 0 && dec.mantissa % 10 == 0) {
      dec.mantissa /= 10;
      dec.scale--;
    }
    if (dec.mantissa == 0) dec.scale = 0;
    negative = dec.mantissa < 0;
    mag = negative ? (uint64_t)(-(dec.mantissa + 1)) + 1ULL : (uint64_t)dec.mantissa;
    *--p = '\0';
    do {
      *--p = (char)('0' + (mag % 10ULL));
      mag /= 10ULL;
    } while (mag != 0);
    digit_count = strlen(p);
    if (negative) {
      if (pos + 1 >= buf_size) return 0;
      buf[pos++] = '-';
    }
    if (dec.scale == 0) {
      if (pos + digit_count >= buf_size) return 0;
      memcpy(buf + pos, p, digit_count + 1);
    } else if ((size_t)dec.scale >= digit_count) {
      size_t zeros = (size_t)dec.scale - digit_count;
      if (pos + 2 + zeros + digit_count >= buf_size) return 0;
      buf[pos++] = '0';
      buf[pos++] = '.';
      while (zeros-- > 0) buf[pos++] = '0';
      memcpy(buf + pos, p, digit_count);
      pos += digit_count;
      buf[pos] = '\0';
    } else {
      size_t whole = digit_count - (size_t)dec.scale;
      if (pos + digit_count + 1 >= buf_size) return 0;
      memcpy(buf + pos, p, whole);
      pos += whole;
      buf[pos++] = '.';
      memcpy(buf + pos, p + whole, (size_t)dec.scale);
      pos += (size_t)dec.scale;
      buf[pos] = '\0';
    }
    *out_data = buf;
    *out_len = strlen(buf);
    return 1;
  }
  if (value.type == EXPRTK_VAL_NULL) {
    *out_data = "null";
    *out_len = 4;
    return 1;
  }
  {
    int n = snprintf(buf, buf_size, "%g", ts_mir_numeric_value(value));
    if (n < 0) return 0;
    *out_data = buf;
    *out_len = (size_t)n;
    return 1;
  }
}

static int ts_mir_eval_value_binary(exprtk_node_t *node, exprtk_env_t *env,
                                    exprtk_value_t *out) {
  exprtk_value_t lhs;
  exprtk_value_t rhs;
  double l = 0.0;
  double r = 0.0;

  if (!node || !env || !out) return 0;
  if (!node->data.binary.left) {
    if (!ts_mir_runtime_value_arg(node->data.binary.right, env, &rhs)) return 0;
    r = ts_mir_numeric_value(rhs);
    switch (node->data.binary.op) {
    case exprtk_TOKEN_PLUS:
      *out = exprtk_val_num(r);
      return 1;
    case exprtk_TOKEN_MINUS:
      *out = exprtk_val_num(-r);
      return 1;
    case exprtk_TOKEN_NOT:
      *out = exprtk_val_num(fabs(r) <= 1e-9 ? 1.0 : 0.0);
      return 1;
    default:
      return 0;
    }
  }

  if (!ts_mir_runtime_value_arg(node->data.binary.left, env, &lhs)) {
    ts_mir_value_arg_error(env, node->data.binary.left);
    return 0;
  }
  if (!ts_mir_runtime_value_arg(node->data.binary.right, env, &rhs)) {
    ts_mir_value_arg_error(env, node->data.binary.right);
    return 0;
  }

  if (ts_mir_null_compare_value(lhs, rhs, node->data.binary.op, out)) {
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_BOOL && rhs.type == EXPRTK_VAL_BOOL &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = lhs.data.boolean == rhs.data.boolean;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_BYTES && rhs.type == EXPRTK_VAL_BYTES &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = lhs.data.bytes.len == rhs.data.bytes.len &&
                (lhs.data.bytes.len == 0 ||
                 memcmp(lhs.data.bytes.data, rhs.data.bytes.data, lhs.data.bytes.len) == 0);
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_UUID && rhs.type == EXPRTK_VAL_UUID &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = memcmp(lhs.data.uuid.bytes, rhs.data.uuid.bytes,
                       sizeof(lhs.data.uuid.bytes)) == 0;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_DATETIME && rhs.type == EXPRTK_VAL_DATETIME &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = memcmp(&lhs.data.datetime, &rhs.data.datetime, sizeof(lhs.data.datetime)) == 0;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_DATE && rhs.type == EXPRTK_VAL_DATE &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = lhs.data.date.year == rhs.data.date.year &&
                lhs.data.date.month == rhs.data.date.month &&
                lhs.data.date.day == rhs.data.date.day;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_TIME && rhs.type == EXPRTK_VAL_TIME &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = lhs.data.time.hour == rhs.data.time.hour &&
                lhs.data.time.minute == rhs.data.time.minute &&
                lhs.data.time.second == rhs.data.time.second &&
                lhs.data.time.millisecond == rhs.data.time.millisecond;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_DURATION && rhs.type == EXPRTK_VAL_DURATION &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = lhs.data.duration_ms == rhs.data.duration_ms;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (lhs.type == EXPRTK_VAL_DECIMAL && rhs.type == EXPRTK_VAL_DECIMAL &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    exprtk_decimal_t ldec = lhs.data.decimal;
    exprtk_decimal_t rdec = rhs.data.decimal;
    int equal;
    while (ldec.scale > 0 && ldec.mantissa % 10 == 0) {
      ldec.mantissa /= 10;
      ldec.scale--;
    }
    while (rdec.scale > 0 && rdec.mantissa % 10 == 0) {
      rdec.mantissa /= 10;
      rdec.scale--;
    }
    if (ldec.mantissa == 0) ldec.scale = 0;
    if (rdec.mantissa == 0) rdec.scale = 0;
    equal = ldec.mantissa == rdec.mantissa && ldec.scale == rdec.scale;
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if ((lhs.type == EXPRTK_VAL_STRING || lhs.type == EXPRTK_VAL_NULL) &&
      (rhs.type == EXPRTK_VAL_STRING || rhs.type == EXPRTK_VAL_NULL) &&
      (node->data.binary.op == exprtk_TOKEN_EQ || node->data.binary.op == exprtk_TOKEN_NE)) {
    int equal = 0;
    if (lhs.type == EXPRTK_VAL_NULL || rhs.type == EXPRTK_VAL_NULL) {
      equal = lhs.type == rhs.type;
    } else {
      equal = lhs.data.string.len == rhs.data.string.len &&
              memcmp(lhs.data.string.data, rhs.data.string.data, lhs.data.string.len) == 0;
    }
    *out = exprtk_val_num(node->data.binary.op == exprtk_TOKEN_EQ ? (double)equal
                                                                  : (double)!equal);
    return 1;
  }

  if (node->data.binary.op == exprtk_TOKEN_PLUS &&
      (lhs.type == EXPRTK_VAL_STRING || rhs.type == EXPRTK_VAL_STRING)) {
    char l_buf[32];
    char r_buf[32];
    const char *l_data = NULL;
    const char *r_data = NULL;
    size_t l_len = 0;
    size_t r_len = 0;
    tstr_t data = NULL;
    exprtk_value_t promoted = { .type = EXPRTK_VAL_NULL };

    if (!ts_mir_runtime_value_text(lhs, l_buf, sizeof(l_buf), &l_data, &l_len) ||
        !ts_mir_runtime_value_text(rhs, r_buf, sizeof(r_buf), &r_data, &r_len)) {
      return 0;
    }

    if (l_len > SIZE_MAX - r_len) return 0;
    data = tstr_new_len(NULL, l_len + r_len);
    if (!data) return 0;
    if (l_len > 0) memcpy(data, l_data, l_len);
    if (r_len > 0) memcpy(data + l_len, r_data, r_len);
    data[l_len + r_len] = '\0';
    if (exprtk_value_copy_to_env(
            exprtk_val_str(tstr_v_from_buf(data, l_len + r_len)), env,
            &promoted) != 0) {
      tstr_free(data);
      return 0;
    }
    tstr_free(data);
    exprtk_value_destroy(&lhs);
    exprtk_value_destroy(&rhs);
    *out = promoted;
    return 1;
  }

  if (!ts_mir_value_is_numeric(lhs) || !ts_mir_value_is_numeric(rhs)) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "MIR runtime error: unsupported runtime binary op %d with value types %d and %d",
               node->data.binary.op, lhs.type, rhs.type);
    }
    return 0;
  }

  l = ts_mir_numeric_value(lhs);
  r = ts_mir_numeric_value(rhs);
  switch (node->data.binary.op) {
  case exprtk_TOKEN_PLUS:
    *out = exprtk_val_num(l + r);
    return 1;
  case exprtk_TOKEN_MINUS:
    *out = exprtk_val_num(l - r);
    return 1;
  case exprtk_TOKEN_MULTIPLY:
    *out = exprtk_val_num(l * r);
    return 1;
  case exprtk_TOKEN_DIVIDE:
    *out = exprtk_val_num(r == 0.0 ? 0.0 : l / r);
    return 1;
  case exprtk_TOKEN_MOD:
    *out = exprtk_val_num(fmod(l, r));
    return 1;
  case exprtk_TOKEN_POWER:
    *out = exprtk_val_num(pow(l, r));
    return 1;
  case exprtk_TOKEN_EQ:
    *out = exprtk_val_num(fabs(l - r) < 1e-9 ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_NE:
    *out = exprtk_val_num(fabs(l - r) >= 1e-9 ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_LT:
    *out = exprtk_val_num(l < r ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_LE:
    *out = exprtk_val_num(l <= r ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_GT:
    *out = exprtk_val_num(l > r ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_GE:
    *out = exprtk_val_num(l >= r ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_AND:
    *out = exprtk_val_num(fabs(l) > 1e-9 && fabs(r) > 1e-9 ? 1.0 : 0.0);
    return 1;
  case exprtk_TOKEN_OR:
    *out = exprtk_val_num(fabs(l) > 1e-9 || fabs(r) > 1e-9 ? 1.0 : 0.0);
    return 1;
  default:
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "MIR runtime error: unsupported runtime binary op %d with value types %d and %d",
               node->data.binary.op, lhs.type, rhs.type);
    }
    return 0;
  }
}

static int ts_mir_template_append(tstr_t *buf, const char *data, size_t data_len) {
  tstr_t next;

  if (!buf || !*buf || (!data && data_len > 0)) return 0;
  next = tstr_cat_len(*buf, data, data_len);
  if (!next) return 0;
  *buf = next;
  return 1;
}

static int ts_mir_template_append_value(tstr_t *buf, exprtk_value_t value) {
  char num_buf[64];
  int n;

  switch (value.type) {
  case EXPRTK_VAL_INTEGER:
    n = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value.data.integer);
    return n >= 0 && ts_mir_template_append(buf, num_buf, (size_t)n);
  case EXPRTK_VAL_NUMBER:
    n = snprintf(num_buf, sizeof(num_buf), "%g", value.data.number);
    return n >= 0 && ts_mir_template_append(buf, num_buf, (size_t)n);
  case EXPRTK_VAL_BOOL:
    return value.data.boolean
               ? ts_mir_template_append(buf, "true", 4)
               : ts_mir_template_append(buf, "false", 5);
  case EXPRTK_VAL_STRING:
    return ts_mir_template_append(buf, value.data.string.data, value.data.string.len);
  case EXPRTK_VAL_BYTES:
    n = snprintf(num_buf, sizeof(num_buf), "bytes(%zu)", value.data.bytes.len);
    return n >= 0 && ts_mir_template_append(buf, num_buf, (size_t)n);
  case EXPRTK_VAL_UUID:
    if (turbo_uuid_format(&value.data.uuid, num_buf, sizeof(num_buf)) != TURBO_OK) return 0;
    return ts_mir_template_append(buf, num_buf, strlen(num_buf));
  case EXPRTK_VAL_DATETIME: {
    time_t ts = turbo_datetime_to_time(&value.data.datetime);
    if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, num_buf, sizeof(num_buf)) < 0) return 0;
    return ts_mir_template_append(buf, num_buf, strlen(num_buf));
  }
  case EXPRTK_VAL_DATE:
  case EXPRTK_VAL_TIME:
  case EXPRTK_VAL_DURATION:
  case EXPRTK_VAL_DECIMAL: {
    const char *text = NULL;
    size_t text_len = 0;
    if (!ts_mir_runtime_value_text(value, num_buf, sizeof(num_buf), &text, &text_len)) return 0;
    return ts_mir_template_append(buf, text, text_len);
  }
  case EXPRTK_VAL_NULL:
    return ts_mir_template_append(buf, "null", 4);
  case EXPRTK_VAL_VECTOR:
    return ts_mir_template_append(buf, "[vector]", 8);
  case EXPRTK_VAL_MAP:
    return ts_mir_template_append(buf, "[map]", 5);
  case EXPRTK_VAL_OBJECT:
    return ts_mir_template_append(buf, "[object]", 8);
  case EXPRTK_VAL_LIST:
    return ts_mir_template_append(buf, "[list]", 6);
  default:
    return ts_mir_template_append(buf, "", 0);
  }
}

static int ts_mir_runtime_template_value(exprtk_node_t *node, exprtk_env_t *env,
                                      exprtk_value_t *out) {
  const char *str;
  size_t len;
  size_t i = 0;
  tstr_t result = NULL;
  exprtk_value_t promoted = { .type = EXPRTK_VAL_NULL };

  if (!node || !env || !out || node->type != EXPRTK_NODE_TEMPLATE_STRING) return 0;
  str = node->data.template_string.template_str;
  len = node->data.template_string.len;
  if (!str) return 0;
  result = tstr_new();
  if (!result) return 0;

  while (i < len) {
    if (str[i] == '$' && i + 1 < len && str[i + 1] == '{') {
      size_t expr_start;
      int depth = 1;
      i += 2;
      expr_start = i;
      while (i < len && depth > 0) {
        if (str[i] == '{')
          depth++;
        else if (str[i] == '}')
          depth--;
        i++;
      }
      if (depth != 0) {
        tstr_free(result);
        return 0;
      }
      if (i - 1 > expr_start) {
        size_t expr_len = i - 1 - expr_start;
        char *expr_str = (char *)malloc(expr_len + 1);
        exprtk_node_t *expr_node;
        exprtk_value_t expr_value = {0};
        int ok;
        if (!expr_str) {
          tstr_free(result);
          return 0;
        }
        memcpy(expr_str, str + expr_start, expr_len);
        expr_str[expr_len] = '\0';
        expr_node = exprtk_parse(expr_str, expr_len);
        free(expr_str);
        if (!expr_node) {
          tstr_free(result);
          return 0;
        }
        ok = ts_mir_runtime_value_arg(expr_node, env, &expr_value) &&
             ts_mir_template_append_value(&result, expr_value);
        exprtk_value_destroy(&expr_value);
        exprtk_free(expr_node);
        if (!ok) {
          tstr_free(result);
          return 0;
        }
      }
      continue;
    }

    if (!ts_mir_template_append(&result, &str[i], 1)) {
      tstr_free(result);
      return 0;
    }
    i++;
  }

  if (exprtk_value_copy_to_env(
          exprtk_val_str(tstr_v_from_buf(result, tstr_len(result))), env,
          &promoted) != 0) {
    tstr_free(result);
    return 0;
  }
  tstr_free(result);
  *out = promoted;
  return 1;
}

static int ts_mir_runtime_try_catch_value(exprtk_node_t *node, exprtk_env_t *env,
                                       exprtk_value_t *out) {
  exprtk_value_t result;

  if (!node || !env || !out || node->type != EXPRTK_NODE_TRY_CATCH) return 0;
  if (!ts_mir_runtime_value_arg(node->data.try_catch.try_body, env, &result)) return 0;

  if (env->flow == exprtk_FLOW_THROW) {
    exprtk_env_t catch_env;
    env->flow = exprtk_FLOW_NORMAL;
    exprtk_env_init_child(&catch_env, env);
    catch_env.eval_node = env->eval_node;
    catch_env.exec_script_body = env->exec_script_body;
    catch_env.max_recursion = env->max_recursion;
    catch_env.curr_recursion = env->curr_recursion;
    catch_env.max_loop_iterations = env->max_loop_iterations;
    catch_env.curr_loop_iterations = env->curr_loop_iterations;
    catch_env.max_nodes = env->max_nodes;
    catch_env.curr_nodes = env->curr_nodes;

    if (node->data.try_catch.catch_var) {
      exprtk_env_set_local(&catch_env, node->data.try_catch.catch_var, env->error_value);
    }

    if (!ts_mir_runtime_value_arg(node->data.try_catch.catch_body, &catch_env, &result)) {
      exprtk_env_free(&catch_env);
      return 0;
    }
    result = exprtk_value_clone_to_env(result, env);
    env->curr_nodes = catch_env.curr_nodes;
    env->curr_loop_iterations = catch_env.curr_loop_iterations;
    env->aborted = catch_env.aborted;
    if (catch_env.flow != exprtk_FLOW_NORMAL) {
      env->flow = catch_env.flow;
      env->return_value = exprtk_value_clone_to_env(catch_env.return_value, env);
      env->error_value = exprtk_value_clone_to_env(catch_env.error_value, env);
    }
    exprtk_env_free(&catch_env);
  }

  *out = result;
  return 1;
}

static exprtk_value_t ts_mir_zero_value(void) {
  return exprtk_val_num(0.0);
}

static int ts_mir_value_truthy(exprtk_value_t value) {
  if (value.type == EXPRTK_VAL_BOOL) return value.data.boolean != 0;
  if (value.type == EXPRTK_VAL_INTEGER) return llabs(value.data.integer) > 0;
  if (value.type == EXPRTK_VAL_NUMBER) return fabs(value.data.number) > 1e-9;
  if (value.type == EXPRTK_VAL_STRING) return value.data.string.len > 0;
  if (value.type == EXPRTK_VAL_BYTES) return value.data.bytes.len > 0;
  if (value.type == EXPRTK_VAL_UUID) return 1;
  if (value.type == EXPRTK_VAL_DATETIME || value.type == EXPRTK_VAL_DATE ||
      value.type == EXPRTK_VAL_TIME || value.type == EXPRTK_VAL_DURATION ||
      value.type == EXPRTK_VAL_DECIMAL)
    return 1;
  if (value.type == EXPRTK_VAL_VECTOR) return value.data.vector.size > 0;
  if (value.type == EXPRTK_VAL_LIST) return value.data.list.count > 0;
  if (exprtk_value_is_object_like(&value)) return exprtk_map_count(&value) > 0;
  if (value.type == EXPRTK_VAL_FUNCTION || value.type == EXPRTK_VAL_CLASS ||
      value.type == EXPRTK_VAL_INSTANCE || value.type == EXPRTK_VAL_BOUND_METHOD)
    return 1;
  return 0;
}

static int ts_mir_runtime_loop_tick(exprtk_env_t *env) {
  if (!env) return 1;
  env->curr_loop_iterations++;
  if (env->curr_loop_iterations > env->max_loop_iterations) {
    env->aborted = 1;
    return 0;
  }
  return 1;
}

static int ts_mir_runtime_body_flow_done(exprtk_env_t *env, int *should_break) {
  if (!should_break) return 1;
  *should_break = 0;
  if (!env) return 1;

  if (env->flow == exprtk_FLOW_BREAK) {
    env->flow = exprtk_FLOW_NORMAL;
    *should_break = 1;
    return 1;
  }
  if (env->flow == exprtk_FLOW_CONTINUE) {
    env->flow = exprtk_FLOW_NORMAL;
    return 1;
  }
  if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
    *should_break = 1;
  }
  return 1;
}

static int ts_mir_runtime_while_value(exprtk_node_t *node, exprtk_env_t *env,
                                      exprtk_value_t *out) {
  exprtk_value_t last = ts_mir_zero_value();

  if (!node || !env || !out || node->type != EXPRTK_NODE_WHILE) return 0;

  while (1) {
    exprtk_value_t cond;
    int done = 0;

    if (env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE) break;
    if (env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;

    if (!ts_mir_runtime_value_arg(node->data.while_loop.condition, env, &cond)) return 0;
    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) break;
    if (!ts_mir_value_truthy(cond)) break;
    if (!ts_mir_runtime_loop_tick(env)) break;

    if (!ts_mir_runtime_value_arg(node->data.while_loop.body, env, &last)) return 0;
    ts_mir_runtime_body_flow_done(env, &done);
    if (done) break;
  }

  *out = last;
  return 1;
}

static int ts_mir_runtime_for_value(exprtk_node_t *node, exprtk_env_t *env,
                                    exprtk_value_t *out) {
  exprtk_value_t last = ts_mir_zero_value();

  if (!node || !env || !out || node->type != EXPRTK_NODE_FOR) return 0;

  if (node->data.for_loop.init &&
      !ts_mir_runtime_value_arg(node->data.for_loop.init, env, &last)) {
    return 0;
  }
  if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
    *out = ts_mir_zero_value();
    return 1;
  }

  while (1) {
    int done = 0;

    if (node->data.for_loop.condition) {
      exprtk_value_t cond;
      if (!ts_mir_runtime_value_arg(node->data.for_loop.condition, env, &cond)) return 0;
      if (env->flow != exprtk_FLOW_NORMAL || env->aborted) break;
      if (!ts_mir_value_truthy(cond)) break;
    }

    if (!ts_mir_runtime_loop_tick(env)) break;

    if (!ts_mir_runtime_value_arg(node->data.for_loop.body, env, &last)) return 0;
    if (env->flow == exprtk_FLOW_BREAK) {
      env->flow = exprtk_FLOW_NORMAL;
      break;
    }
    if (env->flow == exprtk_FLOW_RETURN || env->flow == exprtk_FLOW_THROW || env->aborted) break;

    if (node->data.for_loop.post) {
      if (!ts_mir_runtime_value_arg(node->data.for_loop.post, env, &last)) return 0;
      if (env->flow != exprtk_FLOW_NORMAL && env->flow != exprtk_FLOW_CONTINUE) break;
    }
    if (env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;
    ts_mir_runtime_body_flow_done(env, &done);
    if (done) break;
  }

  *out = last;
  return 1;
}

static int ts_mir_runtime_do_while_value(exprtk_node_t *node, exprtk_env_t *env,
                                         exprtk_value_t *out) {
  exprtk_value_t last = ts_mir_zero_value();

  if (!node || !env || !out || node->type != EXPRTK_NODE_DO_WHILE) return 0;

  while (1) {
    exprtk_value_t cond;

    if (!ts_mir_runtime_value_arg(node->data.do_while.body, env, &last)) return 0;
    if (env->flow == exprtk_FLOW_BREAK) {
      env->flow = exprtk_FLOW_NORMAL;
      break;
    }
    if (env->flow == exprtk_FLOW_RETURN || env->flow == exprtk_FLOW_THROW || env->aborted) break;
    if (env->flow == exprtk_FLOW_CONTINUE) env->flow = exprtk_FLOW_NORMAL;

    if (!ts_mir_runtime_value_arg(node->data.do_while.condition, env, &cond)) return 0;
    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) break;
    if (!ts_mir_value_truthy(cond)) break;
    if (!ts_mir_runtime_loop_tick(env)) break;
  }

  *out = last;
  return 1;
}

static int ts_mir_runtime_for_in_value(exprtk_node_t *node, exprtk_env_t *env,
                                       exprtk_value_t *out) {
  exprtk_value_t collection;
  exprtk_value_t last = ts_mir_zero_value();

  if (!node || !env || !out || node->type != EXPRTK_NODE_FOR_IN ||
      !node->data.for_in.var_name) {
    return 0;
  }

  if (!ts_mir_runtime_value_arg(node->data.for_in.collection, env, &collection)) return 0;
  if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
    *out = ts_mir_zero_value();
    return 1;
  }

  if (collection.type == EXPRTK_VAL_VECTOR) {
    for (size_t i = 0; i < collection.data.vector.size; ++i) {
      int done = 0;
      if (!ts_mir_runtime_loop_tick(env)) break;
      exprtk_env_set(env, node->data.for_in.var_name,
                     exprtk_val_num(collection.data.vector.data[i]));
      if (!ts_mir_runtime_value_arg(node->data.for_in.body, env, &last)) return 0;
      ts_mir_runtime_body_flow_done(env, &done);
      if (done) break;
    }
  } else if (exprtk_value_is_object_like(&collection)) {
    exprtk_map_iter_t it = exprtk_map_iter_begin(&collection);
    const char *key = NULL;
    while (exprtk_map_iter_next(&it, &key, NULL)) {
      int done = 0;
      tstr_v sv;
      if (!ts_mir_runtime_loop_tick(env)) break;
      sv.data = (char *)key;
      sv.len = key ? strlen(key) : 0;
      exprtk_env_set(env, node->data.for_in.var_name, exprtk_val_str(sv));
      if (!ts_mir_runtime_value_arg(node->data.for_in.body, env, &last)) return 0;
      ts_mir_runtime_body_flow_done(env, &done);
      if (done) break;
    }
  } else if (collection.type == EXPRTK_VAL_LIST) {
    for (size_t i = 0; i < collection.data.list.count; ++i) {
      int done = 0;
      if (!ts_mir_runtime_loop_tick(env)) break;
      exprtk_env_set(env, node->data.for_in.var_name, collection.data.list.items[i]);
      if (!ts_mir_runtime_value_arg(node->data.for_in.body, env, &last)) return 0;
      ts_mir_runtime_body_flow_done(env, &done);
      if (done) break;
    }
  }

  *out = last;
  return 1;
}

static int ts_mir_runtime_switch_value(exprtk_node_t *node, exprtk_env_t *env,
                                       exprtk_value_t *out) {
  exprtk_value_t switch_val;

  if (!node || !env || !out || node->type != EXPRTK_NODE_SWITCH) return 0;
  if (!ts_mir_runtime_value_arg(node->data.switch_stmt.value, env, &switch_val)) return 0;
  if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
    *out = ts_mir_zero_value();
    return 1;
  }

  for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
    exprtk_value_t case_val;
    if (!ts_mir_runtime_value_arg(node->data.switch_stmt.cases[i * 2], env, &case_val)) {
      return 0;
    }
    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
      *out = ts_mir_zero_value();
      return 1;
    }
    if (values_match(switch_val, case_val)) {
      return ts_mir_runtime_value_arg(node->data.switch_stmt.cases[i * 2 + 1], env, out);
    }
  }

  if (node->data.switch_stmt.default_case) {
    return ts_mir_runtime_value_arg(node->data.switch_stmt.default_case, env, out);
  }

  *out = ts_mir_zero_value();
  return 1;
}

static int ts_mir_runtime_this_value(const exprtk_node_t *node, exprtk_env_t *env,
                                     exprtk_value_t *out) {
  if (!env || !out) return 0;
  if (env->current_method_is_static) {
    *out = throw_error(env, node, "this is only valid inside instance methods or constructors");
    return 1;
  }

  *out = exprtk_env_get(env, "this");
  if (out->type != EXPRTK_VAL_INSTANCE) {
    *out = throw_error(env, node, "this is only valid inside instance methods or constructors");
  }
  return 1;
}

static int ts_mir_runtime_super_value(exprtk_node_t *node, exprtk_env_t *env,
                                      exprtk_value_t *out) {
  exprtk_value_t zero = ts_mir_zero_value();
  exprtk_class_t *owner_class = NULL;
  exprtk_class_t *parent = NULL;

  if (!node || !env || !out || node->type != EXPRTK_NODE_SUPER) return 0;

  owner_class = eval_current_class(env);
  if (env->current_method_is_static) {
    if (!owner_class) {
      *out = throw_error(env, node, "super is only valid inside class methods");
      return 1;
    }
    parent = owner_class->prototype;
    if (!parent) {
      *out = throw_error(env, node, "Class '%s' has no parent class",
                         owner_class->name ? owner_class->name : "<unknown>");
      return 1;
    }
    if (node->data.super_expr.member == NULL) {
      *out = throw_error(env, node, "super() is only valid inside instance constructors");
      return 1;
    }

    if (node->data.super_expr.is_call) {
      size_t argc = 0;
      exprtk_value_t *args =
          ts_mir_runtime_call_value_args(node, env, &argc);
      if (!args && node->data.super_expr.arg_count > 0) return 0;

      exprtk_func_t *method =
          exprtk_class_lookup_method_typed(parent, node->data.super_expr.member, 1, argc, args);
      if (!method) {
        ts_mir_runtime_value_args_free(args, argc);
        *out = throw_error(env, node, "Parent class has no static method '%s'",
                           node->data.super_expr.member);
        return 1;
      }
      if (!can_access_method(env, method, node)) {
        ts_mir_runtime_value_args_free(args, argc);
        *out = throw_method_access_error(env, node, node->data.super_expr.member, method);
        return 1;
      }

      *out = eval_script_function(method, argc, args, env, env);
      ts_mir_runtime_value_args_free(args, argc);
      return 1;
    }

    exprtk_value_t static_field;
    if (exprtk_class_get_static_field(parent, node->data.super_expr.member, &static_field)) {
      exprtk_class_t *field_owner = NULL;
      int access =
          exprtk_class_get_static_field_access(parent, node->data.super_expr.member, &field_owner);
      if (!can_access_declared_field(env, field_owner, access, node,
                                     node->data.super_expr.member)) {
        *out = throw_field_access_error(env, node, node->data.super_expr.member, access);
        return 1;
      }
      *out = static_field;
      return 1;
    }

    exprtk_func_t *method = exprtk_class_lookup_method(parent, node->data.super_expr.member, 1);
    if (!method) {
      *out = throw_error(env, node, "Parent class has no static field or method '%s'",
                         node->data.super_expr.member);
      return 1;
    }
    if (!can_access_method(env, method, node)) {
      *out = throw_method_access_error(env, node, node->data.super_expr.member, method);
      return 1;
    }

    out->type = EXPRTK_VAL_FUNCTION;
    out->data.function.arg_params = method->data.script.arg_params;
    out->data.function.arg_count = method->data.script.arg_count;
    out->data.function.body = method->data.script.body;
    out->data.function.closure_env = method->closure_env;
    out->data.function.owner_class = method->owner_class;
    out->data.function.is_static_method = method->is_static_method;
    out->data.function.access_level = method->access_level;
    return 1;
  }

  exprtk_value_t this_val = exprtk_env_get(env, "this");
  if (this_val.type != EXPRTK_VAL_INSTANCE) {
    *out = throw_error(env, node, "super is only valid inside instance methods or constructors");
    return 1;
  }

  exprtk_instance_t *instance = this_val.data.instance_val.instance;
  parent = owner_class ? owner_class->prototype : instance->klass->prototype;
  if (!parent) {
    const char *owner_name = owner_class ? owner_class->name : instance->klass->name;
    *out = throw_error(env, node, "Class '%s' has no parent class",
                       owner_name ? owner_name : "<unknown>");
    return 1;
  }

  if (node->data.super_expr.member == NULL) {
    if (!env->current_method_is_constructor) {
      *out = throw_error(env, node, "super() is only valid inside instance constructors");
      return 1;
    }

    size_t argc = 0;
    exprtk_value_t *args = ts_mir_runtime_call_value_args(node, env, &argc);
    if (!args && node->data.super_expr.arg_count > 0) return 0;

    exprtk_func_t *constructor = eval_find_constructor_typed(parent, argc, args);
    if (!constructor) {
      ts_mir_runtime_value_args_free(args, argc);
      *out = zero;
      return 1;
    }

    *out = eval_script_function(constructor, argc, args, env, env);
    ts_mir_runtime_value_args_free(args, argc);
    return 1;
  }

  if (node->data.super_expr.is_call) {
    size_t argc = 0;
    exprtk_value_t *args = ts_mir_runtime_call_value_args(node, env, &argc);
    if (!args && node->data.super_expr.arg_count > 0) return 0;

    exprtk_func_t *method =
        exprtk_class_lookup_method_typed(parent, node->data.super_expr.member, 0, argc, args);
    if (!method) {
      ts_mir_runtime_value_args_free(args, argc);
      *out = throw_error(env, node, "Parent class has no method '%s'",
                         node->data.super_expr.member);
      return 1;
    }
    if (!can_access_method(env, method, node)) {
      ts_mir_runtime_value_args_free(args, argc);
      *out = throw_method_access_error(env, node, node->data.super_expr.member, method);
      return 1;
    }

    *out = eval_script_function(method, argc, args, env, env);
    ts_mir_runtime_value_args_free(args, argc);
    return 1;
  }

  exprtk_func_t *method = exprtk_class_lookup_method(parent, node->data.super_expr.member, 0);
  if (method) {
    if (!can_access_method(env, method, node)) {
      *out = throw_method_access_error(env, node, node->data.super_expr.member, method);
      return 1;
    }
    *out = exprtk_val_bound_method(instance, method);
    return 1;
  }

  exprtk_value_t field_value;
  if (exprtk_instance_get_field(instance, node->data.super_expr.member, &field_value)) {
    exprtk_class_t *field_owner = NULL;
    int access =
        exprtk_class_get_instance_field_access(parent, node->data.super_expr.member, &field_owner);
    if (!can_access_declared_field(env, field_owner, access, node,
                                   node->data.super_expr.member)) {
      *out = throw_field_access_error(env, node, node->data.super_expr.member, access);
      return 1;
    }
    *out = field_value;
    return 1;
  }

  *out = throw_error(env, node, "Parent class has no method or instance field '%s'",
                     node->data.super_expr.member);
  return 1;
}

static int ts_mir_runtime_member_set_value(exprtk_node_t *node, exprtk_env_t *env,
                                           exprtk_value_t *out) {
  exprtk_value_t zero = ts_mir_zero_value();
  exprtk_node_t *object_node = NULL;
  exprtk_value_t val;

  if (!node || !env || !out || node->type != EXPRTK_NODE_MEMBER_SET) return 0;
  object_node = node->data.member_set.object;
  if (!object_node ||
      (object_node->type != EXPRTK_NODE_VARIABLE &&
       object_node->type != EXPRTK_NODE_THIS &&
       object_node->type != EXPRTK_NODE_SUPER)) {
    *out = throw_error(env, node,
                       "Member assignment '%s' requires a variable receiver, this, or static super",
                       node->data.member_set.member ? node->data.member_set.member : "<null>");
    return 1;
  }

  if (!ts_mir_runtime_value_arg(node->data.member_set.value, env, &val)) return 0;
  if (env->flow != exprtk_FLOW_NORMAL) {
    *out = zero;
    return 1;
  }

  if (object_node->type == EXPRTK_NODE_VARIABLE) {
    const char *var_name = object_node->data.variable.name;
    exprtk_value_t object = exprtk_env_get(env, var_name);
    if (exprtk_value_is_object_like(&object)) {
      exprtk_map_set(&object, node->data.member_set.member, val);
      exprtk_env_set(env, var_name, object);
      *out = val;
      return 1;
    }
    if (object.type == EXPRTK_VAL_INSTANCE) {
      exprtk_class_t *field_owner = NULL;
      int access = exprtk_class_get_instance_field_access(
          object.data.instance_val.instance->klass, node->data.member_set.member, &field_owner);
      if (!can_access_declared_field(env, field_owner, access, object_node,
                                     node->data.member_set.member)) {
        *out = throw_field_access_error(env, node, node->data.member_set.member, access);
        return 1;
      }
      {
        char field_error[256];
        if (!exprtk_instance_set_field_checked(
                object.data.instance_val.instance, node->data.member_set.member,
                val, field_error, sizeof(field_error))) {
          *out = throw_error(env, node, "%s", field_error);
          return 1;
        }
      }
      *out = val;
      return 1;
    }
    if (object.type == EXPRTK_VAL_CLASS) {
      exprtk_class_t *field_owner = NULL;
      int access = exprtk_class_get_static_field_access(object.data.class_val.klass,
                                                        node->data.member_set.member,
                                                        &field_owner);
      if (!can_access_declared_field(env, field_owner, access, object_node,
                                     node->data.member_set.member)) {
        *out = throw_field_access_error(env, node, node->data.member_set.member, access);
        return 1;
      }
      exprtk_class_set_static_field(object.data.class_val.klass, node->data.member_set.member,
                                    val);
      *out = val;
      return 1;
    }
  }

  if (object_node->type == EXPRTK_NODE_THIS) {
    exprtk_value_t this_val;
    if (!ts_mir_runtime_this_value(object_node, env, &this_val)) return 0;
    if (env->flow != exprtk_FLOW_NORMAL || this_val.type != EXPRTK_VAL_INSTANCE) {
      *out = zero;
      return 1;
    }

    exprtk_class_t *field_owner = NULL;
    int access = exprtk_class_get_instance_field_access(this_val.data.instance_val.instance->klass,
                                                        node->data.member_set.member,
                                                        &field_owner);
    if (!can_access_declared_field(env, field_owner, access, object_node,
                                   node->data.member_set.member)) {
      *out = throw_field_access_error(env, node, node->data.member_set.member, access);
      return 1;
    }
    {
      char field_error[256];
      if (!exprtk_instance_set_field_checked(
              this_val.data.instance_val.instance, node->data.member_set.member,
              val, field_error, sizeof(field_error))) {
        *out = throw_error(env, node, "%s", field_error);
        return 1;
      }
    }
    *out = val;
    return 1;
  }

  if (object_node->type == EXPRTK_NODE_SUPER) {
    exprtk_class_t *owner_class = eval_current_class(env);
    exprtk_class_t *parent = owner_class ? owner_class->prototype : NULL;
    if (!parent) {
      *out = throw_error(env, object_node, "super is only valid inside class methods");
      return 1;
    }

    if (env->current_method_is_static) {
      exprtk_class_t *field_owner = NULL;
      int access =
          exprtk_class_get_static_field_access(parent, node->data.member_set.member, &field_owner);
      if (!can_access_declared_field(env, field_owner, access, object_node,
                                     node->data.member_set.member)) {
        *out = throw_field_access_error(env, node, node->data.member_set.member, access);
        return 1;
      }
      exprtk_class_set_static_field(parent, node->data.member_set.member, val);
      *out = val;
      return 1;
    }

    exprtk_value_t this_val = exprtk_env_get(env, "this");
    if (this_val.type != EXPRTK_VAL_INSTANCE) {
      *out = throw_error(env, node, "super is only valid inside instance methods or constructors");
      return 1;
    }
    exprtk_class_t *field_owner = NULL;
    int access =
        exprtk_class_get_instance_field_access(parent, node->data.member_set.member, &field_owner);
    if (!can_access_declared_field(env, field_owner, access, object_node,
                                   node->data.member_set.member)) {
      *out = throw_field_access_error(env, node, node->data.member_set.member, access);
      return 1;
    }
    {
      char field_error[256];
      if (!exprtk_instance_set_field_checked(
              this_val.data.instance_val.instance, node->data.member_set.member,
              val, field_error, sizeof(field_error))) {
        *out = throw_error(env, node, "%s", field_error);
        return 1;
      }
    }
    *out = val;
    return 1;
  }

  *out = throw_error(env, node, "Member assignment '%s' requires a map, instance, or class",
                     node->data.member_set.member ? node->data.member_set.member : "<null>");
  return 1;
}

static int ts_mir_runtime_value_arg(exprtk_node_t *node, exprtk_env_t *env,
                                 exprtk_value_t *out) {
  if (!node || !env || !out) return 0;

  switch (node->type) {
  case EXPRTK_NODE_NUMBER:
    *out = exprtk_val_num(node->data.number);
    return 1;

  case EXPRTK_NODE_INTEGER:
    *out = exprtk_val_int(node->data.integer);
    return 1;

  case EXPRTK_NODE_STRING:
    *out = exprtk_val_str(node->data.string.value);
    return 1;

  case EXPRTK_NODE_NULL:
    memset(out, 0, sizeof(*out));
    out->type = EXPRTK_VAL_NULL;
    return 1;

  case EXPRTK_NODE_THIS:
    return ts_mir_runtime_this_value(node, env, out);

  case EXPRTK_NODE_SUPER:
    return ts_mir_runtime_super_value(node, env, out);

  case EXPRTK_NODE_CLASS_DEF:
    *out = eval_class_def_node(node, env);
    return 1;

  case EXPRTK_NODE_NEW:
    *out = eval_class_instantiation(node->data.new_expr.class_name, node->data.new_expr.args,
                                    node->data.new_expr.arg_count, env);
    return 1;

  case EXPRTK_NODE_TEMPLATE_STRING:
    return ts_mir_runtime_template_value(node, env, out);

  case EXPRTK_NODE_FUNCTION_EXPRESSION:
    out->type = EXPRTK_VAL_FUNCTION;
    out->data.function.arg_params = node->data.func_def.arg_params;
    out->data.function.arg_count = node->data.func_def.arg_count;
    out->data.function.body = node->data.func_def.body;
    out->data.function.closure_env = exprtk_env_snapshot(env);
    out->data.function.owner_class = NULL;
    out->data.function.is_static_method = 0;
    out->data.function.access_level = EXPRTK_ACCESS_PUBLIC;
    return 1;

  case EXPRTK_NODE_FUNCTION_DEFINITION:
    if (!ts_mir_runtime_define_func_in_env(env, node)) return 0;
    *out = ts_mir_zero_value();
    return 1;

  case EXPRTK_NODE_VARIABLE:
    if (!node->data.variable.name) return 0;
    *out = exprtk_env_get(env, node->data.variable.name);
    return 1;

  case EXPRTK_NODE_ASSIGNMENT:
    if (!node->data.assignment.name ||
        !ts_mir_runtime_value_arg(node->data.assignment.value, env, out)) {
      return 0;
    }
    {
      int destroy_container = out->ownership == EXPRTK_VALUE_OWNED &&
          (out->type == EXPRTK_VAL_MAP || out->type == EXPRTK_VAL_OBJECT ||
           out->type == EXPRTK_VAL_LIST || out->type == EXPRTK_VAL_SET);
      exprtk_env_set(env, node->data.assignment.name, *out);
      if (destroy_container) exprtk_value_destroy(out);
    }
    *out = exprtk_env_get(env, node->data.assignment.name);
    return 1;

  case EXPRTK_NODE_CONSTANT_DECL:
    if (!node->data.assignment.name ||
        !ts_mir_runtime_value_arg(node->data.assignment.value, env, out)) {
      return 0;
    }
    {
      int destroy_container = out->ownership == EXPRTK_VALUE_OWNED &&
          (out->type == EXPRTK_VAL_MAP || out->type == EXPRTK_VAL_OBJECT ||
           out->type == EXPRTK_VAL_LIST || out->type == EXPRTK_VAL_SET);
      exprtk_env_set_constant(env, node->data.assignment.name, *out);
      if (destroy_container) exprtk_value_destroy(out);
    }
    *out = exprtk_env_get(env, node->data.assignment.name);
    return 1;

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    if (!ts_mir_runtime_value_arg(node->data.destructuring.value, env, out)) return 0;
    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) return 1;
    eval_destructure(node->data.destructuring.targets, *out, env,
                     node->data.destructuring.is_constant);
    return 1;

  case EXPRTK_NODE_BLOCK: {
    exprtk_value_t last = exprtk_val_num(0.0);
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (!ts_mir_runtime_value_arg(node->data.block.statements[i], env, &last)) return 0;
      if (env->flow != exprtk_FLOW_NORMAL || env->aborted) break;
    }
    *out = last;
    return 1;
  }

  case EXPRTK_NODE_FLOW:
    if (node->data.flow.type == exprtk_TOKEN_RETURN) {
      if (node->data.flow.value) {
        if (!ts_mir_runtime_value_arg(node->data.flow.value, env, out)) return 0;
        if (env->flow != exprtk_FLOW_NORMAL) return 1;
      } else {
        *out = exprtk_val_num(0.0);
      }
      env->return_value = *out;
      env->flow = exprtk_FLOW_RETURN;
      return 1;
    }
    *out = exprtk_val_num(0.0);
    if (node->data.flow.type == exprtk_TOKEN_BREAK) {
      env->flow = exprtk_FLOW_BREAK;
    } else if (node->data.flow.type == exprtk_TOKEN_CONTINUE) {
      env->flow = exprtk_FLOW_CONTINUE;
    }
    return 1;

  case EXPRTK_NODE_THROW:
    if (node->data.throw_stmt.value) {
      if (!ts_mir_runtime_value_arg(node->data.throw_stmt.value, env, out)) return 0;
    } else {
      *out = exprtk_val_num(0.0);
    }
    env->flow = exprtk_FLOW_THROW;
    env->error_value = *out;
    return 1;

  case EXPRTK_NODE_TRY_CATCH:
    return ts_mir_runtime_try_catch_value(node, env, out);

  case EXPRTK_NODE_IF: {
    exprtk_value_t cond;
    if (!ts_mir_runtime_value_arg(node->data.if_stmt.condition, env, &cond)) return 0;
    if (ts_mir_value_truthy(cond)) {
      return node->data.if_stmt.if_branch
                 ? ts_mir_runtime_value_arg(node->data.if_stmt.if_branch, env, out)
                 : (*out = exprtk_val_num(0.0), 1);
    }
    return node->data.if_stmt.else_branch
               ? ts_mir_runtime_value_arg(node->data.if_stmt.else_branch, env, out)
               : (*out = exprtk_val_num(0.0), 1);
  }

  case EXPRTK_NODE_WHILE:
    return ts_mir_runtime_while_value(node, env, out);

  case EXPRTK_NODE_FOR:
    return ts_mir_runtime_for_value(node, env, out);

  case EXPRTK_NODE_DO_WHILE:
    return ts_mir_runtime_do_while_value(node, env, out);

  case EXPRTK_NODE_FOR_IN:
    return ts_mir_runtime_for_in_value(node, env, out);

  case EXPRTK_NODE_SWITCH:
    return ts_mir_runtime_switch_value(node, env, out);

  case EXPRTK_NODE_BINARY_OP:
    return ts_mir_eval_value_binary(node, env, out);

  case EXPRTK_NODE_INSTANCEOF: {
    exprtk_value_t object;
    exprtk_value_t class_value;
    if (!ts_mir_runtime_value_arg(node->data.instanceof_expr.object, env, &object)) return 0;
    if (env->flow != exprtk_FLOW_NORMAL) {
      *out = exprtk_val_num(0.0);
      return 1;
    }
    if (object.type != EXPRTK_VAL_INSTANCE) {
      *out = exprtk_val_num(0.0);
      return 1;
    }
    if (node->data.instanceof_expr.class_expr) {
      if (!ts_mir_runtime_value_arg(node->data.instanceof_expr.class_expr, env, &class_value))
        return 0;
    } else {
      class_value = exprtk_env_get(env, node->data.instanceof_expr.class_name);
    }
    *out = exprtk_val_num(
        class_value.type == EXPRTK_VAL_CLASS
            ? (double)exprtk_instance_of(object.data.instance_val.instance,
                                         class_value.data.class_val.klass)
            : 0.0);
    return 1;
  }

  case EXPRTK_NODE_FUNCTION_CALL: {
    exprtk_value_t *args = NULL;
    size_t argc = 0;
    if (!node->data.function.name) return 0;
    args = ts_mir_runtime_call_value_args(node, env, &argc);
    if (!args && node->data.function.arg_count > 0) return 0;
    *out = exprtk_call_internal(node->data.function.name, argc, args, env);
    ts_mir_runtime_value_args_free(args, argc);
    return !env->aborted;
  }

  case EXPRTK_NODE_MEMBER_CALL: {
    exprtk_value_t object;
    exprtk_value_t *args = NULL;
    size_t argc = 0;

    if (!node->data.member_call.object || !node->data.member_call.method) return 0;

    if (node->data.member_call.object->type != EXPRTK_NODE_VARIABLE) {
      *out = exprtk_eval(node, env);
      return !env->aborted && env->flow != exprtk_FLOW_THROW;
    }

    if (node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_call.object->data.variable.name) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               node->data.member_call.object->data.variable.name,
               node->data.member_call.method);
      if (ts_env_has_func(env, full_name) || exprtk_find_builtin(full_name, env) ||
          ts_resolves_core_compat_func(full_name)) {
        args = ts_mir_runtime_call_value_args(node, env, &argc);
        if (!args && node->data.member_call.arg_count > 0) return 0;
        *out = exprtk_call_internal(full_name, argc, args, env);
        ts_mir_runtime_value_args_free(args, argc);
        return !env->aborted;
      }

      *out = exprtk_member_call_checked_value_nodes(
          node->data.member_call.object->data.variable.name, node->data.member_call.method,
          node->data.member_call.object, node, env);
      if (!env->aborted) return 1;
    }

    if (!ts_mir_runtime_value_arg(node->data.member_call.object, env, &object)) {
      *out = exprtk_eval(node, env);
      return !env->aborted && env->flow != exprtk_FLOW_THROW;
    }
    if (strcmp(node->data.member_call.method, "length") == 0 &&
        node->data.member_call.arg_count == 0) {
      if (object.type == EXPRTK_VAL_LIST || object.type == EXPRTK_VAL_SET) {
        *out = exprtk_val_num((double)object.data.list.count);
        return 1;
      }
      if (object.type == EXPRTK_VAL_VECTOR) {
        *out = exprtk_val_num((double)object.data.vector.size);
        return 1;
      }
      if (object.type == EXPRTK_VAL_STRING) {
        *out = exprtk_val_num((double)object.data.string.len);
        return 1;
      }
      if (object.type == EXPRTK_VAL_BYTES) {
        *out = exprtk_val_num((double)object.data.bytes.len);
        return 1;
      }
    }
    if (object.type == EXPRTK_VAL_INSTANCE || object.type == EXPRTK_VAL_CLASS ||
        exprtk_value_is_object_like(&object) || object.type == EXPRTK_VAL_LIST ||
        object.type == EXPRTK_VAL_SET ||
        object.type == EXPRTK_VAL_VECTOR || object.type == EXPRTK_VAL_STRING ||
        object.type == EXPRTK_VAL_BYTES) {
      const char *temp_name = "__ts_mir_value_receiver";
      exprtk_env_set(env, temp_name, object);
      *out = exprtk_member_call_checked_value_nodes(temp_name, node->data.member_call.method,
                                                    node->data.member_call.object, node, env);
      return !env->aborted;
    }
    return 0;
  }

  case EXPRTK_NODE_VECTOR: {
    size_t actual = 0;
    size_t cap = 0;
    exprtk_value_t *vals = NULL;
    turbo_vec_t vector_data = {0};

    for (size_t i = 0; i < node->data.vector.count; ++i) {
      exprtk_value_t value;
      exprtk_node_t *element = node->data.vector.elements[i];

      if (element && element->type == EXPRTK_NODE_SPREAD) {
        if (!ts_mir_runtime_value_arg(element->data.spread.child, env, &value)) {
          exprtk_values_destroy(vals, actual);
          free(vals);
          return 0;
        }
        if (value.type == EXPRTK_VAL_VECTOR) {
          if (!ts_mir_value_array_grow(&vals, &cap, actual + value.data.vector.size)) {
            exprtk_value_destroy(&value);
            exprtk_values_destroy(vals, actual);
            free(vals);
            return 0;
          }
          for (size_t j = 0; j < value.data.vector.size; ++j) {
            vals[actual++] = exprtk_val_num(value.data.vector.data[j]);
          }
        } else if (value.type == EXPRTK_VAL_LIST) {
          if (!ts_mir_value_array_grow(&vals, &cap, actual + value.data.list.count)) {
            exprtk_value_destroy(&value);
            exprtk_values_destroy(vals, actual);
            free(vals);
            return 0;
          }
          for (size_t j = 0; j < value.data.list.count; ++j) {
            exprtk_value_t copied;
            if (exprtk_value_copy_to_env(value.data.list.items[j], env, &copied) != 0) {
              exprtk_value_destroy(&value);
              exprtk_values_destroy(vals, actual);
              free(vals);
              return 0;
            }
            vals[actual++] = copied;
          }
        } else {
          if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
            exprtk_value_destroy(&value);
            exprtk_values_destroy(vals, actual);
            free(vals);
            return 0;
          }
          vals[actual++] = value;
          memset(&value, 0, sizeof(value));
        }
        exprtk_value_destroy(&value);
        continue;
      }

      if (!ts_mir_runtime_value_arg(element, env, &value)) {
        exprtk_values_destroy(vals, actual);
        free(vals);
        return 0;
      }
      if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
        exprtk_value_destroy(&value);
        exprtk_values_destroy(vals, actual);
        free(vals);
        return 0;
      }
      vals[actual++] = value;
    }

    if (turbo_vec_init(&vector_data, sizeof(double)) != TURBO_OK ||
        turbo_vec_reserve(&vector_data, actual) != TURBO_OK) {
      if (vector_data.data) turbo_vec_destroy(&vector_data);
      exprtk_values_destroy(vals, actual);
      free(vals);
      return 0;
    }
    for (size_t i = 0; i < actual; ++i) {
      double element_value = ts_mir_numeric_value(vals[i]);
      if (turbo_vec_push(&vector_data, &element_value) != TURBO_OK) {
        turbo_vec_destroy(&vector_data);
        exprtk_values_destroy(vals, actual);
        free(vals);
        return 0;
      }
    }
    if (exprtk_value_copy_to_env(
            exprtk_val_vec((double *)vector_data.data, vector_data.size), env, out) != 0) {
      turbo_vec_destroy(&vector_data);
      exprtk_values_destroy(vals, actual);
      free(vals);
      return 0;
    }
    turbo_vec_destroy(&vector_data);
    exprtk_values_destroy(vals, actual);
    free(vals);
    return 1;
  }

  case EXPRTK_NODE_MAP_LITERAL: {
    exprtk_value_t map = exprtk_val_map();
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      exprtk_value_t value;
      if (!node->data.map_literal.keys[i] ||
          !ts_mir_runtime_value_arg(node->data.map_literal.values[i], env, &value)) {
        exprtk_map_free(&map);
        return 0;
      }
      if (exprtk_map_set(&map, node->data.map_literal.keys[i], value) != 0) {
        exprtk_value_destroy(&value);
        exprtk_value_destroy(&map);
        return 0;
      }
      exprtk_value_destroy(&value);
    }
    *out = map;
    return 1;
  }

  case EXPRTK_NODE_INDEX: {
    exprtk_value_t array;
    exprtk_value_t index;
    int64_t idx = 0;

    if (!ts_mir_runtime_value_arg(node->data.index_access.array, env, &array) ||
        !ts_mir_runtime_value_arg(node->data.index_access.index, env, &index)) {
      *out = exprtk_eval(node, env);
      return !env->aborted && env->flow != exprtk_FLOW_THROW;
    }

    if (index.type == EXPRTK_VAL_STRING && exprtk_value_is_object_like(&array)) {
      *out = exprtk_map_get(&array, index.data.string.data);
      return 1;
    }

    idx = (int64_t)ts_mir_numeric_value(index);
    if (idx < 0) {
      *out = exprtk_val_num(0.0);
      return 1;
    }
    if (array.type == EXPRTK_VAL_VECTOR) {
      *out = (size_t)idx < array.data.vector.size
                 ? exprtk_val_num(array.data.vector.data[idx])
                 : exprtk_val_num(0.0);
      return 1;
    }
    if (array.type == EXPRTK_VAL_LIST) {
      *out = exprtk_list_get(&array, (size_t)idx);
      return 1;
    }
    if (array.type == EXPRTK_VAL_SET) {
      *out = exprtk_list_get(&array, (size_t)idx);
      return 1;
    }
    if (array.type == EXPRTK_VAL_BYTES) {
      *out = (size_t)idx < array.data.bytes.len
                 ? exprtk_val_int((unsigned char)array.data.bytes.data[idx])
                 : exprtk_val_num(0.0);
      return 1;
    }
    *out = exprtk_eval(node, env);
    return !env->aborted;
  }

  case EXPRTK_NODE_SLICE: {
    exprtk_value_t array;
    exprtk_value_t start_value = exprtk_val_num(0.0);
    exprtk_value_t end_value = exprtk_val_num(0.0);
    int64_t start = 0;
    int64_t end = 0;

    if (!ts_mir_runtime_value_arg(node->data.slice.array, env, &array)) return 0;
    if (node->data.slice.start &&
        !ts_mir_runtime_value_arg(node->data.slice.start, env, &start_value)) {
      return 0;
    }
    if (node->data.slice.end &&
        !ts_mir_runtime_value_arg(node->data.slice.end, env, &end_value)) {
      return 0;
    }

    if (node->data.slice.start) start = (int64_t)ts_mir_numeric_value(start_value);
    if (node->data.slice.end) {
      end = (int64_t)ts_mir_numeric_value(end_value);
    } else if (array.type == EXPRTK_VAL_VECTOR) {
      end = (int64_t)array.data.vector.size;
    } else if (array.type == EXPRTK_VAL_LIST) {
      end = (int64_t)array.data.list.count;
    } else {
      return 0;
    }

    if (start < 0 || end < start) {
      exprtk_value_destroy(&array);
      exprtk_value_destroy(&start_value);
      exprtk_value_destroy(&end_value);
      return 0;
    }
    if (array.type == EXPRTK_VAL_VECTOR) {
      if ((size_t)end > array.data.vector.size) {
        exprtk_value_destroy(&array);
        exprtk_value_destroy(&start_value);
        exprtk_value_destroy(&end_value);
        return 0;
      }
      size_t count = (size_t)(end - start);
      int copied = exprtk_value_copy_to_env(
          exprtk_val_vec(count ? array.data.vector.data + start : NULL, count), env, out) == 0;
      exprtk_value_destroy(&array);
      exprtk_value_destroy(&start_value);
      exprtk_value_destroy(&end_value);
      return copied;
    }
    if (array.type == EXPRTK_VAL_LIST) {
      if ((size_t)end > array.data.list.count) {
        exprtk_value_destroy(&array);
        exprtk_value_destroy(&start_value);
        exprtk_value_destroy(&end_value);
        return 0;
      }
      exprtk_value_t list = exprtk_val_list_empty();
      for (size_t i = (size_t)start; i < (size_t)end; ++i) {
        if (exprtk_list_push(&list, exprtk_value_borrow(array.data.list.items[i])) != 0) {
          exprtk_value_destroy(&list);
          exprtk_value_destroy(&array);
          exprtk_value_destroy(&start_value);
          exprtk_value_destroy(&end_value);
          return 0;
        }
      }
      exprtk_value_destroy(&array);
      exprtk_value_destroy(&start_value);
      exprtk_value_destroy(&end_value);
      *out = list;
      return 1;
    }
    exprtk_value_destroy(&array);
    exprtk_value_destroy(&start_value);
    exprtk_value_destroy(&end_value);
    return 0;
  }

  case EXPRTK_NODE_MEMBER_ACCESS: {
    exprtk_value_t object;
    const char *member = node->data.member_access.member;
    if (!member || !ts_mir_runtime_value_arg(node->data.member_access.object, env, &object)) {
      return 0;
    }
    if (strcmp(member, "length") == 0) {
      if (object.type == EXPRTK_VAL_LIST || object.type == EXPRTK_VAL_SET) {
        *out = exprtk_val_num((double)object.data.list.count);
        return 1;
      }
      if (object.type == EXPRTK_VAL_VECTOR) {
        *out = exprtk_val_num((double)object.data.vector.size);
        return 1;
      }
      if (object.type == EXPRTK_VAL_STRING) {
        *out = exprtk_val_num((double)object.data.string.len);
        return 1;
      }
      if (object.type == EXPRTK_VAL_BYTES) {
        *out = exprtk_val_num((double)object.data.bytes.len);
        return 1;
      }
    }
    if (exprtk_value_is_object_like(&object)) {
      *out = exprtk_map_get(&object, member);
      return 1;
    }
    if (object.type == EXPRTK_VAL_DATETIME) {
      return exprtk_datetime_member_get(object, member, out);
    }
    if (object.type == EXPRTK_VAL_DATE) {
      return exprtk_date_member_get(object, member, out);
    }
    if (object.type == EXPRTK_VAL_TIME) {
      return exprtk_time_member_get(object, member, out);
    }
    if (object.type == EXPRTK_VAL_DURATION) {
      return exprtk_duration_member_get(object, member, out);
    }
    if (object.type == EXPRTK_VAL_DECIMAL) {
      return exprtk_decimal_member_get(object, member, out);
    }
    if (object.type == EXPRTK_VAL_INSTANCE || object.type == EXPRTK_VAL_CLASS) {
      const char *object_name = NULL;
      if (node->data.member_access.object &&
          node->data.member_access.object->type == EXPRTK_NODE_VARIABLE) {
        object_name = node->data.member_access.object->data.variable.name;
      } else {
        object_name = "__ts_mir_value_receiver";
        exprtk_env_set(env, object_name, object);
      }
      *out = exprtk_oop_get_member_checked(object_name, member,
                                           node->data.member_access.object, env);
      return !env->aborted;
    }
    *out = throw_error(env, node,
                       "Member access '%s' is invalid for %s (receiver node type %d)", member,
                       type_name(object.type),
                       node->data.member_access.object
                           ? (int)node->data.member_access.object->type
                           : -1);
    return 1;
  }

  case EXPRTK_NODE_MEMBER_SET:
    return ts_mir_runtime_member_set_value(node, env, out);

  default:
    return 0;
  }
}

static exprtk_value_t *ts_mir_runtime_call_value_args(exprtk_node_t *call_node, exprtk_env_t *env,
                                                   size_t *out_count) {
  size_t cap = 0;
  size_t actual = 0;
  exprtk_value_t *vals = NULL;
  size_t arg_count = 0;
  exprtk_node_t **arg_nodes = NULL;

  if (!out_count) return NULL;
  *out_count = 0;
  if (!call_node || !env) return NULL;

  if (call_node->type == EXPRTK_NODE_FUNCTION_CALL) {
    arg_count = call_node->data.function.arg_count;
    arg_nodes = call_node->data.function.args;
  } else if (call_node->type == EXPRTK_NODE_MEMBER_CALL) {
    arg_count = call_node->data.member_call.arg_count;
    arg_nodes = call_node->data.member_call.args;
  } else if (call_node->type == EXPRTK_NODE_SUPER) {
    arg_count = call_node->data.super_expr.arg_count;
    arg_nodes = call_node->data.super_expr.args;
  } else {
    return NULL;
  }

  for (size_t i = 0; i < arg_count; ++i) {
    exprtk_node_t *arg = arg_nodes[i];
    if (!arg) continue;

    if (arg->type == EXPRTK_NODE_SPREAD) {
      exprtk_value_t spread;
      if (!ts_mir_runtime_value_arg(arg->data.spread.child, env, &spread)) {
        if (!env->aborted && env->error_msg[0] == '\0') ts_mir_value_arg_error(env, arg);
        ts_mir_runtime_value_args_free(vals, actual);
        return NULL;
      }
      if (spread.type == EXPRTK_VAL_VECTOR) {
        if (!ts_mir_value_array_grow(&vals, &cap, actual + spread.data.vector.size)) {
          exprtk_value_destroy(&spread);
          ts_mir_runtime_value_args_free(vals, actual);
          return NULL;
        }
        for (size_t j = 0; j < spread.data.vector.size; ++j) {
          vals[actual++] = exprtk_val_num(spread.data.vector.data[j]);
        }
      } else if (spread.type == EXPRTK_VAL_LIST) {
        if (!ts_mir_value_array_grow(&vals, &cap, actual + spread.data.list.count)) {
          exprtk_value_destroy(&spread);
          ts_mir_runtime_value_args_free(vals, actual);
          return NULL;
        }
        for (size_t j = 0; j < spread.data.list.count; ++j) {
          exprtk_value_t copied;
          if (exprtk_value_copy_to_env(spread.data.list.items[j], env, &copied) != 0) {
            exprtk_value_destroy(&spread);
            ts_mir_runtime_value_args_free(vals, actual);
            return NULL;
          }
          vals[actual++] = copied;
        }
      } else {
        if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
          exprtk_value_destroy(&spread);
          ts_mir_runtime_value_args_free(vals, actual);
          return NULL;
        }
        vals[actual++] = spread;
        memset(&spread, 0, sizeof(spread));
      }
      exprtk_value_destroy(&spread);
      continue;
    }

    if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
      ts_mir_runtime_value_args_free(vals, actual);
      return NULL;
    }
    if (!ts_mir_runtime_value_arg(arg, env, &vals[actual])) {
      if (!env->aborted && env->error_msg[0] == '\0') ts_mir_value_arg_error(env, arg);
      ts_mir_runtime_value_args_free(vals, actual);
      return NULL;
    }
    actual++;
  }

  *out_count = actual;
  return vals;
}

double ts_mir_call_value_assign(void *ctx_ptr, const char *target_name, const char *name,
                                void *call_node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *call_node = (exprtk_node_t *)call_node_ptr;
  exprtk_value_t *args = NULL;
  exprtk_value_t result;
  size_t argc = 0;
  size_t declared_argc = 0;

  if (!ctx || !target_name || !name || !call_node) return 0.0;
  if (call_node->type == EXPRTK_NODE_FUNCTION_CALL) {
    declared_argc = call_node->data.function.arg_count;
  } else if (call_node->type == EXPRTK_NODE_MEMBER_CALL) {
    declared_argc = call_node->data.member_call.arg_count;
  }

  args = ts_mir_runtime_call_value_args(call_node, &ctx->env, &argc);
  if (!args && declared_argc > 0) {
    ctx->env.aborted = 1;
    return 0.0;
  }

  result = exprtk_call_internal(name, argc, args, &ctx->env);
  exprtk_env_set(&ctx->env, target_name, result);
  ts_mir_runtime_value_args_free(args, argc);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(result);
}

double ts_mir_value_expr_assign(void *ctx_ptr, const char *target_name, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t result;

  if (!ctx || !target_name || !node) return 0.0;
  if (!ts_mir_runtime_value_arg(node, &ctx->env, &result)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  exprtk_env_set(&ctx->env, target_name, result);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(result);
}

double ts_mir_value_expr(void *ctx_ptr, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t result;
  double numeric_result;

  if (!ctx || !node) return 0.0;
  if (!ts_mir_runtime_value_arg(node, &ctx->env, &result)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  numeric_result = ts_mir_numeric_value(result);
  exprtk_value_destroy(&result);
  return numeric_result;
}

exprtk_value_t turbo_script_mir_eval_node(const exprtk_node_t *node, exprtk_env_t *env) {
  exprtk_value_t result = exprtk_val_num(0.0);

  if (!node || !env) return result;
  if (!ts_mir_runtime_value_arg((exprtk_node_t *)node, env, &result)) {
    if (!env->aborted && env->error_msg[0] == '\0')
      ts_mir_value_arg_error(env, (exprtk_node_t *)node);
  }
  return result;
}

int turbo_script_mir_exec_script_body(exprtk_func_t *func, exprtk_env_t *local_env,
                                      exprtk_env_t *caller_env, exprtk_value_t *out) {
  exprtk_value_t result = exprtk_val_num(0.0);

  (void)caller_env;
  if (!func || !local_env || !out) return 1;
  if (!ts_mir_runtime_value_arg(func->data.script.body, local_env, &result)) {
    if (!local_env->aborted && local_env->error_msg[0] == '\0')
      ts_mir_value_arg_error(local_env, func->data.script.body);
    result = exprtk_val_num(0.0);
  }
  *out = result;
  return 1;
}

static int ts_mir_string_equals(exprtk_value_t value, const char *text) {
  size_t len = text ? strlen(text) : 0;
  return value.type == EXPRTK_VAL_STRING && value.data.string.data &&
         value.data.string.len == len &&
         memcmp(value.data.string.data, text, len) == 0;
}

static exprtk_value_t ts_mir_await_result(exprtk_value_t value) {
  if (exprtk_value_is_object_like(&value) && exprtk_map_has(&value, "status") &&
      exprtk_map_has(&value, "value")) {
    exprtk_value_t status = exprtk_map_get(&value, "status");
    if (ts_mir_string_equals(status, "suspended") || ts_mir_string_equals(status, "dead")) {
      return exprtk_map_get(&value, "value");
    }
  }
  return exprtk_value_borrow(value);
}

static int ts_mir_eval_await_arg(exprtk_node_t *node, exprtk_env_t *env, exprtk_value_t *out) {
  exprtk_value_t *args = NULL;
  size_t argc = 0;

  if (!node || !env || !out) return 0;

  if (node->type == EXPRTK_NODE_FUNCTION_CALL && node->data.function.name) {
    args = ts_mir_runtime_call_value_args(node, env, &argc);
    if (!args && node->data.function.arg_count > 0) return 0;
    *out = exprtk_call_internal(node->data.function.name, argc, args, env);
    ts_mir_runtime_value_args_free(args, argc);
    return env->flow == exprtk_FLOW_NORMAL && !env->aborted;
  }

  return ts_mir_runtime_value_arg(node, env, out);
}

double ts_mir_await_value(void *ctx_ptr, void *arg_node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *arg_node = (exprtk_node_t *)arg_node_ptr;
  exprtk_value_t value;
  exprtk_value_t awaited;
  double result;

  if (!ctx || !arg_node) return 0.0;
  if (!ts_mir_eval_await_arg(arg_node, &ctx->env, &value)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, arg_node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  awaited = ts_mir_await_result(value);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  result = ts_mir_numeric_value(awaited);
  exprtk_value_destroy(&value);
  return result;
}

double ts_mir_await_assign(void *ctx_ptr, const char *target_name, void *arg_node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *arg_node = (exprtk_node_t *)arg_node_ptr;
  exprtk_value_t value;
  exprtk_value_t awaited;
  double result;

  if (!ctx || !target_name || !arg_node) return 0.0;
  if (!ts_mir_eval_await_arg(arg_node, &ctx->env, &value)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, arg_node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  awaited = ts_mir_await_result(value);
  result = ts_mir_numeric_value(awaited);
  exprtk_env_set(&ctx->env, target_name, awaited);
  exprtk_value_destroy(&value);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return result;
}

double ts_mir_function_expr_assign(void *ctx_ptr, const char *target_name, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t value = {0};

  if (!ctx || !target_name || !node || node->type != EXPRTK_NODE_FUNCTION_EXPRESSION)
    return 0.0;

  value.type = EXPRTK_VAL_FUNCTION;
  value.data.function.arg_params = node->data.func_def.arg_params;
  value.data.function.arg_count = node->data.func_def.arg_count;
  value.data.function.body = node->data.func_def.body;
  value.data.function.closure_env = exprtk_env_snapshot(&ctx->env);
  value.data.function.owner_class = NULL;
  value.data.function.is_static_method = 0;
  value.data.function.access_level = EXPRTK_ACCESS_PUBLIC;
  exprtk_env_set(&ctx->env, target_name, value);
  return 0.0;
}

double ts_mir_try_catch_assign(void *ctx_ptr, const char *target_name, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t result;

  if (!ctx || !target_name || !node || node->type != EXPRTK_NODE_TRY_CATCH) return 0.0;

  if (!ts_mir_runtime_value_arg(node, &ctx->env, &result)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }
  exprtk_env_set(&ctx->env, target_name, result);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(result);
}
