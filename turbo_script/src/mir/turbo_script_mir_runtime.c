/* Runtime Bridges Module - MIR Backend
 * Extracted from turbo_script_mir.c
 * 
 * This module handles initialization of MIR external function prototypes
 * and registration of C runtime function pointers with the MIR context.
 */

#include "../turbo_script_internal.h"
#include "turbo_script_mir_internal.h"
#include "../host/turbo_script_host_internal.h"
#include "exprtk_class.h"
#include <mir.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int exprtk_datetime_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_date_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_time_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_duration_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);
int exprtk_decimal_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out);

static const char *ts_mir_prefixed_item_name(ts_mir_compiler_t *c, const char *name) {
  char buf[192];
  snprintf(buf, sizeof(buf), "%s_%s", c && c->item_prefix[0] ? c->item_prefix : "ts_mir", name);
  return strdup(buf);
}

/* =========================================================================
 * Runtime Bridge Bodies
 * ========================================================================= */

double ts_mir_numeric_value(exprtk_value_t val) {
  if (val.type == EXPRTK_VAL_INTEGER) return (double)val.data.integer;
  if (val.type == EXPRTK_VAL_NUMBER) return val.data.number;
  if (val.type == EXPRTK_VAL_BOOL) return val.data.boolean ? 1.0 : 0.0;
  return 0.0;
}

static double ts_mir_take_numeric_value(exprtk_value_t *value) {
  double result;
  if (!value) return 0.0;
  result = ts_mir_numeric_value(*value);
  exprtk_value_destroy(value);
  return result;
}

void ts_mir_promote_env_error(turbo_script_ctx_t *ctx) {
  exprtk_env_t *env;
  if (!ctx) return;
  env = ts_task_execution_env(ctx);

  if (env == &ctx->env) {
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE)
      ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    if (ctx->error_msg[0] == '\0') {
      const char *msg = env->error_msg[0] ? env->error_msg : "JIT runtime error";
      snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg);
    }
  }
  env->aborted = 1;
}

double ts_mir_load_var(void *ctx_ptr, const char *name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  return ts_mir_numeric_value(val);
}

void ts_mir_store_var(void *ctx_ptr, const char *name, double value) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_val_num(value);
  exprtk_env_set(&ctx->env, name, val);
}

double ts_mir_assign_var(void *ctx_ptr, const char *target_name, const char *source_name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val;

  if (!ctx || !target_name || !source_name) return 0.0;

  val = exprtk_env_get(&ctx->env, source_name);
  exprtk_env_set(&ctx->env, target_name, val);
  return ts_mir_numeric_value(val);
}

double ts_mir_vector_assign(void *ctx_ptr, const char *target_name, int64_t count,
                            double *values) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val;
  if (!ctx || !target_name || count < 0) return 0.0;

  val = exprtk_val_vec(values, (size_t)count);
  exprtk_env_set(&ctx->env, target_name, val);
  return 0.0;
}

double ts_mir_map_assign(void *ctx_ptr, const char *target_name, void *node_ptr, int64_t count,
                         double *values) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t map;

  if (!ctx || !target_name || !node || node->type != EXPRTK_NODE_MAP_LITERAL || count < 0)
    return 0.0;

  map = exprtk_val_map();
  for (int64_t i = 0; i < count && (size_t)i < node->data.map_literal.count; ++i) {
    if (!node->data.map_literal.keys[i]) continue;
    exprtk_map_set(&map, node->data.map_literal.keys[i], exprtk_val_num(values[i]));
  }

  exprtk_env_set(&ctx->env, target_name, map);
  exprtk_map_free(&map);
  return 0.0;
}

int ts_mir_runtime_define_func_in_env(exprtk_env_t *env, exprtk_node_t *node) {
  exprtk_func_t *curr = NULL;

  if (!env || !node || node->type != EXPRTK_NODE_FUNCTION_DEFINITION ||
      !node->data.func_def.name) {
    return 0;
  }

  curr = env->funcs;
  while (curr) {
    if (curr->name && strcmp(curr->name, node->data.func_def.name) == 0) break;
    curr = curr->next;
  }

  if (!curr) {
    curr = (exprtk_func_t *)calloc(1, sizeof(exprtk_func_t));
    if (!curr) return 0;

    curr->name = strdup(node->data.func_def.name);
    if (!curr->name) {
      free(curr);
      return 0;
    }

    curr->next = env->funcs;
    env->funcs = curr;
  }

  curr->is_script = 1;
  curr->owner_class = NULL;
  curr->closure_env = env;
  exprtk_env_retain(env);
  curr->is_static_method = 0;
  curr->access_level = EXPRTK_ACCESS_PUBLIC;
  curr->is_override = 0;
  curr->is_final = 0;
  curr->data.script.arg_count = node->data.func_def.arg_count;

  if (curr->data.script.arg_count > 0) {
    curr->data.script.arg_params =
        (exprtk_node_t **)calloc(curr->data.script.arg_count, sizeof(exprtk_node_t *));
    if (!curr->data.script.arg_params) return 0;

    for (size_t i = 0; i < curr->data.script.arg_count; ++i) {
      curr->data.script.arg_params[i] =
          exprtk_node_copy(node->data.func_def.arg_params[i], &env->arena);
      if (!curr->data.script.arg_params[i]) return 0;
    }
  } else {
    curr->data.script.arg_params = NULL;
  }

  curr->data.script.body = exprtk_node_copy(node->data.func_def.body, &env->arena);
  return curr->data.script.body || !node->data.func_def.body;
}

void ts_mir_define_func(void *ctx_ptr, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  if (!ctx || !node || node->type != EXPRTK_NODE_FUNCTION_DEFINITION ||
      !node->data.func_def.name) {
    if (ctx) {
      ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
      snprintf(ctx->error_msg, sizeof(ctx->error_msg),
               "MIR runtime error: invalid function definition");
      ctx->env.aborted = 1;
    }
    return;
  }

  exprtk_env_t *env = &ctx->env;
  if (!ts_mir_runtime_define_func_in_env(env, node)) {
    ctx->error_code = TURBO_SCRIPT_ERROR_OOM;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "MIR runtime error: failed to define function");
    env->aborted = 1;
  }
}

double ts_mir_string_assign(void *ctx_ptr, const char *target_name, const char *data,
                            int64_t len) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val;
  if (!ctx || !target_name || !data || len < 0) return 0.0;

  val = exprtk_val_str(vstr_from_buf((char *)data, (size_t)len));
  exprtk_env_set(&ctx->env, target_name, val);
  return 0.0;
}

static exprtk_value_t ts_mir_call_bridge(void *ctx_ptr, const char *name, size_t argc,
                                         double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (size_t i = 0; i < argc; i++) {
    args[i] = exprtk_val_num(argv[i]);
  }
  return exprtk_call_internal(name, argc, args, &ctx->env);
}

double ts_mir_call0(void *ctx_ptr, const char *name) {
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 0, NULL);
  return ts_mir_take_numeric_value(&r);
}

double ts_mir_call1(void *ctx_ptr, const char *name, double a0) {
  double argv[1] = {a0};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 1, argv);
  return ts_mir_take_numeric_value(&r);
}

double ts_mir_call2(void *ctx_ptr, const char *name, double a0, double a1) {
  double argv[2] = {a0, a1};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 2, argv);
  return ts_mir_take_numeric_value(&r);
}

double ts_mir_call3(void *ctx_ptr, const char *name, double a0, double a1, double a2) {
  double argv[3] = {a0, a1, a2};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 3, argv);
  return ts_mir_take_numeric_value(&r);
}

double ts_mir_calln(void *ctx_ptr, const char *name, int64_t argc, double *argv) {
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, (size_t)argc, argv);
  return ts_mir_take_numeric_value(&r);
}

double ts_mir_call_assign(void *ctx_ptr, const char *target_name, const char *name,
                          int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !name) return 0.0;

  exprtk_value_t result = ts_mir_call_bridge(ctx_ptr, name, (size_t)argc, argv);
  exprtk_env_set(&ctx->env, target_name, result);
  return ts_mir_numeric_value(result);
}

double ts_mir_call_native(void *ctx_ptr, void *fn_ptr, void *user_data, int64_t argc,
                          double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_native_fn fn = (exprtk_native_fn)fn_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (int64_t i = 0; i < argc; i++) {
    args[i] = exprtk_val_num(argv[i]);
  }
  exprtk_env_t *env = ts_task_execution_env(ctx);
  exprtk_value_t raw = fn((size_t)argc, args, env, user_data);
  exprtk_value_t result = exprtk_value_clone_to_env(raw, env);
  exprtk_value_destroy(&raw);
  return ts_mir_take_numeric_value(&result);
}

double ts_mir_call_host_slot(void *ctx_ptr, int64_t slot, int64_t argc,
                             double *argv) {
  if (!ctx_ptr || slot < 0 || argc < 0 || argc > 16) return 0.0;
  return ts_host_registry_invoke_numeric_slot((turbo_script_ctx_t *)ctx_ptr,
                                              (size_t)slot, (size_t)argc, argv);
}

double ts_mir_call_builtin(void *ctx_ptr, void *fn_ptr, int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_builtin_fn fn = (exprtk_builtin_fn)fn_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (int64_t i = 0; i < argc; i++) {
    args[i] = exprtk_val_num(argv[i]);
  }
  exprtk_value_t r = exprtk_call_builtin(fn, (size_t)argc, args, &ctx->env);
  return ts_mir_take_numeric_value(&r);
}

double ts_mir_vec_get(void *ctx_ptr, const char *name, double index) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  if (val.type == EXPRTK_VAL_VECTOR) {
    int idx = (int)index;
    if (idx >= 0 && idx < (int)val.data.vector.size) return val.data.vector.data[idx];
  }
  return 0.0;
}

double ts_mir_load_captured_var(void *ctx_ptr, const char *var_name, void *closure_env_ptr) {
  (void)ctx_ptr;

  if (!closure_env_ptr || !var_name) return 0.0;

  exprtk_env_t *closure_env = (exprtk_env_t *)closure_env_ptr;
  exprtk_value_t val = exprtk_env_get(closure_env, var_name);

  return ts_mir_numeric_value(val);
}

double ts_mir_member_get(void *ctx_ptr, const char *obj_name, const char *member,
                         void *cache_ptr, void *object_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (obj.type == EXPRTK_VAL_VECTOR) {
    if (strcmp(member, "length") == 0) return (double)obj.data.vector.size;
  } else if (obj.type == EXPRTK_VAL_STRING) {
    if (strcmp(member, "length") == 0) return (double)obj.data.string.len;
  } else if (obj.type == EXPRTK_VAL_DATETIME) {
    exprtk_value_t val;
    if (exprtk_datetime_member_get(obj, member, &val)) return ts_mir_numeric_value(val);
  } else if (obj.type == EXPRTK_VAL_DATE) {
    exprtk_value_t val;
    if (exprtk_date_member_get(obj, member, &val)) return ts_mir_numeric_value(val);
  } else if (obj.type == EXPRTK_VAL_TIME) {
    exprtk_value_t val;
    if (exprtk_time_member_get(obj, member, &val)) return ts_mir_numeric_value(val);
  } else if (obj.type == EXPRTK_VAL_DURATION) {
    exprtk_value_t val;
    if (exprtk_duration_member_get(obj, member, &val)) return ts_mir_numeric_value(val);
  } else if (obj.type == EXPRTK_VAL_DECIMAL) {
    exprtk_value_t val;
    if (exprtk_decimal_member_get(obj, member, &val)) return ts_mir_numeric_value(val);
  } else if (exprtk_value_is_object_like(&obj)) {
    exprtk_value_t val = exprtk_map_get(&obj, member);
    return ts_mir_numeric_value(val);
  }

  return ts_mir_numeric_value(exprtk_oop_get_member_cached_checked(
      obj_name, member, (exprtk_oop_field_cache_t *)cache_ptr,
      (exprtk_node_t *)object_node, &ctx->env));
}

double ts_mir_member_get_assign(void *ctx_ptr, const char *target_name, const char *obj_name,
                                const char *member, void *cache_ptr, void *object_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !obj_name || !member) return 0.0;

  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  exprtk_value_t value = exprtk_val_num(0.0);

  if (obj.type == EXPRTK_VAL_VECTOR) {
    if (strcmp(member, "length") == 0) value = exprtk_val_num((double)obj.data.vector.size);
  } else if (obj.type == EXPRTK_VAL_STRING) {
    if (strcmp(member, "length") == 0) value = exprtk_val_num((double)obj.data.string.len);
  } else if (obj.type == EXPRTK_VAL_DATETIME) {
    (void)exprtk_datetime_member_get(obj, member, &value);
  } else if (obj.type == EXPRTK_VAL_DATE) {
    (void)exprtk_date_member_get(obj, member, &value);
  } else if (obj.type == EXPRTK_VAL_TIME) {
    (void)exprtk_time_member_get(obj, member, &value);
  } else if (obj.type == EXPRTK_VAL_DURATION) {
    (void)exprtk_duration_member_get(obj, member, &value);
  } else if (obj.type == EXPRTK_VAL_DECIMAL) {
    (void)exprtk_decimal_member_get(obj, member, &value);
  } else if (exprtk_value_is_object_like(&obj)) {
    value = exprtk_map_get(&obj, member);
  } else {
    value = exprtk_oop_get_member_cached_checked(
        obj_name, member, (exprtk_oop_field_cache_t *)cache_ptr,
        (exprtk_node_t *)object_node, &ctx->env);
  }

  exprtk_env_set(&ctx->env, target_name, value);
  return ts_mir_numeric_value(value);
}

void *ts_mir_vec_data(void *ctx_ptr, const char *name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  if (val.type == EXPRTK_VAL_VECTOR && val.data.vector.data) return (void *)val.data.vector.data;
  return NULL;
}

double ts_mir_map_get_key(void *ctx_ptr, const char *obj_name, const char *key) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (exprtk_value_is_object_like(&obj)) {
    exprtk_value_t val = exprtk_map_get(&obj, key);
    return ts_mir_numeric_value(val);
  }
  return 0.0;
}

void *ts_mir_map_num_ptr(void *ctx_ptr, const char *obj_name, const char *key) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (exprtk_value_is_object_like(&obj)) {
    exprtk_value_t *vp = exprtk_map_get_ptr(&obj, key);
    if (vp) {
      if (vp->type == EXPRTK_VAL_INTEGER) {
        vp->type = EXPRTK_VAL_NUMBER;
        vp->data.number = (double)vp->data.integer;
      }
      if (vp->type == EXPRTK_VAL_NUMBER) return (void *)&vp->data.number;
    }
  }
  return NULL;
}

void ts_mir_init_externals(ts_mir_compiler_t *c) {
  MIR_context_t ctx = c->ctx;

  /* fmod(double, double) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    c->ext.fmod_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_fmod"), 1, &res, 2, args);
    c->ext.fmod_import = MIR_new_import(ctx, "fmod");
  }

  /* pow(double, double) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    c->ext.pow_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_pow"), 1, &res, 2, args);
    c->ext.pow_import = MIR_new_import(ctx, "pow");
  }

  /* Single-argument math functions: sin, cos, sqrt, tan, asin, acos, atan, log, exp, floor, ceil, round, fabs */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[1] = {{MIR_T_D, "x", 0}};
    
    c->ext.sin_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_sin"), 1, &res, 1, args);
    c->ext.sin_import = MIR_new_import(ctx, "sin");
    
    c->ext.cos_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_cos"), 1, &res, 1, args);
    c->ext.cos_import = MIR_new_import(ctx, "cos");
    
    c->ext.sqrt_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_sqrt"), 1, &res, 1, args);
    c->ext.sqrt_import = MIR_new_import(ctx, "sqrt");
    
    c->ext.tan_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_tan"), 1, &res, 1, args);
    c->ext.tan_import = MIR_new_import(ctx, "tan");
    
    c->ext.asin_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_asin"), 1, &res, 1, args);
    c->ext.asin_import = MIR_new_import(ctx, "asin");
    
    c->ext.acos_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_acos"), 1, &res, 1, args);
    c->ext.acos_import = MIR_new_import(ctx, "acos");
    
    c->ext.atan_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_atan"), 1, &res, 1, args);
    c->ext.atan_import = MIR_new_import(ctx, "atan");
    
    c->ext.log_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_log"), 1, &res, 1, args);
    c->ext.log_import = MIR_new_import(ctx, "log");
    
    c->ext.exp_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_exp"), 1, &res, 1, args);
    c->ext.exp_import = MIR_new_import(ctx, "exp");
    
    c->ext.floor_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_floor"), 1, &res, 1, args);
    c->ext.floor_import = MIR_new_import(ctx, "floor");
    
    c->ext.ceil_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_ceil"), 1, &res, 1, args);
    c->ext.ceil_import = MIR_new_import(ctx, "ceil");
    
    c->ext.round_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_round"), 1, &res, 1, args);
    c->ext.round_import = MIR_new_import(ctx, "round");
    
    c->ext.fabs_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_fabs"), 1, &res, 1, args);
    c->ext.fabs_import = MIR_new_import(ctx, "fabs");
  }

  /* Two-argument math functions: atan2, fmax, fmin */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    
    c->ext.atan2_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_atan2"), 1, &res, 2, args);
    c->ext.atan2_import = MIR_new_import(ctx, "atan2");
    
    c->ext.fmax_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_fmax"), 1, &res, 2, args);
    c->ext.fmax_import = MIR_new_import(ctx, "fmax");
    
    c->ext.fmin_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_fmin"), 1, &res, 2, args);
    c->ext.fmin_import = MIR_new_import(ctx, "fmin");
  }

  /* ts_mir_load_var(void *ctx, const char *name) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.load_var_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_load_var"), 1, &res, 2, args);
    c->ext.load_var_import = MIR_new_import(ctx, "ts_mir_load_var");
  }

  /* ts_mir_store_var(void *ctx, const char *name, double value) -> void */
  {
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "value", 0}};
    c->ext.store_var_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_store_var"), 0, NULL, 3, args);
    c->ext.store_var_import = MIR_new_import(ctx, "ts_mir_store_var");
  }

  /* ts_mir_assign_var(void *ctx, const char *target, const char *source) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "source", 0}};
    c->ext.assign_var_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_assign_var"), 1, &res, 3, args);
    c->ext.assign_var_import = MIR_new_import(ctx, "ts_mir_assign_var");
  }

  /* ts_mir_call0(void *ctx, const char *name) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.call0_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call0"), 1, &res, 2, args);
    c->ext.call0_import = MIR_new_import(ctx, "ts_mir_call0");
  }

  /* ts_mir_call1(void *ctx, const char *name, double a0) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "a0", 0}};
    c->ext.call1_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call1"), 1, &res, 3, args);
    c->ext.call1_import = MIR_new_import(ctx, "ts_mir_call1");
  }

  /* ts_mir_call2(void *ctx, const char *name, double a0, double a1) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "a0", 0}, {MIR_T_D, "a1", 0}};
    c->ext.call2_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call2"), 1, &res, 4, args);
    c->ext.call2_import = MIR_new_import(ctx, "ts_mir_call2");
  }

  /* ts_mir_call3(void *ctx, const char *name, double a0, a1, a2) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "name", 0},
                         {MIR_T_D, "a0", 0},
                         {MIR_T_D, "a1", 0},
                         {MIR_T_D, "a2", 0}};
    c->ext.call3_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call3"), 1, &res, 5, args);
    c->ext.call3_import = MIR_new_import(ctx, "ts_mir_call3");
  }

  /* ts_mir_calln(void *ctx, const char *name, int64_t argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.calln_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_calln"), 1, &res, 4, args);
    c->ext.calln_import = MIR_new_import(ctx, "ts_mir_calln");
  }
  /* ts_mir_call_assign(void *ctx, const char *target, const char *name, int64_t argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target_name", 0},
                         {MIR_T_P, "name", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.call_assign_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call_assign"), 1, &res, 5, args);
    c->ext.call_assign_import = MIR_new_import(ctx, "ts_mir_call_assign");
  }
  /* ts_mir_call_value_assign(void *ctx, const char *target, const char *name,
   *                          void *call_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target_name", 0},
                         {MIR_T_P, "name", 0},
                         {MIR_T_P, "call_node", 0}};
    c->ext.call_value_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call_value_assign"), 1, &res, 4, args);
    c->ext.call_value_assign_import = MIR_new_import(ctx, "ts_mir_call_value_assign");
  }
  /* ts_mir_value_expr(void *ctx, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.value_expr_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_value_expr"), 1, &res, 2, args);
    c->ext.value_expr_import = MIR_new_import(ctx, "ts_mir_value_expr");
  }
  /* ts_mir_value_expr_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "node", 0}};
    c->ext.value_expr_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_value_expr_assign"), 1, &res, 3, args);
    c->ext.value_expr_assign_import = MIR_new_import(ctx, "ts_mir_value_expr_assign");
  }
  /* ts_mir_await_value(void *ctx, void *arg_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "arg_node", 0}};
    c->ext.await_value_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_await_value"), 1, &res, 2, args);
    c->ext.await_value_import = MIR_new_import(ctx, "ts_mir_await_value");
  }
  /* ts_mir_await_assign(void *ctx, const char *target, void *arg_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "arg_node", 0}};
    c->ext.await_assign_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_await_assign"), 1, &res, 3, args);
    c->ext.await_assign_import = MIR_new_import(ctx, "ts_mir_await_assign");
  }
  /* ts_mir_function_expr_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "node", 0}};
    c->ext.function_expr_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_function_expr_assign"), 1, &res, 3, args);
    c->ext.function_expr_assign_import = MIR_new_import(ctx, "ts_mir_function_expr_assign");
  }
  /* ts_mir_try_catch_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "node", 0}};
    c->ext.try_catch_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_try_catch_assign"), 1, &res, 3, args);
    c->ext.try_catch_assign_import = MIR_new_import(ctx, "ts_mir_try_catch_assign");
  }

  /* ts_mir_vec_get(void *ctx, const char *name, double index) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "index", 0}};
    c->ext.vec_get_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_vec_get"), 1, &res, 3, args);
    c->ext.vec_get_import = MIR_new_import(ctx, "ts_mir_vec_get");
  }

  /* ts_mir_vector_assign(void *ctx, const char *target, int64_t count, double *values) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_I64, "count", 0},
                         {MIR_T_P, "values", 0}};
    c->ext.vec_assign_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_vec_assign"), 1, &res, 4, args);
    c->ext.vec_assign_import = MIR_new_import(ctx, "ts_mir_vector_assign");
  }

  /* ts_mir_destructure_var(void *ctx, void *target, const char *value_name,
   *                        int64_t is_constant) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "value_name", 0},
                         {MIR_T_I64, "is_constant", 0}};
    c->ext.destruct_var_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_destruct_var"), 1, &res, 4, args);
    c->ext.destruct_var_import = MIR_new_import(ctx, "ts_mir_destructure_var");
  }

  /* ts_mir_destructure_vector(void *ctx, void *target, int64_t is_constant,
   *                           int64_t count, double *values) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_I64, "is_constant", 0},
                         {MIR_T_I64, "count", 0},
                         {MIR_T_P, "values", 0}};
    c->ext.destruct_vector_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_destruct_vector"), 1, &res, 5, args);
    c->ext.destruct_vector_import = MIR_new_import(ctx, "ts_mir_destructure_vector");
  }

  /* ts_mir_string_assign(void *ctx, const char *target, const char *data,
   *                      int64_t len) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "data", 0},
                         {MIR_T_I64, "len", 0}};
    c->ext.string_assign_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_string_assign"), 1, &res, 4, args);
    c->ext.string_assign_import = MIR_new_import(ctx, "ts_mir_string_assign");
  }

  /* ts_mir_template_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "node", 0}};
    c->ext.template_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_template_assign"), 1, &res, 3, args);
    c->ext.template_assign_import = MIR_new_import(ctx, "ts_mir_template_assign");
  }

  /* ts_mir_define_func(void *ctx, void *node) -> void */
  {
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.define_func_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_define_func"), 0, NULL, 2, args);
    c->ext.define_func_import = MIR_new_import(ctx, "ts_mir_define_func");
  }

  /* ts_mir_member_get(void *ctx, const char *obj_name, const char *member,
   *                   void *cache, void *object_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "obj_name", 0},
                         {MIR_T_P, "member", 0},
                         {MIR_T_P, "cache", 0},
                         {MIR_T_P, "object_node", 0}};
    c->ext.member_get_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_member_get"), 1, &res, 5, args);
    c->ext.member_get_import = MIR_new_import(ctx, "ts_mir_member_get");
  }

  /* ts_mir_member_get_assign(void *ctx, const char *target, const char *obj,
   *                          const char *member, void *cache, void *object_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[6] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target_name", 0},
                         {MIR_T_P, "obj_name", 0},
                         {MIR_T_P, "member", 0},
                         {MIR_T_P, "cache", 0},
                         {MIR_T_P, "object_node", 0}};
    c->ext.member_get_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_member_get_assign"), 1, &res, 6, args);
    c->ext.member_get_assign_import = MIR_new_import(ctx, "ts_mir_member_get_assign");
  }

  /* ts_mir_vec_data(void *ctx, const char *name) -> void* (returned as i64) */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.vec_data_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_vec_data"), 1, &res, 2, args);
    c->ext.vec_data_import = MIR_new_import(ctx, "ts_mir_vec_data");
  }

  /* ts_mir_map_get_key(void *ctx, const char *obj_name, const char *key) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "key", 0}};
    c->ext.map_get_key_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_map_get_key"), 1, &res, 3, args);
    c->ext.map_get_key_import = MIR_new_import(ctx, "ts_mir_map_get_key");
  }

  /* ts_mir_map_assign(void *ctx, const char *target, void *node, int64_t count,
   *                   double *values) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "node", 0},
                         {MIR_T_I64, "count", 0},
                         {MIR_T_P, "values", 0}};
    c->ext.map_assign_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_map_assign"), 1, &res, 5, args);
    c->ext.map_assign_import = MIR_new_import(ctx, "ts_mir_map_assign");
  }

  /* ts_mir_map_value_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target", 0}, {MIR_T_P, "node", 0}};
    c->ext.map_value_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_map_value_assign"), 1, &res, 3, args);
    c->ext.map_value_assign_import = MIR_new_import(ctx, "ts_mir_map_value_assign");
  }

  /* ts_mir_map_num_ptr(void *ctx, const char *obj_name, const char *key) -> void* (as i64) */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "key", 0}};
    c->ext.map_num_ptr_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_map_num_ptr"), 1, &res, 3, args);
    c->ext.map_num_ptr_import = MIR_new_import(ctx, "ts_mir_map_num_ptr");
  }

  /* ts_mir_load_captured_var(void *ctx, const char *name, void *closure_env) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "closure_env", 0}};
    c->ext.load_captured_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_load_captured"), 1, &res, 3, args);
    c->ext.load_captured_import = MIR_new_import(ctx, "ts_mir_load_captured_var");
  }

  /*  ts_mir_call_native(void *ctx, void *fn, void *ud, i64 argc, void *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "fn", 0},
                         {MIR_T_P, "ud", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.call_native_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call_native"), 1, &res, 5, args);
    c->ext.call_native_import = MIR_new_import(ctx, "ts_mir_call_native");
  }
  /* Frozen Host callsites carry a registry slot, never a mutable name lookup. */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {{MIR_T_P, "ctx", 0}, {MIR_T_I64, "slot", 0},
                         {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.call_host_slot_proto = MIR_new_proto_arr(
        ctx, ts_mir_prefixed_item_name(c, "p_call_host_slot"), 1, &res, 4, args);
    c->ext.call_host_slot_import = MIR_new_import(ctx, "ts_mir_call_host_slot");
  }
  /*  ts_mir_call_builtin(void *ctx, void *fn, i64 argc, void *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "fn", 0}, {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.call_builtin_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_call_builtin"), 1, &res, 4, args);
    c->ext.call_builtin_import = MIR_new_import(ctx, "ts_mir_call_builtin");
  }

  /* ts_mir_oop_define_class(void *ctx, void *node) -> void */
  {
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.oop_define_class_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_define_class"), 0, NULL, 2, args);
    c->ext.oop_define_class_import = MIR_new_import(ctx, "ts_mir_oop_define_class");
  }

  /* ts_mir_oop_class_alias(void *ctx, const char *target, const char *source) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "source_name", 0}};
    c->ext.oop_alias_class_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_class_alias"), 1, &res, 3, args);
    c->ext.oop_alias_class_import = MIR_new_import(ctx, "ts_mir_oop_class_alias");
  }

  /* ts_mir_oop_new_assign(void *ctx, const char *target, const char *class, i64 argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "class_name", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.oop_new_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_new_assign"), 1, &res, 5, args);
    c->ext.oop_new_assign_import = MIR_new_import(ctx, "ts_mir_oop_new_assign");
  }

  /* ts_mir_oop_member_call(void *ctx, const char *object, const char *method,
   *                        void *object_node, i64 argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[6] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "method_name", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.oop_member_call_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_call"), 1, &res, 6, args);
    c->ext.oop_member_call_import = MIR_new_import(ctx, "ts_mir_oop_member_call");
  }

  /* ts_mir_oop_member_call_cached(void *ctx, const char *object, const char *method,
   *                               void *cache, void *object_node, i64 argc,
   *                               double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[7] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "method_name", 0},
                         {MIR_T_P, "cache", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.oop_member_call_cached_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_call_cached"), 1, &res, 7, args);
    c->ext.oop_member_call_cached_import = MIR_new_import(ctx, "ts_mir_oop_member_call_cached");
  }

  /* ts_mir_oop_member_call_assign(void *ctx, const char *target, const char *object,
   *                               const char *method, void *cache, void *object_node,
   *                               i64 argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[8] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target_name", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "method_name", 0},
                         {MIR_T_P, "cache", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.oop_member_call_assign_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_call_assign"), 1, &res, 8, args);
    c->ext.oop_member_call_assign_import =
        MIR_new_import(ctx, "ts_mir_oop_member_call_assign");
  }

  /* ts_mir_oop_member_call_value(void *ctx, const char *object, const char *method,
   *                              void *object_node, void *call_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "method_name", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_P, "call_node", 0}};
    c->ext.oop_member_call_value_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_call_value"), 1, &res, 5, args);
    c->ext.oop_member_call_value_import =
        MIR_new_import(ctx, "ts_mir_oop_member_call_value");
  }

  /* ts_mir_oop_member_call_mono_value(void *ctx, const char *object,
   *                                   const char *expected_class, const char *method,
   *                                   void *object_node, void *call_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[6] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "expected_class_name", 0},
                         {MIR_T_P, "method_name", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_P, "call_node", 0}};
    c->ext.oop_member_call_mono_value_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_call_mono_value"), 1, &res, 6, args);
    c->ext.oop_member_call_mono_value_import =
        MIR_new_import(ctx, "ts_mir_oop_member_call_mono_value");
  }

  /* ts_mir_oop_member_call_assign_value(void *ctx, const char *target,
   *                                     const char *object, const char *method,
   *                                     void *object_node, void *call_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[6] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target_name", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "method_name", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_P, "call_node", 0}};
    c->ext.oop_member_call_assign_value_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_call_assign_value"), 1, &res, 6, args);
    c->ext.oop_member_call_assign_value_import =
        MIR_new_import(ctx, "ts_mir_oop_member_call_assign_value");
  }

  /* ts_mir_oop_member_get(void *ctx, const char *object, const char *member,
   *                       void *cache, void *object_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "member_name", 0},
                         {MIR_T_P, "cache", 0},
                         {MIR_T_P, "object_node", 0}};
    c->ext.oop_member_get_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_get"), 1, &res, 5, args);
    c->ext.oop_member_get_import = MIR_new_import(ctx, "ts_mir_oop_member_get");
  }

  /* ts_mir_oop_member_set(void *ctx, const char *object, const char *member,
   *                       void *cache, void *object_node, double value) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[6] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "member_name", 0},
                         {MIR_T_P, "cache", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_D, "value", 0}};
    c->ext.oop_member_set_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_member_set"), 1, &res, 6, args);
    c->ext.oop_member_set_import = MIR_new_import(ctx, "ts_mir_oop_member_set");
  }

  /* ts_mir_oop_num_ptr(void *ctx, const char *object, const char *member,
   *                    void *object_node, i64 create, void *slot_cache) -> void* */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[6] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "object_name", 0},
                         {MIR_T_P, "member_name", 0},
                         {MIR_T_P, "object_node", 0},
                         {MIR_T_I64, "create_if_missing", 0},
                         {MIR_T_P, "slot_cache", 0}};
    c->ext.oop_num_ptr_proto = MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_num_ptr"), 1, &res, 6, args);
    c->ext.oop_num_ptr_import = MIR_new_import(ctx, "ts_mir_oop_num_ptr");
  }

  /* ts_mir_oop_instanceof(void *ctx, const char *object, const char *class) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "object_name", 0}, {MIR_T_P, "class_name", 0}};
    c->ext.oop_instanceof_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_instanceof"), 1, &res, 3, args);
    c->ext.oop_instanceof_import = MIR_new_import(ctx, "ts_mir_oop_instanceof");
  }

  /* ts_mir_oop_predicate(void *ctx, i64 kind, const char *name,
   *                      const char *member, void *object_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_I64, "kind", 0},
                         {MIR_T_P, "name", 0},
                         {MIR_T_P, "member", 0},
                         {MIR_T_P, "object_node", 0}};
    c->ext.oop_predicate_proto =
        MIR_new_proto_arr(ctx, ts_mir_prefixed_item_name(c, "p_oop_predicate"), 1, &res, 5, args);
    c->ext.oop_predicate_import = MIR_new_import(ctx, "ts_mir_oop_predicate");
  }
}

void ts_mir_load_externals(MIR_context_t ctx) {
  /* Math functions */
  MIR_load_external(ctx, "fmod", (void *)fmod);
  MIR_load_external(ctx, "pow", (void *)pow);
  MIR_load_external(ctx, "sin", (void *)sin);
  MIR_load_external(ctx, "cos", (void *)cos);
  MIR_load_external(ctx, "sqrt", (void *)sqrt);
  MIR_load_external(ctx, "tan", (void *)tan);
  MIR_load_external(ctx, "asin", (void *)asin);
  MIR_load_external(ctx, "acos", (void *)acos);
  MIR_load_external(ctx, "atan", (void *)atan);
  MIR_load_external(ctx, "atan2", (void *)atan2);
  MIR_load_external(ctx, "log", (void *)log);
  MIR_load_external(ctx, "exp", (void *)exp);
  MIR_load_external(ctx, "floor", (void *)floor);
  MIR_load_external(ctx, "ceil", (void *)ceil);
  MIR_load_external(ctx, "round", (void *)round);
  MIR_load_external(ctx, "fabs", (void *)fabs);
  MIR_load_external(ctx, "fmax", (void *)fmax);
  MIR_load_external(ctx, "fmin", (void *)fmin);
  
  /* Runtime bridge functions */
  MIR_load_external(ctx, "ts_mir_load_var", (void *)ts_mir_load_var);
  MIR_load_external(ctx, "ts_mir_store_var", (void *)ts_mir_store_var);
  MIR_load_external(ctx, "ts_mir_assign_var", (void *)ts_mir_assign_var);
  MIR_load_external(ctx, "ts_mir_call0", (void *)ts_mir_call0);
  MIR_load_external(ctx, "ts_mir_call1", (void *)ts_mir_call1);
  MIR_load_external(ctx, "ts_mir_call2", (void *)ts_mir_call2);
  MIR_load_external(ctx, "ts_mir_call3", (void *)ts_mir_call3);
  MIR_load_external(ctx, "ts_mir_calln", (void *)ts_mir_calln);
  MIR_load_external(ctx, "ts_mir_call_assign", (void *)ts_mir_call_assign);
  MIR_load_external(ctx, "ts_mir_call_value_assign", (void *)ts_mir_call_value_assign);
  MIR_load_external(ctx, "ts_mir_value_expr", (void *)ts_mir_value_expr);
  MIR_load_external(ctx, "ts_mir_value_expr_assign", (void *)ts_mir_value_expr_assign);
  MIR_load_external(ctx, "ts_mir_await_value", (void *)ts_mir_await_value);
  MIR_load_external(ctx, "ts_mir_await_assign", (void *)ts_mir_await_assign);
  MIR_load_external(ctx, "ts_mir_function_expr_assign", (void *)ts_mir_function_expr_assign);
  MIR_load_external(ctx, "ts_mir_try_catch_assign", (void *)ts_mir_try_catch_assign);
  MIR_load_external(ctx, "ts_mir_vec_get", (void *)ts_mir_vec_get);
  MIR_load_external(ctx, "ts_mir_vector_assign", (void *)ts_mir_vector_assign);
  MIR_load_external(ctx, "ts_mir_destructure_var", (void *)ts_mir_destructure_var);
  MIR_load_external(ctx, "ts_mir_destructure_vector", (void *)ts_mir_destructure_vector);
  MIR_load_external(ctx, "ts_mir_string_assign", (void *)ts_mir_string_assign);
  MIR_load_external(ctx, "ts_mir_template_assign", (void *)ts_mir_template_assign);
  MIR_load_external(ctx, "ts_mir_define_func", (void *)ts_mir_define_func);
  MIR_load_external(ctx, "ts_mir_member_get", (void *)ts_mir_member_get);
  MIR_load_external(ctx, "ts_mir_member_get_assign", (void *)ts_mir_member_get_assign);
  MIR_load_external(ctx, "ts_mir_vec_data", (void *)ts_mir_vec_data);
  MIR_load_external(ctx, "ts_mir_map_get_key", (void *)ts_mir_map_get_key);
  MIR_load_external(ctx, "ts_mir_map_assign", (void *)ts_mir_map_assign);
  MIR_load_external(ctx, "ts_mir_map_value_assign", (void *)ts_mir_map_value_assign);
  MIR_load_external(ctx, "ts_mir_map_num_ptr", (void *)ts_mir_map_num_ptr);
  MIR_load_external(ctx, "ts_mir_load_captured_var", (void *)ts_mir_load_captured_var);
  /*  direct math functions */
  MIR_load_external(ctx, "sin", (void *)sin);
  MIR_load_external(ctx, "cos", (void *)cos);
  MIR_load_external(ctx, "sqrt", (void *)sqrt);
  MIR_load_external(ctx, "fabs", (void *)fabs);
  MIR_load_external(ctx, "floor", (void *)floor);
  MIR_load_external(ctx, "ceil", (void *)ceil);
  MIR_load_external(ctx, "log", (void *)log);
  MIR_load_external(ctx, "exp", (void *)exp);
  MIR_load_external(ctx, "round", (void *)round);
  MIR_load_external(ctx, "tan", (void *)tan);
  MIR_load_external(ctx, "asin", (void *)asin);
  MIR_load_external(ctx, "acos", (void *)acos);
  MIR_load_external(ctx, "atan", (void *)atan);
  MIR_load_external(ctx, "fmax", (void *)fmax);
  MIR_load_external(ctx, "fmin", (void *)fmin);
  MIR_load_external(ctx, "atan2", (void *)atan2);
  /*  direct dispatch */
  MIR_load_external(ctx, "ts_mir_call_native", (void *)ts_mir_call_native);
  MIR_load_external(ctx, "ts_mir_call_host_slot", (void *)ts_mir_call_host_slot);
  MIR_load_external(ctx, "ts_mir_call_builtin", (void *)ts_mir_call_builtin);
  /*  OOP runtime calls */
  MIR_load_external(ctx, "ts_mir_oop_define_class", (void *)ts_mir_oop_define_class);
  MIR_load_external(ctx, "ts_mir_oop_class_alias", (void *)ts_mir_oop_class_alias);
  MIR_load_external(ctx, "ts_mir_oop_new_assign", (void *)ts_mir_oop_new_assign);
  MIR_load_external(ctx, "ts_mir_oop_member_call", (void *)ts_mir_oop_member_call);
  MIR_load_external(ctx, "ts_mir_oop_member_call_cached", (void *)ts_mir_oop_member_call_cached);
  MIR_load_external(ctx, "ts_mir_oop_member_call_assign", (void *)ts_mir_oop_member_call_assign);
  MIR_load_external(ctx, "ts_mir_oop_member_call_value", (void *)ts_mir_oop_member_call_value);
  MIR_load_external(ctx, "ts_mir_oop_member_call_mono_value",
                    (void *)ts_mir_oop_member_call_mono_value);
  MIR_load_external(ctx, "ts_mir_oop_member_call_assign_value",
                    (void *)ts_mir_oop_member_call_assign_value);
  MIR_load_external(ctx, "ts_mir_oop_member_get", (void *)ts_mir_oop_member_get);
  MIR_load_external(ctx, "ts_mir_oop_member_set", (void *)ts_mir_oop_member_set);
  MIR_load_external(ctx, "ts_mir_oop_num_ptr", (void *)ts_mir_oop_num_ptr);
  MIR_load_external(ctx, "ts_mir_oop_instanceof", (void *)ts_mir_oop_instanceof);
  MIR_load_external(ctx, "ts_mir_oop_predicate", (void *)ts_mir_oop_predicate);

}
