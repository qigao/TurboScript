#include "exprtk.h"
#include "exprtk_class.h"
#include "exprtk_module.h"
#include "turbo_script.h"
#include "turbo_script_internal.h"
#include "turbo_script_closure_analysis.h"

#include "exprtk_grammar.h"
#include <math.h>
#include <mir-gen.h>
#include <mir.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

exprtk_env_t *exprtk_env_snapshot(exprtk_env_t *env);
exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value, exprtk_env_t *dst_env);
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

/* =========================================================================
 * 计时辅助函数（跨平台）
 * ========================================================================= */

static uint64_t ts_get_time_us(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000000ULL) / freq.QuadPart);
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

/* =========================================================================
 * Phase 1-5: Extended MIR JIT Compiler for TurboScript
 * ========================================================================= */

/* --- Loop stack for break/continue (Phase 1) --- */
#define MAX_LOOP_DEPTH 32

typedef struct {
  MIR_label_t break_label;
  MIR_label_t continue_label;
} loop_frame_t;

/* --- External call items cache (Phase 2) --- */
typedef struct {
  MIR_item_t fmod_proto;
  MIR_item_t fmod_import;
  MIR_item_t pow_proto;
  MIR_item_t pow_import;
  /*  variable bridge */
  MIR_item_t load_var_proto;
  MIR_item_t load_var_import;
  MIR_item_t store_var_proto;
  MIR_item_t store_var_import;
  MIR_item_t assign_var_proto;
  MIR_item_t assign_var_import;
  /*  function call bridges */
  MIR_item_t call0_proto;
  MIR_item_t call0_import;
  MIR_item_t call1_proto;
  MIR_item_t call1_import;
  MIR_item_t call2_proto;
  MIR_item_t call2_import;
  MIR_item_t call3_proto;
  MIR_item_t call3_import;
  MIR_item_t calln_proto;
  MIR_item_t calln_import;
  MIR_item_t call_assign_proto;
  MIR_item_t call_assign_import;
  MIR_item_t call_value_assign_proto;
  MIR_item_t call_value_assign_import;
  MIR_item_t value_expr_proto;
  MIR_item_t value_expr_import;
  MIR_item_t value_expr_assign_proto;
  MIR_item_t value_expr_assign_import;
  MIR_item_t await_value_proto;
  MIR_item_t await_value_import;
  MIR_item_t await_assign_proto;
  MIR_item_t await_assign_import;
  MIR_item_t function_expr_assign_proto;
  MIR_item_t function_expr_assign_import;
  MIR_item_t try_catch_assign_proto;
  MIR_item_t try_catch_assign_import;
  /*  vector indexing */
  MIR_item_t vec_get_proto;
  MIR_item_t vec_get_import;
  MIR_item_t vec_assign_proto;
  MIR_item_t vec_assign_import;
  MIR_item_t destruct_var_proto;
  MIR_item_t destruct_var_import;
  MIR_item_t destruct_vector_proto;
  MIR_item_t destruct_vector_import;
  MIR_item_t string_assign_proto;
  MIR_item_t string_assign_import;
  MIR_item_t template_assign_proto;
  MIR_item_t template_assign_import;
  /*  function definition */
  MIR_item_t define_func_proto;
  MIR_item_t define_func_import;
  /*  complex value/node helper */
  /*  member access */
  MIR_item_t member_get_proto;
  MIR_item_t member_get_import;
  MIR_item_t member_get_assign_proto;
  MIR_item_t member_get_assign_import;
  /*  vector data pointer */
  MIR_item_t vec_data_proto;
  MIR_item_t vec_data_import;
  /*  map access by key */
  MIR_item_t map_get_key_proto;
  MIR_item_t map_get_key_import;
  MIR_item_t map_assign_proto;
  MIR_item_t map_assign_import;
  MIR_item_t map_value_assign_proto;
  MIR_item_t map_value_assign_import;
  MIR_item_t map_rest_assign_proto;
  MIR_item_t map_rest_assign_import;
  /*  map field pointer (for native direct load) */
  MIR_item_t map_num_ptr_proto;
  MIR_item_t map_num_ptr_import;
  /*  closure support */
  MIR_item_t load_captured_proto;
  MIR_item_t load_captured_import;
  /*  direct math function imports (bypass call bridge) */
  MIR_item_t sin_proto, sin_import;
  MIR_item_t cos_proto, cos_import;
  MIR_item_t sqrt_proto, sqrt_import;
  MIR_item_t fabs_proto, fabs_import;
  MIR_item_t floor_proto, floor_import;
  MIR_item_t ceil_proto, ceil_import;
  MIR_item_t log_proto, log_import;
  MIR_item_t exp_proto, exp_import;
  MIR_item_t round_proto, round_import;
  MIR_item_t tan_proto, tan_import;
  MIR_item_t asin_proto, asin_import;
  MIR_item_t acos_proto, acos_import;
  MIR_item_t atan_proto, atan_import;
  MIR_item_t fmax_proto, fmax_import;
  MIR_item_t fmin_proto, fmin_import;
  MIR_item_t atan2_proto, atan2_import;
  /*  direct native/builtin function dispatch */
  MIR_item_t call_native_proto, call_native_import;
  MIR_item_t call_builtin_proto, call_builtin_import;
  /*  OOP runtime calls */
  MIR_item_t oop_define_class_proto, oop_define_class_import;
  MIR_item_t oop_alias_class_proto, oop_alias_class_import;
  MIR_item_t oop_new_assign_proto, oop_new_assign_import;
  MIR_item_t oop_member_call_proto, oop_member_call_import;
  MIR_item_t oop_member_call_cached_proto, oop_member_call_cached_import;
  MIR_item_t oop_member_call_assign_proto, oop_member_call_assign_import;
  MIR_item_t oop_member_call_value_proto, oop_member_call_value_import;
  MIR_item_t oop_member_call_mono_value_proto, oop_member_call_mono_value_import;
  MIR_item_t oop_member_call_assign_value_proto, oop_member_call_assign_value_import;
  MIR_item_t oop_member_get_proto, oop_member_get_import;
  MIR_item_t oop_member_set_proto, oop_member_set_import;
  MIR_item_t oop_num_ptr_proto, oop_num_ptr_import;
  MIR_item_t oop_instanceof_proto, oop_instanceof_import;
  MIR_item_t oop_predicate_proto, oop_predicate_import;
} ts_mir_externals_t;

static double ts_mir_numeric_value(exprtk_value_t val) {
  if (val.type == EXPRTK_VAL_INTEGER) return (double)val.data.integer;
  if (val.type == EXPRTK_VAL_NUMBER) return val.data.number;
  if (val.type == EXPRTK_VAL_BOOL) return val.data.boolean ? 1.0 : 0.0;
  return 0.0;
}

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

typedef struct {
  char *name;
  MIR_item_t mir_func;
  MIR_item_t proto;
  size_t arg_count;
  int has_closure_env;
} ts_compiled_func_t;

typedef struct {
  const char *param_name;
  const char *target_name;
} ts_mir_func_alias_t;

typedef struct {
  char *name;
  MIR_reg_t reg;
  int dirty;  // 脏标记：1 表示变量已修改，需要同步
  int dynamic_value;
} ts_mir_var_entry_t;

typedef struct {
  const char *name;
  MIR_reg_t ptr_reg;
} ts_mir_vec_ptr_entry_t;

typedef struct {
  const char *obj_name;
  const char *key_name;
  MIR_reg_t ptr_reg;
} ts_mir_map_ptr_entry_t;

typedef struct {
  const char *obj_name;
  const char *member_name;
  MIR_reg_t ptr_reg;
  void *cache;
} ts_mir_oop_ptr_entry_t;

typedef struct {
  exprtk_instance_t *instance;
  size_t index;
} ts_mir_oop_slot_cache_t;

typedef struct {
  char *var_name;
  char *class_name;
} ts_mir_class_type_entry_t;

typedef struct {
  MIR_item_t func;
  ts_mir_var_entry_t **vars;
  int var_count;
  int var_capacity;
  int tmp_count;
  int loop_depth;
  MIR_reg_t ctx_reg;
  MIR_reg_t closure_env_reg;
  ts_mir_func_alias_t *func_aliases;
  size_t func_alias_count;
  int vec_ptr_count;
  int map_ptr_count;
  int oop_ptr_count;
  char **class_names;
  int class_name_count;
  int class_name_capacity;
  ts_mir_class_type_entry_t *class_types;
  int class_type_count;
  int class_type_capacity;
} ts_mir_compile_frame_t;

typedef struct {
  MIR_context_t ctx;
  MIR_item_t func;
  MIR_module_t module;
  turbo_script_ctx_t *ts_ctx;
  exprtk_node_t *ast_root;
  char item_prefix[64];

  ts_mir_var_entry_t **vars;
  int var_count;
  int var_capacity;
  int tmp_count;
  int failed;

  /*  loop stack for break/continue */
  loop_frame_t loop_stack[MAX_LOOP_DEPTH];
  int loop_depth;

  /*  external call items */
  ts_mir_externals_t ext;

  /*  ctx_ptr register (first function argument) */
  MIR_reg_t ctx_reg;

  /* Compile-time function-parameter aliases for monomorphic higher-order calls. */
  ts_mir_func_alias_t *func_aliases;
  size_t func_alias_count;

  /*  compiled script functions */
  ts_compiled_func_t *compiled_funcs;
  int compiled_func_count;
  int compiled_func_capacity;

  /*  cached vector data pointers for native indexing */
  ts_mir_vec_ptr_entry_t *vec_ptrs;
  int vec_ptr_count;
  int vec_ptr_capacity;

  /*  cached map field pointers for native access */
  ts_mir_map_ptr_entry_t *map_ptrs;
  int map_ptr_count;
  int map_ptr_capacity;

  /* cached public numeric OOP field pointers for native access */
  ts_mir_oop_ptr_entry_t *oop_ptrs;
  int oop_ptr_count;
  int oop_ptr_capacity;

  /* 闭包支持 */
  void *closure_analysis;     // ts_closure_analysis_t* - 闭包分析结果
  MIR_reg_t closure_env_reg;  // 闭包环境寄存器（函数参数）

  /* OOP class names defined in the current script, used for Class(...) lowering. */
  char **class_names;
  int class_name_count;
  int class_name_capacity;

  /* Exact variable class tracking for monomorphic OOP helper calls. */
  ts_mir_class_type_entry_t *class_types;
  int class_type_count;
  int class_type_capacity;
} ts_mir_compiler_t;

static void ts_mir_promote_env_error(turbo_script_ctx_t *ctx);
static void ts_mir_value_arg_error(exprtk_env_t *env, exprtk_node_t *node);
static int ts_mir_runtime_value_arg(exprtk_node_t *node, exprtk_env_t *env,
                                 exprtk_value_t *out);

static void ts_mir_fail(ts_mir_compiler_t *c, const char *fmt, ...) {
  va_list args;

  if (!c || c->failed)
    return;

  c->failed = 1;
  if (!c->ts_ctx)
    return;

  c->ts_ctx->error_code = TURBO_SCRIPT_ERROR_JIT;
  va_start(args, fmt);
  vsnprintf(c->ts_ctx->error_msg, sizeof(c->ts_ctx->error_msg), fmt, args);
  va_end(args);
}

static MIR_reg_t new_temp_reg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_t%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_D, name);
}

static MIR_reg_t new_temp_ireg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_i%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_I64, name);
}

static MIR_reg_t new_temp_preg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_p%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_I64, name);
}

static MIR_reg_t ts_emit_zero_reg(ts_mir_compiler_t *c) {
  MIR_reg_t r = new_temp_reg(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r), MIR_new_double_op(c->ctx, 0.0)));
  return r;
}

static int ts_mir_ensure_var_capacity(ts_mir_compiler_t *c, int needed) {
  ts_mir_var_entry_t **new_vars = NULL;
  int new_capacity = 0;

  if (needed <= c->var_capacity)
    return 1;

  new_capacity = c->var_capacity > 0 ? c->var_capacity * 2 : 32;
  if (new_capacity < needed)
    new_capacity = needed;

  new_vars = (ts_mir_var_entry_t **)realloc(c->vars, (size_t)new_capacity * sizeof(*new_vars));
  if (!new_vars) {
    ts_mir_fail(c, "JIT compile error: out of memory growing variable table");
    return 0;
  }

  c->vars = new_vars;
  c->var_capacity = new_capacity;
  return 1;
}

static int ts_mir_ensure_compiled_func_capacity(ts_mir_compiler_t *c, int needed) {
  ts_compiled_func_t *new_entries = NULL;
  int new_capacity = 0;

  if (needed <= c->compiled_func_capacity)
    return 1;

  new_capacity = c->compiled_func_capacity > 0 ? c->compiled_func_capacity * 2 : 16;
  if (new_capacity < needed)
    new_capacity = needed;

  new_entries = (ts_compiled_func_t *)realloc(c->compiled_funcs,
                                              (size_t)new_capacity * sizeof(*new_entries));
  if (!new_entries) {
    ts_mir_fail(c, "JIT compile error: out of memory growing compiled function table");
    return 0;
  }

  c->compiled_funcs = new_entries;
  c->compiled_func_capacity = new_capacity;
  return 1;
}

static int ts_mir_ensure_vec_ptr_capacity(ts_mir_compiler_t *c, int needed) {
  ts_mir_vec_ptr_entry_t *new_entries = NULL;
  int new_capacity = 0;

  if (needed <= c->vec_ptr_capacity)
    return 1;

  new_capacity = c->vec_ptr_capacity > 0 ? c->vec_ptr_capacity * 2 : 16;
  if (new_capacity < needed)
    new_capacity = needed;

  new_entries =
      (ts_mir_vec_ptr_entry_t *)realloc(c->vec_ptrs, (size_t)new_capacity * sizeof(*new_entries));
  if (!new_entries) {
    ts_mir_fail(c, "JIT compile error: out of memory growing vector pointer cache");
    return 0;
  }

  c->vec_ptrs = new_entries;
  c->vec_ptr_capacity = new_capacity;
  return 1;
}

static int ts_mir_ensure_map_ptr_capacity(ts_mir_compiler_t *c, int needed) {
  ts_mir_map_ptr_entry_t *new_entries = NULL;
  int new_capacity = 0;

  if (needed <= c->map_ptr_capacity)
    return 1;

  new_capacity = c->map_ptr_capacity > 0 ? c->map_ptr_capacity * 2 : 16;
  if (new_capacity < needed)
    new_capacity = needed;

  new_entries =
      (ts_mir_map_ptr_entry_t *)realloc(c->map_ptrs, (size_t)new_capacity * sizeof(*new_entries));
  if (!new_entries) {
    ts_mir_fail(c, "JIT compile error: out of memory growing map pointer cache");
    return 0;
  }

  c->map_ptrs = new_entries;
  c->map_ptr_capacity = new_capacity;
  return 1;
}

static int ts_mir_ensure_oop_ptr_capacity(ts_mir_compiler_t *c, int needed) {
  ts_mir_oop_ptr_entry_t *new_entries = NULL;
  int new_capacity = 0;

  if (needed <= c->oop_ptr_capacity)
    return 1;

  new_capacity = c->oop_ptr_capacity > 0 ? c->oop_ptr_capacity * 2 : 16;
  if (new_capacity < needed)
    new_capacity = needed;

  new_entries =
      (ts_mir_oop_ptr_entry_t *)realloc(c->oop_ptrs, (size_t)new_capacity * sizeof(*new_entries));
  if (!new_entries) {
    ts_mir_fail(c, "JIT compile error: out of memory growing OOP pointer cache");
    return 0;
  }

  c->oop_ptrs = new_entries;
  c->oop_ptr_capacity = new_capacity;
  return 1;
}

static MIR_reg_t get_or_create_reg(ts_mir_compiler_t *c, const char *name) {
  ts_mir_var_entry_t *entry = NULL;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) return c->vars[i]->reg;
  }

  if (!ts_mir_ensure_var_capacity(c, c->var_count + 1))
    return new_temp_reg(c);

  entry = (ts_mir_var_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating variable entry for '%s'",
                name ? name : "<unnamed>");
    return new_temp_reg(c);
  }

  entry->name = strdup(name);
  if (!entry->name) {
    free(entry);
    ts_mir_fail(c, "JIT compile error: out of memory duplicating variable name '%s'",
                name ? name : "<unnamed>");
    return new_temp_reg(c);
  }
  entry->reg = MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_D, name);
  entry->dirty = 0;  // 初始化为未脏
  entry->dynamic_value = 0;
  c->vars[c->var_count++] = entry;
  return entry->reg;
}

static void ts_mir_mark_var_dirty(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;
  
  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dirty = 1;
      return;
    }
  }
}

static void ts_mir_mark_var_clean(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dirty = 0;
      return;
    }
  }
}

static void ts_mir_mark_all_vars_dirty(ts_mir_compiler_t *c) {
  if (!c) return;
  
  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    c->vars[i]->dirty = 1;
  }
}

static void ts_mir_clear_dirty_flags(ts_mir_compiler_t *c) {
  if (!c) return;
  
  for (int i = 0; i < c->var_count; i++) {
    c->vars[i]->dirty = 0;
  }
}

static void ts_mir_mark_var_dynamic(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dynamic_value = 1;
      return;
    }
  }
}

static void ts_mir_mark_var_numeric(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dynamic_value = 0;
      return;
    }
  }
}

static int ts_mir_var_is_dynamic(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) return c->vars[i]->dynamic_value;
  }
  return 0;
}

static void ts_mir_discard_current_vars(ts_mir_compiler_t *c) {
  if (!c)
    return;
  for (int i = 0; i < c->var_count; ++i) {
    free(c->vars[i]);
  }
  free(c->vars);
  c->vars = NULL;
  c->var_count = 0;
  c->var_capacity = 0;
}

static void ts_mir_destroy_compiled_funcs(ts_mir_compiler_t *c) {
  if (!c)
    return;

  for (int i = 0; i < c->compiled_func_count; ++i) {
    free(c->compiled_funcs[i].name);
    c->compiled_funcs[i].name = NULL;
  }

  free(c->compiled_funcs);
  c->compiled_funcs = NULL;
  c->compiled_func_count = 0;
  c->compiled_func_capacity = 0;
}

static void ts_mir_destroy_pointer_caches(ts_mir_compiler_t *c) {
  if (!c)
    return;

  free(c->vec_ptrs);
  c->vec_ptrs = NULL;
  c->vec_ptr_count = 0;
  c->vec_ptr_capacity = 0;

  free(c->map_ptrs);
  c->map_ptrs = NULL;
  c->map_ptr_count = 0;
  c->map_ptr_capacity = 0;

  /* Slot-cache objects are embedded as MIR immediates and live with the MIR context. */
  free(c->oop_ptrs);
  c->oop_ptrs = NULL;
  c->oop_ptr_count = 0;
  c->oop_ptr_capacity = 0;
}

static int ts_mir_add_class_name(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  for (int i = 0; i < c->class_name_count; ++i) {
    if (strcmp(c->class_names[i], name) == 0) return 1;
  }

  if (c->class_name_count >= c->class_name_capacity) {
    int new_capacity = c->class_name_capacity > 0 ? c->class_name_capacity * 2 : 16;
    char **new_names = (char **)realloc(c->class_names, (size_t)new_capacity * sizeof(*new_names));
    if (!new_names) {
      ts_mir_fail(c, "JIT compile error: out of memory growing OOP class-name list");
      return 0;
    }
    c->class_names = new_names;
    c->class_name_capacity = new_capacity;
  }

  c->class_names[c->class_name_count] = strdup(name);
  if (!c->class_names[c->class_name_count]) {
    ts_mir_fail(c, "JIT compile error: out of memory duplicating class name '%s'", name);
    return 0;
  }
  c->class_name_count++;
  return 1;
}

static int ts_mir_is_known_class_name(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  for (int i = 0; i < c->class_name_count; ++i) {
    if (strcmp(c->class_names[i], name) == 0) return 1;
  }
  return 0;
}

static void ts_mir_destroy_class_names(ts_mir_compiler_t *c) {
  if (!c) return;
  for (int i = 0; i < c->class_name_count; ++i) free(c->class_names[i]);
  free(c->class_names);
  c->class_names = NULL;
  c->class_name_count = 0;
  c->class_name_capacity = 0;
}

static void ts_mir_discard_class_names(ts_mir_compiler_t *c) {
  ts_mir_destroy_class_names(c);
}

static void ts_mir_discard_class_types(ts_mir_compiler_t *c) {
  if (!c) return;
  /* Strings can be baked into MIR immediates for monomorphic OOP calls.
   * Match the existing variable-name policy: keep those allocations alive
   * for the MIR context lifetime and release only the tracking array. */
  free(c->class_types);
  c->class_types = NULL;
  c->class_type_count = 0;
  c->class_type_capacity = 0;
}

static int ts_mir_set_var_class(ts_mir_compiler_t *c, const char *var_name,
                                const char *class_name) {
  if (!c || !var_name || !class_name) return 0;

  for (int i = 0; i < c->class_type_count; ++i) {
    if (strcmp(c->class_types[i].var_name, var_name) == 0) {
      char *copy = strdup(class_name);
      if (!copy) {
        ts_mir_fail(c, "JIT compile error: out of memory duplicating class name '%s'",
                    class_name);
        return 0;
      }
      /* Do not free the old class string: it may already be referenced by MIR. */
      c->class_types[i].class_name = copy;
      return 1;
    }
  }

  if (c->class_type_count >= c->class_type_capacity) {
    int new_capacity = c->class_type_capacity > 0 ? c->class_type_capacity * 2 : 16;
    ts_mir_class_type_entry_t *new_entries =
        (ts_mir_class_type_entry_t *)realloc(c->class_types,
                                             (size_t)new_capacity * sizeof(*new_entries));
    if (!new_entries) {
      ts_mir_fail(c, "JIT compile error: out of memory growing OOP type table");
      return 0;
    }
    c->class_types = new_entries;
    c->class_type_capacity = new_capacity;
  }

  c->class_types[c->class_type_count].var_name = strdup(var_name);
  c->class_types[c->class_type_count].class_name = strdup(class_name);
  if (!c->class_types[c->class_type_count].var_name ||
      !c->class_types[c->class_type_count].class_name) {
    free(c->class_types[c->class_type_count].var_name);
    free(c->class_types[c->class_type_count].class_name);
    c->class_types[c->class_type_count].var_name = NULL;
    c->class_types[c->class_type_count].class_name = NULL;
    ts_mir_fail(c, "JIT compile error: out of memory tracking OOP type for '%s'", var_name);
    return 0;
  }
  c->class_type_count++;
  return 1;
}

static void ts_mir_clear_var_class(ts_mir_compiler_t *c, const char *var_name) {
  if (!c || !var_name) return;
  for (int i = 0; i < c->class_type_count; ++i) {
    if (strcmp(c->class_types[i].var_name, var_name) == 0) {
      /* Do not free strings: they may already be referenced by emitted MIR. */
      if (i + 1 < c->class_type_count) {
        memmove(&c->class_types[i], &c->class_types[i + 1],
                (size_t)(c->class_type_count - i - 1) * sizeof(c->class_types[0]));
      }
      c->class_type_count--;
      return;
    }
  }
}

static const char *ts_mir_find_var_class(ts_mir_compiler_t *c, const char *var_name) {
  if (!c || !var_name) return NULL;
  for (int i = 0; i < c->class_type_count; ++i) {
    if (strcmp(c->class_types[i].var_name, var_name) == 0) return c->class_types[i].class_name;
  }
  return NULL;
}

static const char *ts_mir_new_hidden_receiver_name(ts_mir_compiler_t *c) {
  char name[64];
  char *copy = NULL;
  if (!c) return NULL;

  snprintf(name, sizeof(name), "$ts_oop_recv_%d", c->tmp_count++);
  copy = strdup(name);
  if (!copy) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating hidden OOP receiver");
    return NULL;
  }

  /* Names are embedded as MIR immediates and must live for the MIR context lifetime. */
  return copy;
}

static void ts_mir_destroy_compiler_storage(ts_mir_compiler_t *c) {
  if (!c)
    return;

  ts_mir_discard_current_vars(c);
  ts_mir_destroy_compiled_funcs(c);
  ts_mir_destroy_pointer_caches(c);
  ts_mir_destroy_class_names(c);
  ts_mir_discard_class_types(c);
}

static ts_mir_compile_frame_t ts_mir_capture_frame(const ts_mir_compiler_t *c) {
  ts_mir_compile_frame_t frame = {0};

  if (!c)
    return frame;

  frame.func = c->func;
  frame.vars = c->vars;
  frame.var_count = c->var_count;
  frame.var_capacity = c->var_capacity;
  frame.tmp_count = c->tmp_count;
  frame.loop_depth = c->loop_depth;
  frame.ctx_reg = c->ctx_reg;
  frame.closure_env_reg = c->closure_env_reg;
  frame.func_aliases = c->func_aliases;
  frame.func_alias_count = c->func_alias_count;
  frame.vec_ptr_count = c->vec_ptr_count;
  frame.map_ptr_count = c->map_ptr_count;
  frame.oop_ptr_count = c->oop_ptr_count;
  frame.class_names = c->class_names;
  frame.class_name_count = c->class_name_count;
  frame.class_name_capacity = c->class_name_capacity;
  frame.class_types = c->class_types;
  frame.class_type_count = c->class_type_count;
  frame.class_type_capacity = c->class_type_capacity;
  return frame;
}

static void ts_mir_begin_isolated_compile(ts_mir_compiler_t *c) {
  if (!c)
    return;

  c->vars = NULL;
  c->var_count = 0;
  c->var_capacity = 0;
  c->tmp_count = 0;
  c->loop_depth = 0;
  c->ctx_reg = 0;
  c->closure_env_reg = 0;
  c->func_aliases = NULL;
  c->func_alias_count = 0;
  c->vec_ptr_count = 0;
  c->map_ptr_count = 0;
  c->oop_ptr_count = 0;
  c->class_names = NULL;
  c->class_name_count = 0;
  c->class_name_capacity = 0;
  c->class_types = NULL;
  c->class_type_count = 0;
  c->class_type_capacity = 0;
}

static void ts_mir_restore_frame(ts_mir_compiler_t *c, const ts_mir_compile_frame_t *frame) {
  if (!c || !frame)
    return;

  ts_mir_discard_current_vars(c);
  ts_mir_discard_class_names(c);
  ts_mir_discard_class_types(c);
  c->func = frame->func;
  c->vars = frame->vars;
  c->var_count = frame->var_count;
  c->var_capacity = frame->var_capacity;
  c->tmp_count = frame->tmp_count;
  c->loop_depth = frame->loop_depth;
  c->ctx_reg = frame->ctx_reg;
  c->closure_env_reg = frame->closure_env_reg;
  c->func_aliases = frame->func_aliases;
  c->func_alias_count = frame->func_alias_count;
  c->vec_ptr_count = frame->vec_ptr_count;
  c->map_ptr_count = frame->map_ptr_count;
  c->oop_ptr_count = frame->oop_ptr_count;
  c->class_names = frame->class_names;
  c->class_name_count = frame->class_name_count;
  c->class_name_capacity = frame->class_name_capacity;
  c->class_types = frame->class_types;
  c->class_type_count = frame->class_type_count;
  c->class_type_capacity = frame->class_type_capacity;
}

static MIR_reg_t ts_mir_get_or_add_vec_ptr(ts_mir_compiler_t *c, const char *name) {
  MIR_reg_t ptr_reg = 0;

  for (int i = 0; i < c->vec_ptr_count; i++) {
    if (strcmp(c->vec_ptrs[i].name, name) == 0)
      return c->vec_ptrs[i].ptr_reg;
  }

  if (!ts_mir_ensure_vec_ptr_capacity(c, c->vec_ptr_count + 1))
    return 0;

  ptr_reg = new_temp_ireg(c);
  c->vec_ptrs[c->vec_ptr_count].name = name;
  c->vec_ptrs[c->vec_ptr_count].ptr_reg = ptr_reg;
  c->vec_ptr_count++;
  return ptr_reg;
}

static MIR_reg_t ts_mir_get_or_add_map_ptr(ts_mir_compiler_t *c, const char *obj_name,
                                           const char *key_name) {
  MIR_reg_t ptr_reg = 0;

  for (int i = 0; i < c->map_ptr_count; i++) {
    if (c->map_ptrs[i].obj_name == obj_name && strcmp(c->map_ptrs[i].key_name, key_name) == 0)
      return c->map_ptrs[i].ptr_reg;
  }

  if (!ts_mir_ensure_map_ptr_capacity(c, c->map_ptr_count + 1))
    return 0;

  ptr_reg = new_temp_ireg(c);
  c->map_ptrs[c->map_ptr_count].obj_name = obj_name;
  c->map_ptrs[c->map_ptr_count].key_name = key_name;
  c->map_ptrs[c->map_ptr_count].ptr_reg = ptr_reg;
  c->map_ptr_count++;
  return ptr_reg;
}

static MIR_reg_t ts_mir_get_or_add_oop_ptr(ts_mir_compiler_t *c, const char *obj_name,
                                           const char *member_name) {
  MIR_reg_t ptr_reg = 0;

  for (int i = 0; i < c->oop_ptr_count; i++) {
    if (strcmp(c->oop_ptrs[i].obj_name, obj_name) == 0 &&
        strcmp(c->oop_ptrs[i].member_name, member_name) == 0) {
      return c->oop_ptrs[i].ptr_reg;
    }
  }

  if (!ts_mir_ensure_oop_ptr_capacity(c, c->oop_ptr_count + 1))
    return 0;

  ptr_reg = new_temp_preg(c);
  void *cache = calloc(1, sizeof(ts_mir_oop_slot_cache_t));
  if (!cache) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating OOP slot cache");
    return 0;
  }
  c->oop_ptrs[c->oop_ptr_count].obj_name = obj_name;
  c->oop_ptrs[c->oop_ptr_count].member_name = member_name;
  c->oop_ptrs[c->oop_ptr_count].ptr_reg = ptr_reg;
  c->oop_ptrs[c->oop_ptr_count].cache = cache;
  c->oop_ptr_count++;

  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, ptr_reg),
                               MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache)));
  return ptr_reg;
}

static int ts_mir_bind_existing_reg(ts_mir_compiler_t *c, const char *name, MIR_reg_t reg) {
  ts_mir_var_entry_t *entry = NULL;

  if (!ts_mir_ensure_var_capacity(c, c->var_count + 1))
    return 0;

  entry = (ts_mir_var_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating parameter entry for '%s'",
                name ? name : "<unnamed>");
    return 0;
  }

  entry->name = strdup(name);
  if (!entry->name) {
    free(entry);
    ts_mir_fail(c, "JIT compile error: out of memory duplicating parameter name '%s'",
                name ? name : "<unnamed>");
    return 0;
  }

  entry->reg = reg;
  entry->dirty = 0;
  entry->dynamic_value = 0;
  c->vars[c->var_count++] = entry;
  return 1;
}

/* Forward declarations */
static MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node);
static void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node);
static void ts_compile_branch_false(ts_mir_compiler_t *c, exprtk_node_t *node,
                                    MIR_label_t false_label);
static void ts_compile_branch_true(ts_mir_compiler_t *c, exprtk_node_t *node,
                                   MIR_label_t true_label);
static MIR_reg_t ts_emit_packed_args(ts_mir_compiler_t *c, size_t argc, const MIR_reg_t *arg_regs);
static int ts_emit_direct_math_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                    const MIR_reg_t *arg_regs, MIR_reg_t res);
static int ts_emit_direct_resolved_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                        const MIR_reg_t *arg_regs, MIR_reg_t res);
static void ts_emit_runtime_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                 const MIR_reg_t *arg_regs, MIR_reg_t res);
static MIR_reg_t ts_emit_member_access(ts_mir_compiler_t *c, const char *obj_name,
                                       const char *member, exprtk_node_t *object_node);
static MIR_reg_t ts_emit_member_access_assign(ts_mir_compiler_t *c, const char *target_name,
                                              const char *obj_name, const char *member,
                                              exprtk_node_t *object_node);
static MIR_reg_t ts_emit_oop_member_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                                const char *object_name, const char *method_name,
                                                exprtk_node_t *object_node, size_t argc,
                                                const MIR_reg_t *arg_regs);
static MIR_reg_t ts_emit_oop_member_call_assign_value(ts_mir_compiler_t *c,
                                                      const char *target_name,
                                                      const char *object_name,
                                                      const char *method_name,
                                                      exprtk_node_t *object_node,
                                                      exprtk_node_t *call_node);
static MIR_reg_t ts_try_emit_oop_member_get_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 exprtk_node_t *object_node);
static MIR_reg_t ts_try_emit_oop_member_set_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 exprtk_node_t *object_node,
                                                 MIR_reg_t value_reg);
static void ts_emit_sync_var_to_env(ts_mir_compiler_t *c, const char *name);
static void ts_emit_sync_to_env(ts_mir_compiler_t *c);
static void ts_emit_reload_from_env(ts_mir_compiler_t *c);
static MIR_reg_t ts_emit_unsupported_node(ts_mir_compiler_t *c, exprtk_node_t *node);
static MIR_reg_t ts_emit_runtime_value_node(ts_mir_compiler_t *c, exprtk_node_t *node);

typedef struct {
  const char *name;
  size_t proto_offset;
  size_t import_offset;
} ts_math_dispatch_entry_t;

static int ts_math_dispatch_cmp(const void *key, const void *entry) {
  return strcmp((const char *)key, ((const ts_math_dispatch_entry_t *)entry)->name);
}

static int ts_find_math_dispatch(ts_mir_compiler_t *c, const char *name, size_t argc,
                                 MIR_item_t *proto, MIR_item_t *import) {
  static const ts_math_dispatch_entry_t unary_table[] = {
      {"abs", offsetof(ts_mir_externals_t, fabs_proto), offsetof(ts_mir_externals_t, fabs_import)},
      {"acos", offsetof(ts_mir_externals_t, acos_proto),
       offsetof(ts_mir_externals_t, acos_import)},
      {"asin", offsetof(ts_mir_externals_t, asin_proto),
       offsetof(ts_mir_externals_t, asin_import)},
      {"atan", offsetof(ts_mir_externals_t, atan_proto),
       offsetof(ts_mir_externals_t, atan_import)},
      {"ceil", offsetof(ts_mir_externals_t, ceil_proto),
       offsetof(ts_mir_externals_t, ceil_import)},
      {"cos", offsetof(ts_mir_externals_t, cos_proto), offsetof(ts_mir_externals_t, cos_import)},
      {"exp", offsetof(ts_mir_externals_t, exp_proto), offsetof(ts_mir_externals_t, exp_import)},
      {"floor", offsetof(ts_mir_externals_t, floor_proto),
       offsetof(ts_mir_externals_t, floor_import)},
      {"log", offsetof(ts_mir_externals_t, log_proto), offsetof(ts_mir_externals_t, log_import)},
      {"round", offsetof(ts_mir_externals_t, round_proto),
       offsetof(ts_mir_externals_t, round_import)},
      {"sin", offsetof(ts_mir_externals_t, sin_proto), offsetof(ts_mir_externals_t, sin_import)},
      {"sqrt", offsetof(ts_mir_externals_t, sqrt_proto),
       offsetof(ts_mir_externals_t, sqrt_import)},
      {"tan", offsetof(ts_mir_externals_t, tan_proto), offsetof(ts_mir_externals_t, tan_import)},
  };
  static const ts_math_dispatch_entry_t binary_table[] = {
      {"atan2", offsetof(ts_mir_externals_t, atan2_proto),
       offsetof(ts_mir_externals_t, atan2_import)},
      {"max", offsetof(ts_mir_externals_t, fmax_proto),
       offsetof(ts_mir_externals_t, fmax_import)},
      {"min", offsetof(ts_mir_externals_t, fmin_proto),
       offsetof(ts_mir_externals_t, fmin_import)},
  };

  const ts_math_dispatch_entry_t *table = NULL;
  size_t table_count = 0;
  const ts_math_dispatch_entry_t *entry = NULL;
  char *ext_base = NULL;

  if (!c || !name || !proto || !import)
    return 0;

  if (argc == 1) {
    table = unary_table;
    table_count = sizeof(unary_table) / sizeof(unary_table[0]);
  } else if (argc == 2) {
    table = binary_table;
    table_count = sizeof(binary_table) / sizeof(binary_table[0]);
  } else {
    return 0;
  }

  entry = (const ts_math_dispatch_entry_t *)bsearch(name, table, table_count, sizeof(table[0]),
                                                    ts_math_dispatch_cmp);
  if (!entry)
    return 0;

  ext_base = (char *)&c->ext;
  *proto = *(MIR_item_t *)(ext_base + entry->proto_offset);
  *import = *(MIR_item_t *)(ext_base + entry->import_offset);
  return 1;
}
static size_t ts_compile_call_args(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args,
                                   MIR_reg_t out_regs[16]);
static ts_compiled_func_t *ts_find_hof_specialized_call(ts_mir_compiler_t *c,
                                                        exprtk_node_t *call_node,
                                                        int numeric_arg_indices[16],
                                                        size_t *numeric_arg_count);
static MIR_reg_t ts_emit_index_access(ts_mir_compiler_t *c, const char *name,
                                      exprtk_node_t *index_node);
static int ts_is_non_numeric_node(exprtk_node_t *node);
static MIR_reg_t ts_emit_assignment(ts_mir_compiler_t *c, exprtk_node_t *node);
static int ts_compile_data_access_and_assignment(ts_mir_compiler_t *c, exprtk_node_t *node,
                                                 MIR_reg_t *out);
static ts_compiled_func_t *ts_find_compiled_func(ts_mir_compiler_t *c, const char *name);
static const char *ts_find_func_alias(ts_mir_compiler_t *c, const char *name);
static int ts_function_call_is_known_class(ts_mir_compiler_t *c, const char *name);
static int ts_expr_contains_value_call(ts_mir_compiler_t *c, exprtk_node_t *node);
static int ts_index_assignment_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node);
static int ts_binary_is_null_eq_compare(exprtk_node_t *node);

/* --- Dispatch helpers: function calls (Phase 13/17 + explicit runtime calls) --- */
static MIR_reg_t ts_emit_packed_args(ts_mir_compiler_t *c, size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t arr_reg = new_temp_preg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_ALLOCA, MIR_new_reg_op(c->ctx, arr_reg),
                               MIR_new_int_op(c->ctx, (int64_t)(argc * sizeof(double)))));
  for (size_t i = 0; i < argc; i++) {
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV,
                     MIR_new_mem_op(c->ctx, MIR_T_D, (int64_t)(i * sizeof(double)), arr_reg, 0, 1),
                     MIR_new_reg_op(c->ctx, arg_regs[i])));
  }
  return arr_reg;
}

static int ts_emit_direct_math_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                    const MIR_reg_t *arg_regs, MIR_reg_t res) {
  MIR_item_t proto = NULL, import = NULL;

  if (argc == 1) {
    if (!ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
    if (!proto) return 0;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 4, MIR_new_ref_op(c->ctx, proto),
                                      MIR_new_ref_op(c->ctx, import), MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, arg_regs[0])));
    return 1;
  }

  if (argc == 2) {
    if (!ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
    if (!proto) return 0;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, proto),
                                      MIR_new_ref_op(c->ctx, import), MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, arg_regs[0]),
                                      MIR_new_reg_op(c->ctx, arg_regs[1])));
    return 1;
  }

  return 0;
}

static int ts_emit_direct_resolved_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                        const MIR_reg_t *arg_regs, MIR_reg_t res) {
  /* 1) env-registered native functions */
  exprtk_func_t *f = c->ts_ctx->env.funcs;
  while (f) {
    if (f->name && strcmp(f->name, name) == 0 && !f->is_script) {
      void *fn_ptr = (void *)f->data.native.fn;
      void *ud = f->data.native.user_data;
      MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(
                          c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call_native_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call_native_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)fn_ptr),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)ud),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
      return 1;
    }
    f = f->next;
  }

  /* 2) module/registry builtins */
  exprtk_builtin_fn bfn = exprtk_find_builtin(name, &c->ts_ctx->env);
  if (bfn) {
    MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.call_builtin_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call_builtin_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)bfn),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
    return 1;
  }

  return 0;
}

static void ts_emit_runtime_call(ts_mir_compiler_t *c, const char *name, size_t argc,
                                 const MIR_reg_t *arg_regs, MIR_reg_t res) {
  // 保守策略：函数调用可能修改任何变量，标记所有变量为脏
  ts_mir_mark_all_vars_dirty(c);
  
  if (argc == 0) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.call0_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.call0_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name)));
  } else if (argc == 1) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.call1_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.call1_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                      MIR_new_reg_op(c->ctx, arg_regs[0])));
  } else if (argc == 2) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(
                        c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.call2_proto),
                        MIR_new_ref_op(c->ctx, c->ext.call2_import), MIR_new_reg_op(c->ctx, res),
                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_reg_op(c->ctx, arg_regs[0]), MIR_new_reg_op(c->ctx, arg_regs[1])));
  } else if (argc == 3) {
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call3_proto),
                          MIR_new_ref_op(c->ctx, c->ext.call3_import), MIR_new_reg_op(c->ctx, res),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                          MIR_new_reg_op(c->ctx, arg_regs[0]), MIR_new_reg_op(c->ctx, arg_regs[1]),
                          MIR_new_reg_op(c->ctx, arg_regs[2])));
  } else {
    MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.calln_proto),
                          MIR_new_ref_op(c->ctx, c->ext.calln_import), MIR_new_reg_op(c->ctx, res),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                          MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  }
}

static int ts_call_assignment_needs_value_bridge(ts_mir_compiler_t *c, const char *name,
                                                 size_t argc) {
  MIR_item_t proto = NULL;
  MIR_item_t import = NULL;
  exprtk_func_t *f = NULL;
  const char *alias_name = NULL;

  if (!c || !name) return 0;
  alias_name = ts_find_func_alias(c, name);
  if (alias_name) name = alias_name;
  if (ts_find_compiled_func(c, name)) return 0;
  if (ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
  if (exprtk_find_builtin(name, &c->ts_ctx->env)) return 0;

  for (f = c->ts_ctx->env.funcs; f; f = f->next) {
    if (f->name && strcmp(f->name, name) == 0 && !f->is_script) return 0;
  }

  return 1;
}

static MIR_reg_t ts_emit_runtime_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                             const char *name, size_t argc,
                                             exprtk_node_t **arg_nodes) {
  MIR_reg_t arg_regs[16];
  argc = ts_compile_call_args(c, argc, arg_nodes, arg_regs);

  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.call_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.call_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  return res;
}

static MIR_reg_t ts_emit_call_value_assign(ts_mir_compiler_t *c, const char *target_name,
                                           const char *name, exprtk_node_t *call_node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 7,
                                    MIR_new_ref_op(c->ctx, c->ext.call_value_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.call_value_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static MIR_reg_t ts_emit_value_expr_assign(ts_mir_compiler_t *c, const char *target_name,
                                           exprtk_node_t *expr_node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)expr_node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static MIR_reg_t ts_emit_value_expr(ts_mir_compiler_t *c, exprtk_node_t *expr_node) {
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 5,
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.value_expr_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)expr_node)));
  ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_await_value(ts_mir_compiler_t *c, exprtk_node_t *arg_node) {
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 5,
                                    MIR_new_ref_op(c->ctx, c->ext.await_value_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.await_value_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)arg_node)));
  ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_await_assign(ts_mir_compiler_t *c, const char *target_name,
                                      exprtk_node_t *arg_node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.await_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.await_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)arg_node)));
  ts_emit_reload_from_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static MIR_reg_t ts_emit_function_expr_assign(ts_mir_compiler_t *c, const char *target_name,
                                              exprtk_node_t *node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.function_expr_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.function_expr_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static MIR_reg_t ts_emit_try_catch_assign(ts_mir_compiler_t *c, const char *target_name,
                                          exprtk_node_t *node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.try_catch_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.try_catch_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static MIR_reg_t ts_emit_dynamic_var_assign(ts_mir_compiler_t *c, const char *target_name,
                                            const char *source_name) {
  MIR_reg_t res = new_temp_reg(c);

  if (!target_name || !source_name) return 0;

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.assign_var_proto),
                        MIR_new_ref_op(c->ctx, c->ext.assign_var_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)source_name)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

static int ts_vector_literal_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_VECTOR) return 1;

  for (size_t i = 0; i < node->data.vector.count; ++i) {
    exprtk_node_t *element = node->data.vector.elements[i];
    if (!element || element->type == EXPRTK_NODE_SPREAD || ts_is_non_numeric_node(element) ||
        ts_expr_contains_value_call(c, element) || ts_index_assignment_needs_value_eval(c, element)) {
      return 1;
    }
  }

  return 0;
}

static MIR_reg_t ts_emit_vector_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                               exprtk_node_t *node) {
  MIR_reg_t element_regs[16];
  MIR_reg_t arr_reg;
  MIR_reg_t res = new_temp_reg(c);
  size_t count;

  if (!target_name || !node || node->type != EXPRTK_NODE_VECTOR ||
      node->data.vector.count > 16 || ts_vector_literal_needs_value_eval(c, node)) {
    return 0;
  }

  count = node->data.vector.count;
  for (size_t i = 0; i < count; ++i) {
    element_regs[i] = ts_compile_expr(c, node->data.vector.elements[i]);
  }
  arr_reg = ts_emit_packed_args(c, count, element_regs);

  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.vec_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.vec_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_int_op(c->ctx, (int64_t)count), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

static int ts_map_literal_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_MAP_LITERAL) return 1;

  for (size_t i = 0; i < node->data.map_literal.count; ++i) {
    exprtk_node_t *value = node->data.map_literal.values[i];
    if (!node->data.map_literal.keys[i] || !value || value->type == EXPRTK_NODE_SPREAD ||
        ts_is_non_numeric_node(value) || ts_expr_contains_value_call(c, value) ||
        ts_index_assignment_needs_value_eval(c, value)) {
      return 1;
    }
  }

  return 0;
}

static MIR_reg_t ts_emit_map_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                            exprtk_node_t *node) {
  MIR_reg_t value_regs[16];
  MIR_reg_t arr_reg;
  MIR_reg_t res = new_temp_reg(c);
  size_t count;

  if (!target_name || !node || node->type != EXPRTK_NODE_MAP_LITERAL ||
      node->data.map_literal.count > 16 || ts_map_literal_needs_value_eval(c, node)) {
    return 0;
  }

  count = node->data.map_literal.count;
  for (size_t i = 0; i < count; ++i) {
    value_regs[i] = ts_compile_expr(c, node->data.map_literal.values[i]);
  }
  arr_reg = ts_emit_packed_args(c, count, value_regs);

  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.map_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.map_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node),
                        MIR_new_int_op(c->ctx, (int64_t)count), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

static MIR_reg_t ts_emit_map_value_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                                  exprtk_node_t *node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);

  if (!target_name || !node || node->type != EXPRTK_NODE_MAP_LITERAL) return 0;

  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6,
                                    MIR_new_ref_op(c->ctx, c->ext.map_value_assign_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.map_value_assign_import),
                                    MIR_new_reg_op(c->ctx, res),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  ts_emit_reload_from_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                               MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

static MIR_reg_t ts_emit_string_literal_assign(ts_mir_compiler_t *c, const char *target_name,
                                               exprtk_node_t *node) {
  MIR_reg_t res = new_temp_reg(c);

  if (!target_name || !node || node->type != EXPRTK_NODE_STRING ||
      !node->data.string.value.data) {
    return 0;
  }

  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.string_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.string_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node->data.string.value.data),
                        MIR_new_int_op(c->ctx, (int64_t)node->data.string.value.len)));
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

static MIR_reg_t ts_emit_template_string_assign(ts_mir_compiler_t *c, const char *target_name,
                                                exprtk_node_t *node) {
  MIR_reg_t res = new_temp_reg(c);

  if (!target_name || !node || node->type != EXPRTK_NODE_TEMPLATE_STRING) {
    return 0;
  }

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.template_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.template_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  ts_mir_mark_var_clean(c, target_name);
  return res;
}

/* --- Access helpers: member/index fast path + runtime helpers --- */
static MIR_reg_t ts_emit_member_access(ts_mir_compiler_t *c, const char *obj_name,
                                       const char *member, exprtk_node_t *object_node) {
  exprtk_value_t obj = exprtk_env_get(&c->ts_ctx->env, obj_name);
  MIR_reg_t oop_slot = ts_try_emit_oop_member_get_slot(c, obj_name, member, object_node);
  if (oop_slot) return oop_slot;

  if (exprtk_value_is_object_like(&obj) && exprtk_map_has(&obj, member)) {
    MIR_reg_t ptr_reg = ts_mir_get_or_add_map_ptr(c, obj_name, member);

    if (ptr_reg) {
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, 0, 1)));
      return res;
    }

    MIR_reg_t res = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.map_get_key_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.map_get_key_import),
                                      MIR_new_reg_op(c->ctx, res),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member)));
    return res;
  }

  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));
  MIR_reg_t res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.member_get_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.member_get_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  return res;
}

static MIR_reg_t ts_emit_member_access_assign(ts_mir_compiler_t *c, const char *target_name,
                                              const char *obj_name, const char *member,
                                              exprtk_node_t *object_node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_reg_t target = get_or_create_reg(c, target_name);
  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));
  (void)target;

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.member_get_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.member_get_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)obj_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, res)));
  ts_mir_mark_var_clean(c, target_name);
  ts_mir_mark_var_dynamic(c, target_name);
  return target;
}

static void ts_emit_sync_var_to_env(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    if (!c->vars[i]->dirty || strcmp(c->vars[i]->name, name) != 0) continue;

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
    c->vars[i]->dirty = 0;
    return;
  }
}

static void ts_emit_sync_to_env(ts_mir_compiler_t *c) {
  // 增量同步：只同步脏变量
  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    if (!c->vars[i]->dirty) continue;  // 跳过未修改的变量

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
    c->vars[i]->dirty = 0;
  }
}

static void ts_emit_reload_from_env(ts_mir_compiler_t *c) {
  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(
                        c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                        MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                        MIR_new_reg_op(c->ctx, c->vars[i]->reg),
                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name)));
  }
  // 清除脏标记（重新加载后所有变量都是干净的）
  ts_mir_clear_dirty_flags(c);
}

static MIR_reg_t ts_emit_unsupported_node(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (node && node->type == EXPRTK_NODE_MEMBER_CALL) {
    ts_mir_fail(c,
                "MIR lowering error: unsupported member call '%s' on syntax object type %d",
                node->data.member_call.method ? node->data.member_call.method : "<null>",
                node->data.member_call.object ? (int)node->data.member_call.object->type : -1);
    return ts_emit_zero_reg(c);
  }
  if (node && (node->type == EXPRTK_NODE_ASSIGNMENT ||
               node->type == EXPRTK_NODE_CONSTANT_DECL)) {
    exprtk_node_t *value = node->data.assignment.value;
    if (value && value->type == EXPRTK_NODE_FUNCTION_CALL) {
      ts_mir_fail(c,
                  "MIR lowering error: unsupported assignment '%s' with RHS function call "
                  "'%s' argc %zu",
                  node->data.assignment.name ? node->data.assignment.name : "<null>",
                  value->data.function.name ? value->data.function.name : "<null>",
                  value->data.function.arg_count);
      return ts_emit_zero_reg(c);
    }
    ts_mir_fail(c,
                "MIR lowering error: unsupported assignment '%s' with RHS syntax node type %d",
                node->data.assignment.name ? node->data.assignment.name : "<null>",
                value ? (int)value->type : -1);
    return ts_emit_zero_reg(c);
  }
  if (node && node->type == EXPRTK_NODE_INDEX) {
    ts_mir_fail(c,
                "MIR lowering error: unsupported index array syntax node type %d",
                node->data.index_access.array ? (int)node->data.index_access.array->type : -1);
    return ts_emit_zero_reg(c);
  }
  ts_mir_fail(c, "MIR lowering error: unsupported syntax node type %d",
              node ? (int)node->type : -1);
  return ts_emit_zero_reg(c);
}

static MIR_reg_t ts_emit_runtime_value_node(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) {
    MIR_reg_t res = new_temp_reg(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res), MIR_new_double_op(c->ctx, 0.0)));
    return res;
  }

  if ((node->type == EXPRTK_NODE_ASSIGNMENT || node->type == EXPRTK_NODE_CONSTANT_DECL) &&
      node->data.assignment.name) {
    return ts_emit_value_expr_assign(c, node->data.assignment.name, node);
  }

  return ts_emit_value_expr(c, node);
}

static void ts_emit_oop_define_class(ts_mir_compiler_t *c, exprtk_node_t *node) {
  ts_emit_sync_to_env(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 4,
                                    MIR_new_ref_op(c->ctx, c->ext.oop_define_class_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.oop_define_class_import),
                                    MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
}

static MIR_reg_t ts_emit_oop_class_alias(ts_mir_compiler_t *c, const char *target_name,
                                         const char *source_name) {
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.oop_alias_class_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_alias_class_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)source_name)));
  ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_oop_new_assign(ts_mir_compiler_t *c, const char *target_name,
                                        exprtk_node_t *new_node) {
  size_t argc = new_node->data.new_expr.arg_count;
  MIR_reg_t arg_regs[16];
  argc = ts_compile_call_args(c, argc, new_node->data.new_expr.args, arg_regs);
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)new_node->data.new_expr.class_name),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_oop_class_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                               exprtk_node_t *call_node) {
  size_t argc = call_node->data.function.arg_count;
  MIR_reg_t arg_regs[16];
  argc = ts_compile_call_args(c, argc, call_node->data.function.args, arg_regs);
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_new_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node->data.function.name),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  return res;
}

static int ts_oop_arg_needs_value_bridge(ts_mir_compiler_t *c, exprtk_node_t *arg) {
  if (!arg) return 0;

  switch (arg->type) {
  case EXPRTK_NODE_NULL:
  case EXPRTK_NODE_STRING:
  case EXPRTK_NODE_VECTOR:
  case EXPRTK_NODE_MAP_LITERAL:
  case EXPRTK_NODE_SLICE:
  case EXPRTK_NODE_TEMPLATE_STRING:
  case EXPRTK_NODE_NEW:
  case EXPRTK_NODE_THIS:
  case EXPRTK_NODE_SUPER:
  case EXPRTK_NODE_FUNCTION_EXPRESSION:
  case EXPRTK_NODE_CLASS_DEF:
    return 1;

  case EXPRTK_NODE_VARIABLE:
    return ts_mir_find_var_class(c, arg->data.variable.name) ||
           ts_mir_is_known_class_name(c, arg->data.variable.name);

  case EXPRTK_NODE_FUNCTION_CALL:
    if (arg->data.function.name) {
      const char *name = arg->data.function.name;
      const char *alias_name = ts_find_func_alias(c, name);
      if (alias_name) name = alias_name;
      return ts_function_call_is_known_class(c, name) ||
             ts_call_assignment_needs_value_bridge(c, name, arg->data.function.arg_count);
    }
    return 0;

  case EXPRTK_NODE_MEMBER_ACCESS:
  case EXPRTK_NODE_MEMBER_CALL:
    return 1;

  default:
    return 0;
  }
}

static int ts_oop_call_args_need_value_bridge(ts_mir_compiler_t *c, size_t argc,
                                              exprtk_node_t **args) {
  if (!c || !args) return 0;
  for (size_t i = 0; i < argc; ++i) {
    if (ts_oop_arg_needs_value_bridge(c, args[i])) return 1;
  }
  return 0;
}

static const char *ts_emit_oop_receiver_to_temp(ts_mir_compiler_t *c, exprtk_node_t *object_node) {
  const char *target_name = NULL;
  if (!c || !object_node) return NULL;

  if (object_node->type == EXPRTK_NODE_VARIABLE) {
    return object_node->data.variable.name;
  }

  target_name = ts_mir_new_hidden_receiver_name(c);
  if (!target_name) return NULL;

  if (object_node->type == EXPRTK_NODE_NEW && object_node->data.new_expr.class_name) {
    ts_emit_oop_new_assign(c, target_name, object_node);
    ts_mir_set_var_class(c, target_name, object_node->data.new_expr.class_name);
    return target_name;
  }

  if (object_node->type == EXPRTK_NODE_FUNCTION_CALL && object_node->data.function.name) {
    const char *name = object_node->data.function.name;
    if (ts_mir_is_known_class_name(c, name)) {
      ts_emit_oop_class_call_assign(c, target_name, object_node);
      ts_mir_set_var_class(c, target_name, name);
      return target_name;
    }

    if (ts_call_assignment_needs_value_bridge(c, name, object_node->data.function.arg_count)) {
      ts_emit_runtime_call_assign(c, target_name, name, object_node->data.function.arg_count,
                                  object_node->data.function.args);
      return target_name;
    }
  }

  if (object_node->type == EXPRTK_NODE_MEMBER_CALL && object_node->data.member_call.object &&
      object_node->data.member_call.method) {
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, object_node->data.member_call.object);
    if (!obj_name) return NULL;

    if (ts_oop_call_args_need_value_bridge(c, object_node->data.member_call.arg_count,
                                           object_node->data.member_call.args)) {
      ts_emit_oop_member_call_assign_value(c, target_name, obj_name,
                                           object_node->data.member_call.method,
                                           object_node->data.member_call.object, object_node);
      return target_name;
    }

    MIR_reg_t arg_regs[16];
    size_t argc =
        ts_compile_call_args(c, object_node->data.member_call.arg_count,
                             object_node->data.member_call.args, arg_regs);
    ts_emit_oop_member_call_assign(c, target_name, obj_name, object_node->data.member_call.method,
                                   object_node->data.member_call.object, argc, arg_regs);
    return target_name;
  }

  if (object_node->type == EXPRTK_NODE_MEMBER_ACCESS && object_node->data.member_access.object &&
      object_node->data.member_access.member) {
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, object_node->data.member_access.object);
    if (!obj_name) return NULL;

    ts_emit_member_access_assign(c, target_name, obj_name, object_node->data.member_access.member,
                                 object_node->data.member_access.object);
    return target_name;
  }

  ts_emit_unsupported_node(c, object_node);
  return NULL;
}

static int ts_oop_predicate_kind(const char *name) {
  if (!name) return 0;
  if (strcmp(name, "is_class") == 0) return 1;
  if (strcmp(name, "is_instance") == 0) return 2;
  if (strcmp(name, "is_function") == 0) return 3;
  if (strcmp(name, "is_null") == 0) return 4;
  return 0;
}

static int ts_oop_predicate_can_preserve_value(int kind, exprtk_node_t *arg) {
  if (!arg) return 0;
  if (kind != 4) return 1;
  return arg->type == EXPRTK_NODE_NULL || arg->type == EXPRTK_NODE_VARIABLE ||
         arg->type == EXPRTK_NODE_MEMBER_ACCESS;
}

static MIR_reg_t ts_emit_oop_predicate(ts_mir_compiler_t *c, int kind, exprtk_node_t *arg) {
  const char *name = NULL;
  const char *member = NULL;
  exprtk_node_t *object_node = NULL;
  MIR_reg_t res = new_temp_reg(c);

  if (!c || !arg || kind == 0) return ts_emit_unsupported_node(c, arg);

  if (kind == 4 && arg->type == EXPRTK_NODE_NULL) {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 1.0)));
    return res;
  }

  if (arg->type == EXPRTK_NODE_VARIABLE) {
    name = arg->data.variable.name;
    object_node = arg;
  } else if (arg->type == EXPRTK_NODE_MEMBER_ACCESS && arg->data.member_access.object &&
             arg->data.member_access.member) {
    name = ts_emit_oop_receiver_to_temp(c, arg->data.member_access.object);
    member = arg->data.member_access.member;
    object_node = arg->data.member_access.object;
  } else {
    name = ts_emit_oop_receiver_to_temp(c, arg);
    object_node = arg;
  }

  if (!name) return ts_emit_unsupported_node(c, arg);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_predicate_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_predicate_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_int_op(c->ctx, (int64_t)kind),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  ts_emit_reload_from_env(c);
  return res;
}

static int ts_typeof_oop_kind(exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_STRING || !node->data.string.value.data) return 0;

  tstr_v value = node->data.string.value;
  if (value.len == 5 && memcmp(value.data, "class", 5) == 0) return 1;
  if (value.len == 8 && memcmp(value.data, "instance", 8) == 0) return 2;
  if (value.len == 8 && memcmp(value.data, "function", 8) == 0) return 3;
  return 0;
}

static int ts_typeof_arg(exprtk_node_t *node, exprtk_node_t **out_arg) {
  if (!node || node->type != EXPRTK_NODE_FUNCTION_CALL || !node->data.function.name ||
      strcmp(node->data.function.name, "typeof") != 0 || node->data.function.arg_count != 1) {
    return 0;
  }

  if (out_arg) *out_arg = node->data.function.args[0];
  return 1;
}

static int ts_try_emit_typeof_oop_compare(ts_mir_compiler_t *c, exprtk_node_t *left,
                                          exprtk_node_t *right, int op, MIR_reg_t *out) {
  exprtk_node_t *arg = NULL;
  int kind = 0;

  if (!c || !left || !right || !out ||
      (op != exprtk_TOKEN_EQ && op != exprtk_TOKEN_NE)) {
    return 0;
  }

  if (ts_typeof_arg(left, &arg)) {
    kind = ts_typeof_oop_kind(right);
  } else if (ts_typeof_arg(right, &arg)) {
    kind = ts_typeof_oop_kind(left);
  }
  if (!kind || !arg) return 0;

  MIR_reg_t pred = ts_emit_oop_predicate(c, kind, arg);
  if (op == exprtk_TOKEN_EQ) {
    *out = pred;
    return 1;
  }

  MIR_reg_t ireg = new_temp_ireg(c);
  MIR_reg_t res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DEQ, MIR_new_reg_op(c->ctx, ireg),
                               MIR_new_reg_op(c->ctx, pred), MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                               MIR_new_reg_op(c->ctx, ireg)));
  *out = res;
  return 1;
}

static MIR_reg_t ts_emit_oop_member_call(ts_mir_compiler_t *c, const char *object_name,
                                         const char *method_name, exprtk_node_t *object_node,
                                         size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_oop_member_call_value(ts_mir_compiler_t *c, const char *object_name,
                                               const char *method_name,
                                               exprtk_node_t *object_node,
                                               exprtk_node_t *call_node) {
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_value_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_value_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_oop_member_call_mono_value(ts_mir_compiler_t *c,
                                                    const char *object_name,
                                                    const char *expected_class_name,
                                                    const char *method_name,
                                                    exprtk_node_t *object_node,
                                                    exprtk_node_t *call_node) {
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9,
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_mono_value_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_mono_value_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)expected_class_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  ts_emit_reload_from_env(c);
  return res;
}

static int ts_name_is_param(const char *name, exprtk_node_t **params, size_t param_count) {
  if (!name) return 0;
  for (size_t i = 0; i < param_count; ++i) {
    if (params[i] && params[i]->type == EXPRTK_NODE_VARIABLE &&
        params[i]->data.variable.name && strcmp(params[i]->data.variable.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ts_oop_method_expr_is_read_only(exprtk_node_t *node);

static int ts_oop_method_body_needs_reload(exprtk_node_t *node, exprtk_node_t **params,
                                           size_t param_count) {
  if (!node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_NUMBER:
  case EXPRTK_NODE_INTEGER:
  case EXPRTK_NODE_VARIABLE:
  case EXPRTK_NODE_STRING:
  case EXPRTK_NODE_NULL:
  case EXPRTK_NODE_THIS:
    return 0;

  case EXPRTK_NODE_BINARY_OP:
    return ts_oop_method_body_needs_reload(node->data.binary.left, params, param_count) ||
           ts_oop_method_body_needs_reload(node->data.binary.right, params, param_count);

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_oop_method_body_needs_reload(node->data.block.statements[i], params, param_count)) {
        return 1;
      }
    }
    return 0;

  case EXPRTK_NODE_FLOW:
    return ts_oop_method_expr_is_read_only(node->data.flow.value) ? 0 : 1;

  case EXPRTK_NODE_ASSIGNMENT:
    if (!ts_name_is_param(node->data.assignment.name, params, param_count)) return 1;
    return ts_oop_method_expr_is_read_only(node->data.assignment.value) ? 0 : 1;

  case EXPRTK_NODE_MEMBER_ACCESS:
    return ts_oop_method_expr_is_read_only(node->data.member_access.object) ? 0 : 1;

  case EXPRTK_NODE_MEMBER_SET:
    if (!node->data.member_set.object ||
        node->data.member_set.object->type != EXPRTK_NODE_THIS) {
      return 1;
    }
    return ts_oop_method_expr_is_read_only(node->data.member_set.value) ? 0 : 1;

  default:
    return 1;
  }
}

static int ts_oop_method_expr_is_read_only(exprtk_node_t *node) {
  return !ts_oop_method_body_needs_reload(node, NULL, 0);
}

static int ts_oop_member_call_can_skip_reload(ts_mir_compiler_t *c, const char *class_name,
                                              const char *method_name, size_t argc) {
  exprtk_value_t class_value;
  exprtk_value_t args[16];
  exprtk_func_t *method;

  if (!c || !class_name || !method_name || argc > 16) return 0;
  class_value = exprtk_env_get(&c->ts_ctx->env, class_name);
  if (class_value.type != EXPRTK_VAL_CLASS || !class_value.data.class_val.klass) return 0;

  for (size_t i = 0; i < argc; ++i) {
    args[i] = exprtk_val_num(0.0);
  }
  method = exprtk_class_lookup_method_typed(class_value.data.class_val.klass, method_name, 0,
                                            argc, argc ? args : NULL);
  if (!method || !method->is_script) return 0;
  return !ts_oop_method_body_needs_reload(method->data.script.body,
                                          method->data.script.arg_params,
                                          method->data.script.arg_count);
}

static MIR_reg_t ts_emit_oop_member_call_cached(ts_mir_compiler_t *c, const char *object_name,
                                                const char *method_name, exprtk_node_t *object_node,
                                                size_t argc, const MIR_reg_t *arg_regs,
                                                int reload_after) {
  exprtk_oop_method_cache_t *cache =
      (exprtk_oop_method_cache_t *)calloc(1, sizeof(exprtk_oop_method_cache_t));
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  if (!reload_after) ts_mir_clear_dirty_flags(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 10, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_cached_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_cached_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  if (reload_after) ts_emit_reload_from_env(c);
  return res;
}

static MIR_reg_t ts_emit_oop_member_call_assign(ts_mir_compiler_t *c, const char *target_name,
                                                const char *object_name, const char *method_name,
                                                exprtk_node_t *object_node, size_t argc,
                                                const MIR_reg_t *arg_regs) {
  exprtk_oop_method_cache_t *cache =
      (exprtk_oop_method_cache_t *)calloc(1, sizeof(exprtk_oop_method_cache_t));
  MIR_reg_t arr_reg = ts_emit_packed_args(c, argc, arg_regs);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 11, MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, (int64_t)argc), MIR_new_reg_op(c->ctx, arr_reg)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  return res;
}

static MIR_reg_t ts_emit_oop_member_call_assign_value(ts_mir_compiler_t *c,
                                                      const char *target_name,
                                                      const char *object_name,
                                                      const char *method_name,
                                                      exprtk_node_t *object_node,
                                                      exprtk_node_t *call_node) {
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9,
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_value_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_call_assign_value_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)target_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)method_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)call_node)));
  ts_emit_reload_from_env(c);
  ts_mir_mark_var_dynamic(c, target_name);
  return res;
}

static MIR_reg_t ts_emit_oop_member_get(ts_mir_compiler_t *c, const char *object_name,
                                        const char *member_name, exprtk_node_t *object_node) {
  MIR_reg_t slot_value = ts_try_emit_oop_member_get_slot(c, object_name, member_name, object_node);
  if (slot_value) return slot_value;

  MIR_reg_t res = new_temp_reg(c);
  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));
  ts_emit_sync_var_to_env(c, object_name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.oop_member_get_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_get_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node)));
  return res;
}

static MIR_reg_t ts_emit_oop_member_set(ts_mir_compiler_t *c, const char *object_name,
                                        const char *member_name, exprtk_node_t *object_node,
                                        exprtk_node_t *value_node) {
  MIR_reg_t value_reg = ts_compile_expr(c, value_node);
  MIR_reg_t slot_result =
      ts_try_emit_oop_member_set_slot(c, object_name, member_name, object_node, value_reg);
  if (slot_result) return slot_result;

  MIR_reg_t res = new_temp_reg(c);
  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));

  ts_emit_sync_var_to_env(c, object_name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_member_set_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_set_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_reg_op(c->ctx, value_reg)));
  return res;
}

static MIR_reg_t ts_emit_oop_member_set_reg(ts_mir_compiler_t *c, const char *object_name,
                                            const char *member_name, exprtk_node_t *object_node,
                                            MIR_reg_t value_reg) {
  MIR_reg_t slot_result =
      ts_try_emit_oop_member_set_slot(c, object_name, member_name, object_node, value_reg);
  if (slot_result) return slot_result;

  MIR_reg_t res = new_temp_reg(c);
  exprtk_oop_field_cache_t *cache =
      (exprtk_oop_field_cache_t *)calloc(1, sizeof(exprtk_oop_field_cache_t));

  ts_emit_sync_var_to_env(c, object_name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_member_set_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_member_set_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)cache),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_reg_op(c->ctx, value_reg)));
  return res;
}

static exprtk_node_t *ts_find_top_level_class_def(ts_mir_compiler_t *c, const char *class_name) {
  if (!c || !c->ast_root || !class_name || c->ast_root->type != EXPRTK_NODE_BLOCK) return NULL;

  for (size_t i = 0; i < c->ast_root->data.block.count; ++i) {
    exprtk_node_t *stmt = c->ast_root->data.block.statements[i];
    if (stmt && stmt->type == EXPRTK_NODE_CLASS_DEF && stmt->data.class_def.name &&
        strcmp(stmt->data.class_def.name, class_name) == 0) {
      return stmt;
    }
  }
  return NULL;
}

static int ts_top_level_class_name_is_assigned(ts_mir_compiler_t *c, const char *class_name) {
  if (!c || !c->ast_root || !class_name || c->ast_root->type != EXPRTK_NODE_BLOCK) return 0;

  for (size_t i = 0; i < c->ast_root->data.block.count; ++i) {
    exprtk_node_t *stmt = c->ast_root->data.block.statements[i];
    if (!stmt) continue;
    if ((stmt->type == EXPRTK_NODE_ASSIGNMENT || stmt->type == EXPRTK_NODE_CONSTANT_DECL) &&
        stmt->data.assignment.name && strcmp(stmt->data.assignment.name, class_name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ts_inline_param_index(exprtk_node_t **params, size_t param_count, const char *name,
                                 size_t *out_index) {
  if (!name) return 0;
  for (size_t i = 0; i < param_count; ++i) {
    if (params[i] && params[i]->type == EXPRTK_NODE_VARIABLE &&
        params[i]->data.variable.name && strcmp(params[i]->data.variable.name, name) == 0) {
      if (out_index) *out_index = i;
      return 1;
    }
  }
  return 0;
}

static exprtk_node_t *ts_find_inlineable_instance_method(ts_mir_compiler_t *c,
                                                        const char *class_name,
                                                        const char *method_name,
                                                        size_t argc);

static const char *ts_direct_parent_class_name(ts_mir_compiler_t *c, const char *class_name) {
  exprtk_node_t *class_node = ts_find_top_level_class_def(c, class_name);
  if (!class_node || !class_node->data.class_def.parent_name) return NULL;
  if (ts_top_level_class_name_is_assigned(c, class_node->data.class_def.parent_name)) return NULL;
  return class_node->data.class_def.parent_name;
}

static int ts_node_mentions_this_field(exprtk_node_t *node, const char *member_name) {
  if (!node || !member_name) return 0;

  switch (node->type) {
  case EXPRTK_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object &&
        node->data.member_access.object->type == EXPRTK_NODE_THIS &&
        node->data.member_access.member &&
        strcmp(node->data.member_access.member, member_name) == 0) {
      return 1;
    }
    return ts_node_mentions_this_field(node->data.member_access.object, member_name);

  case EXPRTK_NODE_MEMBER_SET:
    if (node->data.member_set.object &&
        node->data.member_set.object->type == EXPRTK_NODE_THIS &&
        node->data.member_set.member &&
        strcmp(node->data.member_set.member, member_name) == 0) {
      return 1;
    }
    return ts_node_mentions_this_field(node->data.member_set.object, member_name) ||
           ts_node_mentions_this_field(node->data.member_set.value, member_name);

  case EXPRTK_NODE_BINARY_OP:
    return ts_node_mentions_this_field(node->data.binary.left, member_name) ||
           ts_node_mentions_this_field(node->data.binary.right, member_name);

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_mentions_this_field(node->data.block.statements[i], member_name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_FLOW:
    return ts_node_mentions_this_field(node->data.flow.value, member_name);

  case EXPRTK_NODE_IF:
    return ts_node_mentions_this_field(node->data.if_stmt.condition, member_name) ||
           ts_node_mentions_this_field(node->data.if_stmt.if_branch, member_name) ||
           ts_node_mentions_this_field(node->data.if_stmt.else_branch, member_name);

  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_mentions_this_field(node->data.function.args[i], member_name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_mentions_this_field(node->data.member_call.object, member_name)) return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_mentions_this_field(node->data.member_call.args[i], member_name)) return 1;
    }
    return 0;

  default:
    return 0;
  }
}

static int ts_class_has_public_instance_field_hint(ts_mir_compiler_t *c,
                                                   const char *class_name,
                                                   const char *member_name) {
  exprtk_node_t *class_node = ts_find_top_level_class_def(c, class_name);
  if (!class_node || !member_name) return 0;

  for (size_t i = 0; i < class_node->data.class_def.method_count; ++i) {
    exprtk_node_t *entry = class_node->data.class_def.methods[i];
    if (!entry) continue;
    if (entry->type == EXPRTK_NODE_FIELD_DECL && entry->data.field_decl.name &&
        strcmp(entry->data.field_decl.name, member_name) == 0 &&
        !entry->data.field_decl.is_static) {
      return entry->data.field_decl.access_level == EXPRTK_ACCESS_PUBLIC ? 1 : -1;
    }
  }

  for (size_t i = 0; i < class_node->data.class_def.method_count; ++i) {
    exprtk_node_t *entry = class_node->data.class_def.methods[i];
    if (!entry || entry->type != EXPRTK_NODE_METHOD || entry->data.method.is_static) continue;
    if (ts_node_mentions_this_field(entry->data.method.body, member_name)) return 1;
  }

  const char *parent_name = ts_direct_parent_class_name(c, class_name);
  if (parent_name) return ts_class_has_public_instance_field_hint(c, parent_name, member_name);
  return 0;
}

static int ts_oop_member_can_use_num_slot(ts_mir_compiler_t *c, const char *object_name,
                                          const char *member_name) {
  const char *class_name = ts_mir_find_var_class(c, object_name);
  if (!class_name || !member_name) return 0;
  if (member_name[0] == '_') return 0;
  return ts_class_has_public_instance_field_hint(c, class_name, member_name) > 0;
}

static MIR_reg_t ts_emit_oop_slot_cache_instance(ts_mir_compiler_t *c, MIR_reg_t cache_reg) {
  MIR_reg_t instance_reg = new_temp_preg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, instance_reg),
                               MIR_new_mem_op(c->ctx, MIR_T_I64,
                                              (int64_t)offsetof(ts_mir_oop_slot_cache_t, instance),
                                              cache_reg, 0, 1)));
  return instance_reg;
}

static void ts_emit_oop_slot_cache_init(ts_mir_compiler_t *c, MIR_reg_t cache_reg,
                                        const char *object_name, const char *member_name,
                                        exprtk_node_t *object_node, int create_if_missing,
                                        MIR_label_t missing_label) {
  MIR_label_t ready_label = MIR_new_label(c->ctx);
  MIR_reg_t instance_reg = ts_emit_oop_slot_cache_instance(c, cache_reg);

  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_BNE, MIR_new_label_op(c->ctx, ready_label),
                               MIR_new_reg_op(c->ctx, instance_reg), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 9, MIR_new_ref_op(c->ctx, c->ext.oop_num_ptr_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_num_ptr_import),
                        MIR_new_reg_op(c->ctx, instance_reg), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)member_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_node),
                        MIR_new_int_op(c->ctx, create_if_missing ? 1 : 0),
                        MIR_new_reg_op(c->ctx, cache_reg)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_BEQ, MIR_new_label_op(c->ctx, missing_label),
                               MIR_new_reg_op(c->ctx, instance_reg), MIR_new_int_op(c->ctx, 0)));
  MIR_append_insn(c->ctx, c->func, ready_label);
}

static MIR_reg_t ts_try_emit_oop_member_get_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 exprtk_node_t *object_node) {
  if (!ts_oop_member_can_use_num_slot(c, object_name, member_name)) return 0;

  MIR_reg_t cache_reg = ts_mir_get_or_add_oop_ptr(c, object_name, member_name);
  if (!cache_reg) return 0;

  MIR_label_t missing_label = MIR_new_label(c->ctx);
  MIR_label_t done_label = MIR_new_label(c->ctx);
  MIR_reg_t res = new_temp_reg(c);

  ts_emit_sync_var_to_env(c, object_name);
  ts_emit_oop_slot_cache_init(c, cache_reg, object_name, member_name, object_node, 0,
                              missing_label);

  MIR_reg_t instance_reg = ts_emit_oop_slot_cache_instance(c, cache_reg);
  MIR_reg_t slots_reg = new_temp_preg(c);
  MIR_reg_t index_reg = new_temp_ireg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, slots_reg),
                               MIR_new_mem_op(c->ctx, MIR_T_I64,
                                              (int64_t)offsetof(exprtk_instance_t, field_slots),
                                              instance_reg, 0, 1)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, index_reg),
                               MIR_new_mem_op(c->ctx, MIR_T_I64,
                                              (int64_t)offsetof(ts_mir_oop_slot_cache_t, index),
                                              cache_reg, 0, 1)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                               MIR_new_mem_op(c->ctx, MIR_T_D,
                                              (int64_t)offsetof(exprtk_value_t, data.number),
                                              slots_reg, index_reg,
                                              (int64_t)sizeof(exprtk_value_t))));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done_label)));

  MIR_append_insn(c->ctx, c->func, missing_label);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                               MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, c->func, done_label);
  return res;
}

static MIR_reg_t ts_try_emit_oop_member_set_slot(ts_mir_compiler_t *c, const char *object_name,
                                                 const char *member_name,
                                                 exprtk_node_t *object_node,
                                                 MIR_reg_t value_reg) {
  if (!ts_oop_member_can_use_num_slot(c, object_name, member_name)) return 0;

  MIR_reg_t cache_reg = ts_mir_get_or_add_oop_ptr(c, object_name, member_name);
  if (!cache_reg) return 0;

  MIR_label_t missing_label = MIR_new_label(c->ctx);
  MIR_label_t done_label = MIR_new_label(c->ctx);

  ts_emit_sync_var_to_env(c, object_name);
  ts_emit_oop_slot_cache_init(c, cache_reg, object_name, member_name, object_node, 1,
                              missing_label);

  MIR_reg_t instance_reg = ts_emit_oop_slot_cache_instance(c, cache_reg);
  MIR_reg_t slots_reg = new_temp_preg(c);
  MIR_reg_t index_reg = new_temp_ireg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, slots_reg),
                               MIR_new_mem_op(c->ctx, MIR_T_I64,
                                              (int64_t)offsetof(exprtk_instance_t, field_slots),
                                              instance_reg, 0, 1)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, index_reg),
                               MIR_new_mem_op(c->ctx, MIR_T_I64,
                                              (int64_t)offsetof(ts_mir_oop_slot_cache_t, index),
                                              cache_reg, 0, 1)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DMOV,
                               MIR_new_mem_op(c->ctx, MIR_T_D,
                                              (int64_t)offsetof(exprtk_value_t, data.number),
                                              slots_reg, index_reg,
                                              (int64_t)sizeof(exprtk_value_t)),
                               MIR_new_reg_op(c->ctx, value_reg)));
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, done_label)));
  MIR_append_insn(c->ctx, c->func, missing_label);
  MIR_append_insn(c->ctx, c->func, done_label);
  return value_reg;
}

static int ts_oop_inline_expr_supported(ts_mir_compiler_t *c, const char *class_name,
                                        exprtk_node_t *node, exprtk_node_t **params,
                                        size_t param_count) {
  if (!node) return 1;

  switch (node->type) {
  case EXPRTK_NODE_NUMBER:
  case EXPRTK_NODE_INTEGER:
  case EXPRTK_NODE_NULL:
    return 1;

  case EXPRTK_NODE_VARIABLE:
    return ts_inline_param_index(params, param_count, node->data.variable.name, NULL);

  case EXPRTK_NODE_MEMBER_ACCESS:
    return node->data.member_access.object &&
           node->data.member_access.object->type == EXPRTK_NODE_THIS &&
           node->data.member_access.member != NULL;

  case EXPRTK_NODE_SUPER: {
    const char *parent_name;
    if (!node->data.super_expr.is_call || !node->data.super_expr.member) return 0;
    parent_name = ts_direct_parent_class_name(c, class_name);
    if (!parent_name) return 0;
    for (size_t i = 0; i < node->data.super_expr.arg_count; ++i) {
      if (!ts_oop_inline_expr_supported(c, class_name, node->data.super_expr.args[i],
                                        params, param_count)) {
        return 0;
      }
    }
    return ts_find_inlineable_instance_method(c, parent_name, node->data.super_expr.member,
                                              node->data.super_expr.arg_count) != NULL;
  }

  case EXPRTK_NODE_BINARY_OP:
    if (node->data.binary.left == NULL) {
      return node->data.binary.op == exprtk_TOKEN_PLUS ||
             node->data.binary.op == exprtk_TOKEN_MINUS;
    }
    switch (node->data.binary.op) {
    case exprtk_TOKEN_PLUS:
    case exprtk_TOKEN_MINUS:
    case exprtk_TOKEN_MULTIPLY:
    case exprtk_TOKEN_DIVIDE:
    case exprtk_TOKEN_LT:
    case exprtk_TOKEN_GT:
    case exprtk_TOKEN_LE:
    case exprtk_TOKEN_GE:
    case exprtk_TOKEN_EQ:
    case exprtk_TOKEN_NE:
      return ts_oop_inline_expr_supported(c, class_name, node->data.binary.left, params,
                                          param_count) &&
             ts_oop_inline_expr_supported(c, class_name, node->data.binary.right, params,
                                          param_count);
    default:
      return 0;
    }

  default:
    return 0;
  }
}

static int ts_oop_inline_stmt_supported(ts_mir_compiler_t *c, const char *class_name,
                                        exprtk_node_t *node, exprtk_node_t **params,
                                        size_t param_count) {
  if (!node) return 1;

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (!ts_oop_inline_stmt_supported(c, class_name, node->data.block.statements[i],
                                        params, param_count)) {
        return 0;
      }
    }
    return 1;

  case EXPRTK_NODE_FLOW:
    return node->data.flow.type == exprtk_TOKEN_RETURN &&
           ts_oop_inline_expr_supported(c, class_name, node->data.flow.value, params,
                                        param_count);

  case EXPRTK_NODE_MEMBER_SET:
    return node->data.member_set.object &&
           node->data.member_set.object->type == EXPRTK_NODE_THIS &&
           node->data.member_set.member &&
           ts_oop_inline_expr_supported(c, class_name, node->data.member_set.value, params,
                                        param_count);

  default:
    return ts_oop_inline_expr_supported(c, class_name, node, params, param_count);
  }
}

static exprtk_node_t *ts_find_inlineable_instance_method(ts_mir_compiler_t *c,
                                                        const char *class_name,
                                                        const char *method_name,
                                                        size_t argc) {
  exprtk_node_t *class_node = ts_find_top_level_class_def(c, class_name);
  exprtk_node_t *found = NULL;

  if (!class_node || !method_name) return NULL;
  if (method_name[0] == '_') return NULL;
  if (ts_top_level_class_name_is_assigned(c, class_name)) return NULL;
  if (class_node->data.class_def.interface_count > 0 || class_node->data.class_def.is_abstract ||
      class_node->data.class_def.is_interface) {
    return NULL;
  }

  for (size_t i = 0; i < class_node->data.class_def.method_count; ++i) {
    exprtk_node_t *method = class_node->data.class_def.methods[i];
    if (method && method->type == EXPRTK_NODE_FIELD_DECL &&
        method->data.field_decl.access_level != EXPRTK_ACCESS_PUBLIC) {
      return NULL;
    }
    if (!method || method->type != EXPRTK_NODE_METHOD || !method->data.method.name) continue;
    if (strcmp(method->data.method.name, method_name) != 0) continue;
    if (method->data.method.is_static || method->data.method.is_abstract) continue;
    if (method->data.method.access_level != EXPRTK_ACCESS_PUBLIC) continue;
    if (method->data.method.arg_count != argc) continue;
    if (found) return NULL;
    found = method;
  }

  if (!found) return NULL;
  for (size_t i = 0; i < found->data.method.arg_count; ++i) {
    if (!found->data.method.arg_params[i] ||
        found->data.method.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
      return NULL;
    }
  }
  return ts_oop_inline_stmt_supported(c, class_name, found->data.method.body,
                                      found->data.method.arg_params,
                                      found->data.method.arg_count)
             ? found
             : NULL;
}

static MIR_reg_t ts_emit_oop_inline_method_expr(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name,
                                                exprtk_node_t **params, size_t param_count,
                                                const MIR_reg_t *arg_regs, exprtk_node_t *expr);
static MIR_reg_t ts_emit_oop_inline_method_stmt(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name, exprtk_node_t **params,
                                                size_t param_count, const MIR_reg_t *arg_regs,
                                                exprtk_node_t *stmt, int *returned);

static MIR_reg_t ts_emit_oop_inline_binary(ts_mir_compiler_t *c, const char *object_name,
                                           const char *class_name, exprtk_node_t **params,
                                           size_t param_count, const MIR_reg_t *arg_regs,
                                           exprtk_node_t *expr) {
  MIR_reg_t left;
  MIR_reg_t right;
  MIR_reg_t res;
  MIR_insn_code_t op;
  int is_cmp = 0;

  if (!expr->data.binary.left) {
    MIR_reg_t operand = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count,
                                                       arg_regs, expr->data.binary.right);
    if (expr->data.binary.op == exprtk_TOKEN_PLUS) return operand;
    res = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DSUB, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 0.0),
                                 MIR_new_reg_op(c->ctx, operand)));
    return res;
  }

  left = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count, arg_regs,
                                        expr->data.binary.left);
  right = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count, arg_regs,
                                         expr->data.binary.right);
  switch (expr->data.binary.op) {
  case exprtk_TOKEN_PLUS: op = MIR_DADD; break;
  case exprtk_TOKEN_MINUS: op = MIR_DSUB; break;
  case exprtk_TOKEN_MULTIPLY: op = MIR_DMUL; break;
  case exprtk_TOKEN_DIVIDE: op = MIR_DDIV; break;
  case exprtk_TOKEN_LT: op = MIR_DLT; is_cmp = 1; break;
  case exprtk_TOKEN_GT: op = MIR_DGT; is_cmp = 1; break;
  case exprtk_TOKEN_LE: op = MIR_DLE; is_cmp = 1; break;
  case exprtk_TOKEN_GE: op = MIR_DGE; is_cmp = 1; break;
  case exprtk_TOKEN_EQ: op = MIR_DEQ; is_cmp = 1; break;
  case exprtk_TOKEN_NE: op = MIR_DNE; is_cmp = 1; break;
  default: op = MIR_DADD; break;
  }

  if (is_cmp) {
    MIR_reg_t ireg = new_temp_ireg(c);
    res = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, ireg),
                                 MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_reg_op(c->ctx, ireg)));
    return res;
  }

  res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, res),
                               MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
  return res;
}

static MIR_reg_t ts_emit_oop_inline_method_expr(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name,
                                                exprtk_node_t **params, size_t param_count,
                                                const MIR_reg_t *arg_regs, exprtk_node_t *expr) {
  if (!expr || expr->type == EXPRTK_NODE_NULL) return ts_emit_zero_reg(c);

  switch (expr->type) {
  case EXPRTK_NODE_NUMBER: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, expr->data.number)));
    return r;
  }

  case EXPRTK_NODE_INTEGER: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, (double)expr->data.integer)));
    return r;
  }

  case EXPRTK_NODE_VARIABLE: {
    size_t idx = 0;
    if (ts_inline_param_index(params, param_count, expr->data.variable.name, &idx)) {
      return arg_regs[idx];
    }
    return ts_emit_zero_reg(c);
  }

  case EXPRTK_NODE_MEMBER_ACCESS:
    return ts_emit_oop_member_get(c, object_name, expr->data.member_access.member,
                                  expr->data.member_access.object);

  case EXPRTK_NODE_BINARY_OP:
    return ts_emit_oop_inline_binary(c, object_name, class_name, params, param_count, arg_regs,
                                     expr);

  case EXPRTK_NODE_SUPER: {
    const char *parent_name = ts_direct_parent_class_name(c, class_name);
    exprtk_node_t *method = NULL;
    MIR_reg_t super_arg_regs[16];
    int returned = 0;

    if (!parent_name || !expr->data.super_expr.is_call || !expr->data.super_expr.member ||
        expr->data.super_expr.arg_count > 16) {
      return ts_emit_zero_reg(c);
    }
    method = ts_find_inlineable_instance_method(c, parent_name, expr->data.super_expr.member,
                                                expr->data.super_expr.arg_count);
    if (!method) return ts_emit_zero_reg(c);

    for (size_t i = 0; i < expr->data.super_expr.arg_count; ++i) {
      super_arg_regs[i] = ts_emit_oop_inline_method_expr(
          c, object_name, class_name, params, param_count, arg_regs, expr->data.super_expr.args[i]);
    }

    return ts_emit_oop_inline_method_stmt(c, object_name, parent_name,
                                          method->data.method.arg_params,
                                          method->data.method.arg_count, super_arg_regs,
                                          method->data.method.body, &returned);
  }

  default:
    return ts_emit_zero_reg(c);
  }
}

static MIR_reg_t ts_emit_oop_inline_method_stmt(ts_mir_compiler_t *c, const char *object_name,
                                                const char *class_name, exprtk_node_t **params,
                                                size_t param_count, const MIR_reg_t *arg_regs,
                                                exprtk_node_t *stmt, int *returned) {
  MIR_reg_t last = 0;

  if (!stmt) return ts_emit_zero_reg(c);
  switch (stmt->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < stmt->data.block.count; ++i) {
      last = ts_emit_oop_inline_method_stmt(c, object_name, class_name, params, param_count, arg_regs,
                                            stmt->data.block.statements[i], returned);
      if (returned && *returned) return last;
    }
    return last ? last : ts_emit_zero_reg(c);

  case EXPRTK_NODE_FLOW:
    if (stmt->data.flow.type == exprtk_TOKEN_RETURN) {
      if (returned) *returned = 1;
      return ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count, arg_regs,
                                            stmt->data.flow.value);
    }
    return ts_emit_zero_reg(c);

  case EXPRTK_NODE_MEMBER_SET: {
    MIR_reg_t value = ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count,
                                                     arg_regs, stmt->data.member_set.value);
    return ts_emit_oop_member_set_reg(c, object_name, stmt->data.member_set.member,
                                      stmt->data.member_set.object, value);
  }

  default:
    return ts_emit_oop_inline_method_expr(c, object_name, class_name, params, param_count,
                                          arg_regs, stmt);
  }
}

static MIR_reg_t ts_try_emit_oop_inline_method_call(ts_mir_compiler_t *c,
                                                    const char *object_name,
                                                    const char *class_name,
                                                    const char *method_name, size_t argc,
                                                    const MIR_reg_t *arg_regs) {
  int returned = 0;
  exprtk_node_t *method =
      ts_find_inlineable_instance_method(c, class_name, method_name, argc);
  if (!method) return 0;

  return ts_emit_oop_inline_method_stmt(c, object_name, class_name, method->data.method.arg_params,
                                        method->data.method.arg_count, arg_regs,
                                        method->data.method.body, &returned);
}

static MIR_reg_t ts_emit_oop_instanceof(ts_mir_compiler_t *c, const char *object_name,
                                        const char *class_name) {
  MIR_reg_t res = new_temp_reg(c);
  ts_emit_sync_to_env(c);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.oop_instanceof_proto),
                        MIR_new_ref_op(c->ctx, c->ext.oop_instanceof_import),
                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)object_name),
                        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)class_name)));
  ts_emit_reload_from_env(c);
  return res;
}

static size_t ts_compile_call_args(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args,
                                   MIR_reg_t out_regs[16]) {
  if (argc > 16) argc = 16;
  for (size_t i = 0; i < argc; i++) {
    out_regs[i] = ts_compile_expr(c, args[i]);
  }
  return argc;
}

static MIR_reg_t ts_emit_compiled_func_call(ts_mir_compiler_t *c, ts_compiled_func_t *cf,
                                            size_t argc, const MIR_reg_t *arg_regs) {
  MIR_reg_t res = 0;
  size_t nops = 0;
  MIR_op_t ops[21];

  if (!c || !cf || cf->arg_count != argc) return 0;
  if (cf->has_closure_env && !c->ctx_reg) return 0;

  if (cf->has_closure_env) ts_emit_sync_to_env(c);

  res = new_temp_reg(c);
  ops[0] = MIR_new_ref_op(c->ctx, cf->proto);
  ops[1] = MIR_new_ref_op(c->ctx, cf->mir_func);
  ops[2] = MIR_new_reg_op(c->ctx, res);
  nops = 3;

  if (cf->has_closure_env) {
    ops[nops++] = MIR_new_reg_op(c->ctx, c->ctx_reg);
    if (c->closure_env_reg) {
      ops[nops++] = MIR_new_reg_op(c->ctx, c->closure_env_reg);
    } else {
      ops[nops++] = MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)&c->ts_ctx->env);
    }
  }

  for (size_t i = 0; i < argc; ++i) {
    ops[nops++] = MIR_new_reg_op(c->ctx, arg_regs[i]);
  }
  MIR_append_insn(c->ctx, c->func, MIR_new_insn_arr(c->ctx, MIR_CALL, nops, ops));
  return res;
}

static MIR_reg_t ts_try_emit_hof_specialized_call(ts_mir_compiler_t *c,
                                                  exprtk_node_t *call_node) {
  int numeric_arg_indices[16];
  size_t numeric_arg_count = 0;
  ts_compiled_func_t *cf =
      ts_find_hof_specialized_call(c, call_node, numeric_arg_indices, &numeric_arg_count);
  if (!cf || cf->arg_count != numeric_arg_count) return 0;

  MIR_reg_t arg_regs[16];
  for (size_t i = 0; i < numeric_arg_count; ++i) {
    arg_regs[i] = ts_compile_expr(c, call_node->data.function.args[numeric_arg_indices[i]]);
  }

  return ts_emit_compiled_func_call(c, cf, numeric_arg_count, arg_regs);
}

static MIR_reg_t ts_emit_index_access(ts_mir_compiler_t *c, const char *name,
                                      exprtk_node_t *index_node) {
  MIR_reg_t idx_reg = ts_compile_expr(c, index_node);

  /* Native fast path for vectors pre-bound in env at compile time. */
  exprtk_value_t existing = exprtk_env_get(&c->ts_ctx->env, name);
  int is_prebound = (existing.type == EXPRTK_VAL_VECTOR && existing.data.vector.data != NULL);
  if (is_prebound) {
    MIR_reg_t ptr_reg = ts_mir_get_or_add_vec_ptr(c, name);

    if (ptr_reg) {
      MIR_reg_t idx_i = new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_D2I, MIR_new_reg_op(c->ctx, idx_i),
                                   MIR_new_reg_op(c->ctx, idx_reg)));

      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, idx_i, 8)));
      return res;
    }
  }

  /* Runtime helper for env-owned vectors. */
  MIR_reg_t res = new_temp_reg(c);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.vec_get_proto),
                                    MIR_new_ref_op(c->ctx, c->ext.vec_get_import),
                                    MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                                    MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)name),
                                    MIR_new_reg_op(c->ctx, idx_reg)));
  return res;
}

/* --- Assignment helpers --- */
static int ts_is_non_numeric_node(exprtk_node_t *node) {
  if (!node) return 0;
  return node->type == EXPRTK_NODE_VECTOR || node->type == EXPRTK_NODE_MAP_LITERAL ||
         node->type == EXPRTK_NODE_STRING || node->type == EXPRTK_NODE_TEMPLATE_STRING ||
         node->type == EXPRTK_NODE_SLICE || node->type == EXPRTK_NODE_FUNCTION_EXPRESSION;
}

static int ts_expr_contains_value_call(ts_mir_compiler_t *c, exprtk_node_t *node);

static int ts_member_call_is_stream_chain(exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_MEMBER_CALL) return 0;
  if (node->data.member_call.method &&
      strcmp(node->data.member_call.method, "stream") == 0) {
    return 1;
  }
  if (node->data.member_call.object &&
      node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
      node->data.member_call.object->data.variable.name &&
      strcmp(node->data.member_call.object->data.variable.name, "stream") == 0 &&
      node->data.member_call.method &&
      (strcmp(node->data.member_call.method, "of") == 0 ||
       strcmp(node->data.member_call.method, "file") == 0 ||
       strcmp(node->data.member_call.method, "lines") == 0 ||
       strcmp(node->data.member_call.method, "text") == 0 ||
       strcmp(node->data.member_call.method, "json") == 0 ||
       strcmp(node->data.member_call.method, "xml") == 0 ||
       strcmp(node->data.member_call.method, "csv") == 0)) {
    return 1;
  }
  return ts_member_call_is_stream_chain(node->data.member_call.object);
}

static int ts_call_has_spread_args(size_t argc, exprtk_node_t **args) {
  if (!args) return 0;
  for (size_t i = 0; i < argc; ++i) {
    if (args[i] && args[i]->type == EXPRTK_NODE_SPREAD) return 1;
  }
  return 0;
}

static int ts_call_args_need_ast_eval(ts_mir_compiler_t *c, size_t argc, exprtk_node_t **args) {
  if (!args) return 0;
  for (size_t i = 0; i < argc; ++i) {
    exprtk_node_t *arg = args[i];
    if (!arg) continue;
    if (arg->type == EXPRTK_NODE_SPREAD || ts_is_non_numeric_node(arg) ||
        ts_oop_arg_needs_value_bridge(c, arg) || ts_expr_contains_value_call(c, arg)) {
      return 1;
    }
  }
  return 0;
}

static int ts_name_starts_with(const char *name, const char *prefix) {
  size_t prefix_len = 0;
  if (!name || !prefix) return 0;
  prefix_len = strlen(prefix);
  return strncmp(name, prefix, prefix_len) == 0;
}

static int ts_call_name_needs_value_eval(const char *name) {
  if (!name) return 0;
  return ts_name_starts_with(name, "mat_") ||
         ts_name_starts_with(name, "matrix.") ||
         ts_name_starts_with(name, "linalg.") ||
         ts_name_starts_with(name, "table.") ||
         ts_name_starts_with(name, "json.") ||
         ts_name_starts_with(name, "xml.") ||
         ts_name_starts_with(name, "csv.") ||
         ts_name_starts_with(name, "parser.json_") ||
         ts_name_starts_with(name, "parser.xml_") ||
         ts_name_starts_with(name, "parser.csv_") ||
         ts_name_starts_with(name, "vec.") ||
         ts_name_starts_with(name, "vector_") ||
         ts_name_starts_with(name, "str_") ||
         ts_name_starts_with(name, "string.") ||
         strcmp(name, "stats.t_test_1samp") == 0;
}

static int ts_env_has_func(exprtk_env_t *env, const char *name) {
  for (exprtk_env_t *curr_env = env; curr_env; curr_env = curr_env->parent) {
    for (exprtk_func_t *f = curr_env->funcs; f; f = f->next) {
      if (f->name && strcmp(f->name, name) == 0) return 1;
    }
  }
  return 0;
}

static int ts_is_core_compat_namespace(const char *name, size_t len) {
  static const char *const names[] = {"math", "stats", "string", "io", "core", "regex", NULL};
  for (size_t i = 0; names[i]; ++i) {
    if (strlen(names[i]) == len && strncmp(names[i], name, len) == 0) return 1;
  }
  return 0;
}

static int ts_resolves_core_compat_func(const char *name) {
  const char *dot = name ? strchr(name, '.') : NULL;
  if (!dot || !ts_is_core_compat_namespace(name, (size_t)(dot - name))) return 0;
  return exprtk_registry_find(dot + 1) != NULL;
}

static int ts_resolves_runtime_func(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  return ts_env_has_func(&c->ts_ctx->env, name) ||
         exprtk_find_builtin(name, &c->ts_ctx->env) != NULL ||
         ts_resolves_core_compat_func(name);
}

static int ts_runtime_call_needs_value_eval(ts_mir_compiler_t *c, const char *name,
                                            size_t argc) {
  MIR_item_t proto = NULL;
  MIR_item_t import = NULL;
  if (!c || !name) return 0;
  if (ts_find_math_dispatch(c, name, argc, &proto, &import)) return 0;
  return ts_call_name_needs_value_eval(name) || ts_resolves_runtime_func(c, name);
}

static int ts_expr_list_contains_value_call(ts_mir_compiler_t *c, size_t count,
                                            exprtk_node_t **nodes);

static int ts_index_constant(exprtk_node_t *node, int *out) {
  if (!node || !out) return 0;
  if (node->type == EXPRTK_NODE_INTEGER) {
    *out = (int)node->data.integer;
    return 1;
  }
  if (node->type == EXPRTK_NODE_NUMBER) {
    *out = (int)node->data.number;
    return node->data.number == (double)*out;
  }
  return 0;
}

static int ts_expr_contains_value_call(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    return ts_mir_var_is_dynamic(c, node->data.variable.name);

  case EXPRTK_NODE_FUNCTION_CALL:
    if (node->data.function.name && strcmp(node->data.function.name, "await") == 0) {
      return 0;
    }
    if (ts_runtime_call_needs_value_eval(c, node->data.function.name,
                                         node->data.function.arg_count)) {
      return 1;
    }
    if (ts_call_name_needs_value_eval(node->data.function.name)) return 1;
    return ts_call_args_need_ast_eval(c, node->data.function.arg_count,
                                      node->data.function.args) ||
           ts_expr_list_contains_value_call(c, node->data.function.arg_count,
                                            node->data.function.args);

  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_member_call_is_stream_chain(node)) return 1;
    if (node->data.member_call.object &&
        node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        ts_mir_var_is_dynamic(c, node->data.member_call.object->data.variable.name)) {
      return 1;
    }
    if (node->data.member_call.object &&
        node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_call.object->data.variable.name &&
        node->data.member_call.method) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               node->data.member_call.object->data.variable.name,
               node->data.member_call.method);
      if (ts_resolves_runtime_func(c, full_name)) return 1;
      if (ts_call_name_needs_value_eval(full_name)) return 1;
    }
    return ts_expr_contains_value_call(c, node->data.member_call.object) ||
           ts_call_args_need_ast_eval(c, node->data.member_call.arg_count,
                                      node->data.member_call.args) ||
           ts_expr_list_contains_value_call(c, node->data.member_call.arg_count,
                                            node->data.member_call.args);

  case EXPRTK_NODE_BINARY_OP:
    if (ts_binary_is_null_eq_compare(node)) return 1;
    return ts_expr_contains_value_call(c, node->data.binary.left) ||
           ts_expr_contains_value_call(c, node->data.binary.right);

  case EXPRTK_NODE_INDEX:
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VARIABLE) {
      if (ts_mir_var_is_dynamic(c, node->data.index_access.array->data.variable.name)) {
        return 1;
      }
      return ts_expr_contains_value_call(c, node->data.index_access.index);
    }
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VECTOR) {
      int idx = 0;
      if (!ts_index_constant(node->data.index_access.index, &idx)) return 1;
      if (idx < 0 || (size_t)idx >= node->data.index_access.array->data.vector.count) return 1;
      return ts_expr_contains_value_call(
          c, node->data.index_access.array->data.vector.elements[idx]);
    }
    return 1;

  case EXPRTK_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object &&
        node->data.member_access.object->type == EXPRTK_NODE_VARIABLE &&
        ts_mir_var_is_dynamic(c, node->data.member_access.object->data.variable.name)) {
      return 1;
    }
    return ts_expr_contains_value_call(c, node->data.member_access.object);

  case EXPRTK_NODE_IF:
    return ts_expr_contains_value_call(c, node->data.if_stmt.condition) ||
           ts_expr_contains_value_call(c, node->data.if_stmt.if_branch) ||
           ts_expr_contains_value_call(c, node->data.if_stmt.else_branch);

  default:
    return 0;
  }
}

static int ts_expr_list_contains_value_call(ts_mir_compiler_t *c, size_t count,
                                            exprtk_node_t **nodes) {
  if (!nodes) return 0;
  for (size_t i = 0; i < count; ++i) {
    if (ts_expr_contains_value_call(c, nodes[i])) return 1;
  }
  return 0;
}

static int ts_index_assignment_needs_value_eval(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_value_t existing;
  if (!c || !node || node->type != EXPRTK_NODE_INDEX) return 0;

  if (node->data.index_access.array &&
      node->data.index_access.array->type == EXPRTK_NODE_VECTOR) {
    int idx = 0;
    if (!ts_index_constant(node->data.index_access.index, &idx)) return 1;
    if (idx < 0 || (size_t)idx >= node->data.index_access.array->data.vector.count) return 1;
    return ts_expr_contains_value_call(c,
        node->data.index_access.array->data.vector.elements[idx]);
  }

  if (!node->data.index_access.array ||
      node->data.index_access.array->type != EXPRTK_NODE_VARIABLE) {
    return 1;
  }
  if (ts_mir_var_is_dynamic(c, node->data.index_access.array->data.variable.name)) {
    return 1;
  }

  existing = exprtk_env_get(&c->ts_ctx->env,
                            node->data.index_access.array->data.variable.name);
  return existing.type != EXPRTK_VAL_VECTOR;
}

static exprtk_node_t *ts_find_map_literal_member(exprtk_node_t *map_node, const char *member) {
  if (!map_node || map_node->type != EXPRTK_NODE_MAP_LITERAL || !member) return NULL;

  for (size_t i = 0; i < map_node->data.map_literal.count; ++i) {
    if (map_node->data.map_literal.keys[i] &&
        strcmp(map_node->data.map_literal.keys[i], member) == 0) {
      return map_node->data.map_literal.values[i];
    }
  }
  return NULL;
}

static void ts_register_destructure_targets(ts_mir_compiler_t *c, exprtk_node_t *target) {
  if (!c || !target) return;

  switch (target->type) {
  case EXPRTK_NODE_VARIABLE:
    (void)get_or_create_reg(c, target->data.variable.name);
    break;

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < target->data.vector.count; ++i) {
      ts_register_destructure_targets(c, target->data.vector.elements[i]);
    }
    break;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < target->data.map_literal.count; ++i) {
      ts_register_destructure_targets(c, target->data.map_literal.values[i]);
    }
    break;

  case EXPRTK_NODE_SPREAD:
    ts_register_destructure_targets(c, target->data.spread.child);
    break;

  case EXPRTK_NODE_ASSIGNMENT:
    (void)get_or_create_reg(c, target->data.assignment.name);
    break;

  default:
    break;
  }
}

static void ts_mark_destructure_targets_dynamic(ts_mir_compiler_t *c, exprtk_node_t *target) {
  if (!c || !target) return;

  switch (target->type) {
  case EXPRTK_NODE_VARIABLE:
    ts_mir_mark_var_dynamic(c, target->data.variable.name);
    ts_mir_mark_var_clean(c, target->data.variable.name);
    break;

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < target->data.vector.count; ++i) {
      ts_mark_destructure_targets_dynamic(c, target->data.vector.elements[i]);
    }
    break;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < target->data.map_literal.count; ++i) {
      ts_mark_destructure_targets_dynamic(c, target->data.map_literal.values[i]);
    }
    break;

  case EXPRTK_NODE_SPREAD:
    ts_mark_destructure_targets_dynamic(c, target->data.spread.child);
    break;

  case EXPRTK_NODE_ASSIGNMENT:
    ts_mir_mark_var_dynamic(c, target->data.assignment.name);
    ts_mir_mark_var_clean(c, target->data.assignment.name);
    break;

  default:
    break;
  }
}

static MIR_reg_t ts_emit_destructure_assignment(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_node_t *value;
  MIR_reg_t res;

  if (!c || !node || node->type != EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT ||
      !node->data.destructuring.targets || !node->data.destructuring.value) {
    return 0;
  }

  value = node->data.destructuring.value;
  ts_register_destructure_targets(c, node->data.destructuring.targets);
  res = new_temp_reg(c);

  if (value->type == EXPRTK_NODE_VARIABLE && value->data.variable.name) {
    ts_emit_sync_to_env(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 7, MIR_new_ref_op(c->ctx, c->ext.destruct_var_proto),
                          MIR_new_ref_op(c->ctx, c->ext.destruct_var_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node->data.destructuring.targets),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)value->data.variable.name),
                          MIR_new_int_op(c->ctx, (int64_t)node->data.destructuring.is_constant)));
    ts_emit_reload_from_env(c);
    ts_mark_destructure_targets_dynamic(c, node->data.destructuring.targets);
    return res;
  }

  if (value->type == EXPRTK_NODE_VECTOR && value->data.vector.count <= 16 &&
      !ts_vector_literal_needs_value_eval(c, value)) {
    MIR_reg_t element_regs[16];
    MIR_reg_t arr_reg;
    size_t count = value->data.vector.count;

    for (size_t i = 0; i < count; ++i) {
      element_regs[i] = ts_compile_expr(c, value->data.vector.elements[i]);
    }
    arr_reg = ts_emit_packed_args(c, count, element_regs);

    ts_emit_sync_to_env(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 8, MIR_new_ref_op(c->ctx, c->ext.destruct_vector_proto),
                          MIR_new_ref_op(c->ctx, c->ext.destruct_vector_import),
                          MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node->data.destructuring.targets),
                          MIR_new_int_op(c->ctx, (int64_t)node->data.destructuring.is_constant),
                          MIR_new_int_op(c->ctx, (int64_t)count), MIR_new_reg_op(c->ctx, arr_reg)));
    ts_emit_reload_from_env(c);
    ts_mark_destructure_targets_dynamic(c, node->data.destructuring.targets);
    return res;
  }

  return 0;
}

static void ts_register_helper_written_vars(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    (void)get_or_create_reg(c, node->data.assignment.name);
    ts_register_helper_written_vars(c, node->data.assignment.value);
    break;

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    ts_register_destructure_targets(c, node->data.destructuring.targets);
    ts_register_helper_written_vars(c, node->data.destructuring.value);
    break;

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      ts_register_helper_written_vars(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_IF:
    ts_register_helper_written_vars(c, node->data.if_stmt.if_branch);
    ts_register_helper_written_vars(c, node->data.if_stmt.else_branch);
    break;

  case EXPRTK_NODE_TRY_CATCH:
    ts_register_helper_written_vars(c, node->data.try_catch.try_body);
    ts_register_helper_written_vars(c, node->data.try_catch.catch_body);
    if (node->data.try_catch.catch_var) {
      (void)get_or_create_reg(c, node->data.try_catch.catch_var);
    }
    break;

  default:
    break;
  }
}

static void ts_prescan_variables(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    if (node->data.variable.name) (void)get_or_create_reg(c, node->data.variable.name);
    break;

  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    if (node->data.assignment.name) (void)get_or_create_reg(c, node->data.assignment.name);
    ts_prescan_variables(c, node->data.assignment.value);
    break;

  case EXPRTK_NODE_BINARY_OP:
    ts_prescan_variables(c, node->data.binary.left);
    ts_prescan_variables(c, node->data.binary.right);
    break;

  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      ts_prescan_variables(c, node->data.function.args[i]);
    }
    break;

  case EXPRTK_NODE_MEMBER_CALL:
    ts_prescan_variables(c, node->data.member_call.object);
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      ts_prescan_variables(c, node->data.member_call.args[i]);
    }
    break;

  case EXPRTK_NODE_MEMBER_ACCESS:
    ts_prescan_variables(c, node->data.member_access.object);
    break;

  case EXPRTK_NODE_MEMBER_SET:
    ts_prescan_variables(c, node->data.member_set.object);
    ts_prescan_variables(c, node->data.member_set.value);
    break;

  case EXPRTK_NODE_INDEX:
    ts_prescan_variables(c, node->data.index_access.array);
    ts_prescan_variables(c, node->data.index_access.index);
    break;

  case EXPRTK_NODE_SLICE:
    ts_prescan_variables(c, node->data.slice.array);
    ts_prescan_variables(c, node->data.slice.start);
    ts_prescan_variables(c, node->data.slice.end);
    break;

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i) {
      ts_prescan_variables(c, node->data.vector.elements[i]);
    }
    break;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      ts_prescan_variables(c, node->data.map_literal.values[i]);
    }
    break;

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      ts_prescan_variables(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_IF:
    ts_prescan_variables(c, node->data.if_stmt.condition);
    ts_prescan_variables(c, node->data.if_stmt.if_branch);
    ts_prescan_variables(c, node->data.if_stmt.else_branch);
    break;

  case EXPRTK_NODE_WHILE:
    ts_prescan_variables(c, node->data.while_loop.condition);
    ts_prescan_variables(c, node->data.while_loop.body);
    break;

  case EXPRTK_NODE_FOR:
    ts_prescan_variables(c, node->data.for_loop.init);
    ts_prescan_variables(c, node->data.for_loop.condition);
    ts_prescan_variables(c, node->data.for_loop.post);
    ts_prescan_variables(c, node->data.for_loop.body);
    break;

  case EXPRTK_NODE_DO_WHILE:
    ts_prescan_variables(c, node->data.do_while.body);
    ts_prescan_variables(c, node->data.do_while.condition);
    break;

  case EXPRTK_NODE_FOR_IN:
    if (node->data.for_in.var_name) (void)get_or_create_reg(c, node->data.for_in.var_name);
    ts_prescan_variables(c, node->data.for_in.collection);
    ts_prescan_variables(c, node->data.for_in.body);
    break;

  case EXPRTK_NODE_FLOW:
    ts_prescan_variables(c, node->data.flow.value);
    break;

  case EXPRTK_NODE_SWITCH:
    ts_prescan_variables(c, node->data.switch_stmt.value);
    for (size_t i = 0; i < node->data.switch_stmt.case_count; ++i) {
      ts_prescan_variables(c, node->data.switch_stmt.cases[i]);
    }
    ts_prescan_variables(c, node->data.switch_stmt.default_case);
    break;

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    ts_register_destructure_targets(c, node->data.destructuring.targets);
    ts_prescan_variables(c, node->data.destructuring.value);
    break;

  case EXPRTK_NODE_TRY_CATCH:
    ts_prescan_variables(c, node->data.try_catch.try_body);
    if (node->data.try_catch.catch_var) (void)get_or_create_reg(c, node->data.try_catch.catch_var);
    ts_prescan_variables(c, node->data.try_catch.catch_body);
    break;

  case EXPRTK_NODE_THROW:
    ts_prescan_variables(c, node->data.throw_stmt.value);
    break;

  case EXPRTK_NODE_SPREAD:
    ts_prescan_variables(c, node->data.spread.child);
    break;

  case EXPRTK_NODE_NEW:
    for (size_t i = 0; i < node->data.new_expr.arg_count; ++i) {
      ts_prescan_variables(c, node->data.new_expr.args[i]);
    }
    break;

  case EXPRTK_NODE_INSTANCEOF:
    ts_prescan_variables(c, node->data.instanceof_expr.object);
    ts_prescan_variables(c, node->data.instanceof_expr.class_expr);
    break;

  case EXPRTK_NODE_FUNCTION_DEFINITION:
  case EXPRTK_NODE_FUNCTION_EXPRESSION:
  case EXPRTK_NODE_GENERATOR_FUNCTION:
  case EXPRTK_NODE_CLASS_DEF:
  case EXPRTK_NODE_METHOD:
    break;

  default:
    break;
  }
}

static int ts_oop_receiver_can_assign_temp(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    return 1;

  case EXPRTK_NODE_NEW:
    return node->data.new_expr.class_name != NULL;

  case EXPRTK_NODE_FUNCTION_CALL:
    return node->data.function.name &&
           !ts_call_has_spread_args(node->data.function.arg_count, node->data.function.args) &&
           (ts_mir_is_known_class_name(c, node->data.function.name) ||
            ts_call_assignment_needs_value_bridge(c, node->data.function.name,
                                                  node->data.function.arg_count));

  case EXPRTK_NODE_MEMBER_CALL:
    return node->data.member_call.object && node->data.member_call.method &&
           !ts_expr_contains_value_call(c, node->data.member_call.object) &&
           !ts_expr_list_contains_value_call(c, node->data.member_call.arg_count,
                                             node->data.member_call.args) &&
           ts_oop_receiver_can_assign_temp(c, node->data.member_call.object);

  case EXPRTK_NODE_MEMBER_ACCESS:
    return node->data.member_access.object && node->data.member_access.member &&
           !ts_expr_contains_value_call(c, node->data.member_access.object) &&
           ts_oop_receiver_can_assign_temp(c, node->data.member_access.object);

  default:
    return 0;
  }
}

static MIR_reg_t ts_emit_assignment(ts_mir_compiler_t *c, exprtk_node_t *node) {
  exprtk_node_t *rhs = node->data.assignment.value;
  if (rhs && rhs->type == EXPRTK_NODE_NEW && rhs->data.new_expr.class_name) {
    MIR_reg_t res = ts_emit_oop_new_assign(c, node->data.assignment.name, rhs);
    ts_mir_set_var_class(c, node->data.assignment.name, rhs->data.new_expr.class_name);
    return res;
  }
  if (rhs && rhs->type == EXPRTK_NODE_FUNCTION_CALL && rhs->data.function.name &&
      ts_mir_is_known_class_name(c, rhs->data.function.name)) {
    MIR_reg_t res = ts_emit_oop_class_call_assign(c, node->data.assignment.name, rhs);
    ts_mir_set_var_class(c, node->data.assignment.name, rhs->data.function.name);
    return res;
  }
  if (rhs && rhs->type == EXPRTK_NODE_FUNCTION_CALL && rhs->data.function.name) {
    MIR_reg_t hof_res = ts_try_emit_hof_specialized_call(c, rhs);
    if (hof_res) {
      MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
      ts_mir_clear_var_class(c, node->data.assignment.name);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, hof_res)));
      ts_mir_mark_var_dirty(c, node->data.assignment.name);
      ts_mir_mark_var_numeric(c, node->data.assignment.name);
      return target;
    }
    if (strcmp(rhs->data.function.name, "await") == 0) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_await_assign(c, node->data.assignment.name,
                                  rhs->data.function.arg_count > 0 ? rhs->data.function.args[0]
                                                                   : NULL);
    }
    if (ts_call_args_need_ast_eval(c, rhs->data.function.arg_count, rhs->data.function.args)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_call_value_assign(c, node->data.assignment.name, rhs->data.function.name, rhs);
    }
    if (ts_runtime_call_needs_value_eval(c, rhs->data.function.name,
                                         rhs->data.function.arg_count)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    if (ts_call_assignment_needs_value_bridge(c, rhs->data.function.name,
                                              rhs->data.function.arg_count)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_runtime_call_assign(c, node->data.assignment.name, rhs->data.function.name,
                                         rhs->data.function.arg_count, rhs->data.function.args);
    }
  }
  if (rhs && rhs->type == EXPRTK_NODE_MEMBER_CALL && rhs->data.member_call.object &&
      rhs->data.member_call.method) {
    if (ts_member_call_is_stream_chain(rhs)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
    }
    if (rhs->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        ts_mir_var_is_dynamic(c, rhs->data.member_call.object->data.variable.name)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_oop_member_call_assign_value(
          c, node->data.assignment.name, rhs->data.member_call.object->data.variable.name,
          rhs->data.member_call.method, rhs->data.member_call.object, rhs);
    }
    if (ts_expr_contains_value_call(c, rhs->data.member_call.object)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    if (rhs->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        rhs->data.member_call.object->data.variable.name) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               rhs->data.member_call.object->data.variable.name, rhs->data.member_call.method);
      if (ts_resolves_runtime_func(c, full_name) || ts_call_name_needs_value_eval(full_name)) {
        char *full_name_copy = strdup(full_name);
        if (!full_name_copy) {
          ts_mir_fail(c, "JIT compile error: out of memory duplicating function name '%s'",
                      full_name);
          return ts_emit_zero_reg(c);
        }
        ts_mir_clear_var_class(c, node->data.assignment.name);
        return ts_emit_call_value_assign(c, node->data.assignment.name, full_name_copy, rhs);
      }
    }
    if (ts_call_args_need_ast_eval(c, rhs->data.member_call.arg_count,
                                   rhs->data.member_call.args) &&
        !ts_oop_call_args_need_value_bridge(c, rhs->data.member_call.arg_count,
                                            rhs->data.member_call.args)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, rhs->data.member_call.object);
    if (!obj_name) return ts_emit_unsupported_node(c, rhs);

    if (ts_oop_call_args_need_value_bridge(c, rhs->data.member_call.arg_count,
                                           rhs->data.member_call.args)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_oop_member_call_assign_value(c, node->data.assignment.name, obj_name,
                                                  rhs->data.member_call.method,
                                                  rhs->data.member_call.object, rhs);
    }

    MIR_reg_t arg_regs[16];
    size_t argc =
        ts_compile_call_args(c, rhs->data.member_call.arg_count, rhs->data.member_call.args,
                             arg_regs);
    const char *exact_class = ts_mir_find_var_class(c, obj_name);
    if (exact_class) {
      MIR_reg_t inline_res =
          ts_try_emit_oop_inline_method_call(c, obj_name, exact_class,
                                             rhs->data.member_call.method, argc, arg_regs);
      if (inline_res) {
        MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
        ts_mir_clear_var_class(c, node->data.assignment.name);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                     MIR_new_reg_op(c->ctx, inline_res)));
        ts_mir_mark_var_dirty(c, node->data.assignment.name);
        ts_mir_mark_var_numeric(c, node->data.assignment.name);
        return target;
      }
    }
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_oop_member_call_assign(c, node->data.assignment.name, obj_name,
                                          rhs->data.member_call.method,
                                          rhs->data.member_call.object, argc, arg_regs);
  }
  if (rhs && rhs->type == EXPRTK_NODE_VARIABLE && rhs->data.variable.name &&
      ts_mir_is_known_class_name(c, rhs->data.variable.name)) {
    MIR_reg_t res =
        ts_emit_oop_class_alias(c, node->data.assignment.name, rhs->data.variable.name);
    ts_mir_add_class_name(c, node->data.assignment.name);
    return res;
  }
  if (rhs && rhs->type == EXPRTK_NODE_VARIABLE && rhs->data.variable.name &&
      ts_mir_var_is_dynamic(c, rhs->data.variable.name)) {
    ts_mir_clear_var_class(c, node->data.assignment.name);
    (void)get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_dynamic_var_assign(c, node->data.assignment.name, rhs->data.variable.name);
  }
  if (rhs && rhs->type == EXPRTK_NODE_MEMBER_ACCESS && rhs->data.member_access.object &&
      rhs->data.member_access.member) {
    if (rhs->data.member_access.object->type == EXPRTK_NODE_MAP_LITERAL) {
      exprtk_node_t *value =
          ts_find_map_literal_member(rhs->data.member_access.object, rhs->data.member_access.member);
      if (!value) return ts_emit_unsupported_node(c, rhs);
      MIR_reg_t val = ts_compile_expr(c, value);
      MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
      ts_mir_clear_var_class(c, node->data.assignment.name);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, val)));
      ts_mir_mark_var_dirty(c, node->data.assignment.name);
      ts_mir_mark_var_numeric(c, node->data.assignment.name);
      return target;
    }
    if (ts_expr_contains_value_call(c, rhs->data.member_access.object)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
    }
    if (!ts_oop_receiver_can_assign_temp(c, rhs->data.member_access.object)) {
      ts_mir_clear_var_class(c, node->data.assignment.name);
      (void)get_or_create_reg(c, node->data.assignment.name);
      return ts_emit_runtime_value_node(c, node);
    }
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, rhs->data.member_access.object);
    if (!obj_name) return ts_emit_unsupported_node(c, rhs);
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_member_access_assign(c, node->data.assignment.name, obj_name,
                                        rhs->data.member_access.member,
                                        rhs->data.member_access.object);
  }

  ts_mir_clear_var_class(c, node->data.assignment.name);

  if (rhs && rhs->type == EXPRTK_NODE_STRING) {
    MIR_reg_t res = ts_emit_string_literal_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    (void)get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_TEMPLATE_STRING) {
    MIR_reg_t res = ts_emit_template_string_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    (void)get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_FUNCTION_EXPRESSION) {
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_function_expr_assign(c, node->data.assignment.name, rhs);
  }

  if (rhs && rhs->type == EXPRTK_NODE_TRY_CATCH) {
    ts_mir_clear_var_class(c, node->data.assignment.name);
    return ts_emit_try_catch_assign(c, node->data.assignment.name, rhs);
  }

  if (rhs && rhs->type == EXPRTK_NODE_NULL) {
    (void)get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_VECTOR) {
    MIR_reg_t res = ts_emit_vector_literal_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    (void)get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (rhs && rhs->type == EXPRTK_NODE_MAP_LITERAL) {
    MIR_reg_t res = ts_emit_map_literal_assign(c, node->data.assignment.name, rhs);
    if (res) return res;
    return ts_emit_map_value_literal_assign(c, node->data.assignment.name, rhs);
  }

  if (ts_is_non_numeric_node(rhs)) {
    (void)get_or_create_reg(c, node->data.assignment.name);
    return ts_emit_runtime_value_node(c, node);
  }

  if (ts_expr_contains_value_call(c, rhs)) {
    return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
  }

  if (ts_index_assignment_needs_value_eval(c, rhs)) {
    return ts_emit_value_expr_assign(c, node->data.assignment.name, rhs);
  }

  MIR_reg_t val = ts_compile_expr(c, rhs);
  MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
  MIR_append_insn(
      c->ctx, c->func,
      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, val)));
  ts_mir_mark_var_dirty(c, node->data.assignment.name);  // 标记为脏
  ts_mir_mark_var_numeric(c, node->data.assignment.name);
  return target;
}

static int ts_compile_data_access_and_assignment(ts_mir_compiler_t *c, exprtk_node_t *node,
                                                 MIR_reg_t *out) {
  if (!node || !out) return 0;

  switch (node->type) {
  case EXPRTK_NODE_ASSIGNMENT:
    *out = ts_emit_assignment(c, node);
    return 1;

  case EXPRTK_NODE_CONSTANT_DECL: {
    exprtk_node_t *value = node->data.assignment.value;
    if (value && value->type == EXPRTK_NODE_NULL) {
      (void)get_or_create_reg(c, node->data.assignment.name);
      *out = ts_emit_runtime_value_node(c, node);
      return 1;
    }
    if (value && value->type == EXPRTK_NODE_STRING) {
      MIR_reg_t res = ts_emit_string_literal_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (value && value->type == EXPRTK_NODE_TEMPLATE_STRING) {
      MIR_reg_t res = ts_emit_template_string_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (value && value->type == EXPRTK_NODE_VARIABLE && value->data.variable.name &&
        ts_mir_var_is_dynamic(c, value->data.variable.name)) {
      (void)get_or_create_reg(c, node->data.assignment.name);
      *out = ts_emit_dynamic_var_assign(c, node->data.assignment.name, value->data.variable.name);
      return 1;
    }
    if (value && value->type == EXPRTK_NODE_VECTOR) {
      MIR_reg_t res = ts_emit_vector_literal_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (value && value->type == EXPRTK_NODE_MAP_LITERAL) {
      MIR_reg_t res = ts_emit_map_literal_assign(c, node->data.assignment.name, value);
      if (res) {
        *out = res;
        return 1;
      }
    }
    if (ts_is_non_numeric_node(value) || ts_expr_contains_value_call(c, value) ||
        ts_index_assignment_needs_value_eval(c, value)) {
      (void)get_or_create_reg(c, node->data.assignment.name);
      *out = ts_emit_runtime_value_node(c, node);
      return 1;
    }
    MIR_reg_t val = ts_compile_expr(c, value);
    MIR_reg_t target = get_or_create_reg(c, node->data.assignment.name);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, target),
                                 MIR_new_reg_op(c->ctx, val)));
    ts_mir_mark_var_dirty(c, node->data.assignment.name);  // 标记为脏
    ts_mir_mark_var_numeric(c, node->data.assignment.name);
    *out = target;
    return 1;
  }

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    *out = ts_emit_destructure_assignment(c, node);
    if (!*out) {
      ts_register_destructure_targets(c, node->data.destructuring.targets);
      *out = ts_emit_runtime_value_node(c, node);
    }
    return 1;

  case EXPRTK_NODE_NULL: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(
        c->ctx, c->func,
        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r), MIR_new_double_op(c->ctx, 0.0)));
    *out = r;
    return 1;
  }

  case EXPRTK_NODE_INDEX:
    if (node->data.index_access.array &&
        node->data.index_access.array->type == EXPRTK_NODE_VARIABLE && c->ctx_reg) {
      const char *name = node->data.index_access.array->data.variable.name;
      *out = ts_emit_index_access(c, name, node->data.index_access.index);
    } else if (node->data.index_access.array &&
               node->data.index_access.array->type == EXPRTK_NODE_VECTOR) {
      int idx = 0;
      if (!ts_index_constant(node->data.index_access.index, &idx) || idx < 0 ||
          (size_t)idx >= node->data.index_access.array->data.vector.count) {
        *out = ts_emit_unsupported_node(c, node);
      } else {
        *out = ts_compile_expr(c, node->data.index_access.array->data.vector.elements[idx]);
      }
    } else {
      *out = ts_emit_runtime_value_node(c, node);
    }
    return 1;

  case EXPRTK_NODE_MEMBER_ACCESS:
    if (node->data.member_access.object && node->data.member_access.member) {
      if (node->data.member_access.object->type == EXPRTK_NODE_MAP_LITERAL) {
        exprtk_node_t *value =
            ts_find_map_literal_member(node->data.member_access.object,
                                       node->data.member_access.member);
        *out = value ? ts_compile_expr(c, value) : ts_emit_unsupported_node(c, node);
        return 1;
      }
      if (ts_expr_contains_value_call(c, node->data.member_access.object) ||
          !ts_oop_receiver_can_assign_temp(c, node->data.member_access.object)) {
        *out = ts_emit_runtime_value_node(c, node);
        return 1;
      }
      const char *obj_name = ts_emit_oop_receiver_to_temp(c, node->data.member_access.object);
      const char *member = node->data.member_access.member;
      *out = obj_name ? ts_emit_member_access(c, obj_name, member, node->data.member_access.object)
                      : ts_emit_runtime_value_node(c, node);
    } else {
      *out = ts_emit_unsupported_node(c, node);
    }
    return 1;

  default:
    return 0;
  }
}

/* =========================================================================
 *  Lookup compiled function by name
 * ========================================================================= */

static ts_compiled_func_t *ts_find_compiled_func(ts_mir_compiler_t *c, const char *name) {
  for (int i = 0; i < c->compiled_func_count; i++) {
    if (c->compiled_funcs[i].name && strcmp(c->compiled_funcs[i].name, name) == 0)
      return &c->compiled_funcs[i];
  }
  return NULL;
}

static const char *ts_find_func_alias(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return NULL;
  for (size_t i = 0; i < c->func_alias_count; ++i) {
    if (c->func_aliases[i].param_name &&
        strcmp(c->func_aliases[i].param_name, name) == 0) {
      return c->func_aliases[i].target_name;
    }
  }
  return NULL;
}

/* =========================================================================
 *  闭包支持：生成 Prologue 加载捕获变量
 * ========================================================================= */

/**
 * @brief 生成闭包 prologue，从闭包环境加载捕获的变量
 *
 * 在函数入口处为每个捕获变量生成：
 *   var_reg = call load_captured_var(ctx, "var_name", closure_env)
 */
static void ts_emit_closure_prologue(ts_mir_compiler_t *c) {
  if (!c->closure_analysis) return;

  ts_closure_analysis_t *analysis = (ts_closure_analysis_t *)c->closure_analysis;
  if (analysis->captured_count == 0) return;

  /* 为每个捕获变量生成加载代码 */
  for (size_t i = 0; i < analysis->captured_count; i++) {
    const char *var_name = analysis->captured_vars[i];

    /* 创建或获取变量寄存器 */
    MIR_reg_t var_reg = get_or_create_reg(c, var_name);

    /* 生成调用：var_reg = load_captured_var(ctx, var_name, closure_env) */
    MIR_append_insn(c->ctx, c->func,
      MIR_new_call_insn(c->ctx, 6,
        MIR_new_ref_op(c->ctx, c->ext.load_captured_proto),
        MIR_new_ref_op(c->ctx, c->ext.load_captured_import),
        MIR_new_reg_op(c->ctx, var_reg),                          // 返回值
        MIR_new_reg_op(c->ctx, c->ctx_reg),                       // ctx
        MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)var_name),  // 变量名
        MIR_new_reg_op(c->ctx, c->closure_env_reg)));            // closure_env
  }
}

static int ts_function_call_is_known_class(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  if (ts_mir_is_known_class_name(c, name)) return 1;
  exprtk_value_t value = exprtk_env_get(&c->ts_ctx->env, name);
  return value.type == EXPRTK_VAL_CLASS;
}

static int ts_name_matches_variable_param(const char *name, exprtk_node_t **params,
                                          size_t param_count) {
  if (!name || !params) return 0;
  for (size_t i = 0; i < param_count; ++i) {
    exprtk_node_t *param = params[i];
    if (!param) continue;
    if (param->type == EXPRTK_NODE_VARIABLE && param->data.variable.name &&
        strcmp(name, param->data.variable.name) == 0) {
      return 1;
    }
    if (param->type == EXPRTK_NODE_ASSIGNMENT && param->data.assignment.name &&
        strcmp(name, param->data.assignment.name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ts_node_calls_param_function(exprtk_node_t *node, exprtk_node_t **params,
                                        size_t param_count) {
  if (!node) return 0;

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_calls_param_function(node->data.block.statements[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_FLOW:
    return ts_node_calls_param_function(node->data.flow.value, params, param_count);
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    return ts_node_calls_param_function(node->data.assignment.value, params, param_count);
  case EXPRTK_NODE_BINARY_OP:
    return ts_node_calls_param_function(node->data.binary.left, params, param_count) ||
           ts_node_calls_param_function(node->data.binary.right, params, param_count);
  case EXPRTK_NODE_IF:
    return ts_node_calls_param_function(node->data.if_stmt.condition, params, param_count) ||
           ts_node_calls_param_function(node->data.if_stmt.if_branch, params, param_count) ||
           ts_node_calls_param_function(node->data.if_stmt.else_branch, params, param_count);
  case EXPRTK_NODE_WHILE:
    return ts_node_calls_param_function(node->data.while_loop.condition, params, param_count) ||
           ts_node_calls_param_function(node->data.while_loop.body, params, param_count);
  case EXPRTK_NODE_FOR:
    return ts_node_calls_param_function(node->data.for_loop.init, params, param_count) ||
           ts_node_calls_param_function(node->data.for_loop.condition, params, param_count) ||
           ts_node_calls_param_function(node->data.for_loop.post, params, param_count) ||
           ts_node_calls_param_function(node->data.for_loop.body, params, param_count);
  case EXPRTK_NODE_DO_WHILE:
    return ts_node_calls_param_function(node->data.do_while.body, params, param_count) ||
           ts_node_calls_param_function(node->data.do_while.condition, params, param_count);
  case EXPRTK_NODE_FOR_IN:
    return ts_node_calls_param_function(node->data.for_in.collection, params, param_count) ||
           ts_node_calls_param_function(node->data.for_in.body, params, param_count);
  case EXPRTK_NODE_FUNCTION_CALL:
    if (ts_name_matches_variable_param(node->data.function.name, params, param_count)) return 1;
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_calls_param_function(node->data.function.args[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_calls_param_function(node->data.member_call.object, params, param_count))
      return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_calls_param_function(node->data.member_call.args[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_ACCESS:
    return ts_node_calls_param_function(node->data.member_access.object, params, param_count);
  case EXPRTK_NODE_MEMBER_SET:
    return ts_node_calls_param_function(node->data.member_set.object, params, param_count) ||
           ts_node_calls_param_function(node->data.member_set.value, params, param_count);
  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i) {
      if (ts_node_calls_param_function(node->data.vector.elements[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      if (ts_node_calls_param_function(node->data.map_literal.values[i], params, param_count))
        return 1;
    }
    return 0;
  case EXPRTK_NODE_INDEX:
    return ts_node_calls_param_function(node->data.index_access.array, params, param_count) ||
           ts_node_calls_param_function(node->data.index_access.index, params, param_count);
  case EXPRTK_NODE_SLICE:
    return ts_node_calls_param_function(node->data.slice.array, params, param_count) ||
           ts_node_calls_param_function(node->data.slice.start, params, param_count) ||
           ts_node_calls_param_function(node->data.slice.end, params, param_count);
  default:
    return 0;
  }
}

static int ts_node_uses_name_as_value(exprtk_node_t *node, const char *name) {
  if (!node || !name) return 0;

  switch (node->type) {
  case EXPRTK_NODE_VARIABLE:
    return node->data.variable.name && strcmp(node->data.variable.name, name) == 0;

  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    if (node->data.assignment.name && strcmp(node->data.assignment.name, name) == 0) return 1;
    return ts_node_uses_name_as_value(node->data.assignment.value, name);

  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_uses_name_as_value(node->data.block.statements[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_FLOW:
    return ts_node_uses_name_as_value(node->data.flow.value, name);

  case EXPRTK_NODE_BINARY_OP:
    return ts_node_uses_name_as_value(node->data.binary.left, name) ||
           ts_node_uses_name_as_value(node->data.binary.right, name);

  case EXPRTK_NODE_IF:
    return ts_node_uses_name_as_value(node->data.if_stmt.condition, name) ||
           ts_node_uses_name_as_value(node->data.if_stmt.if_branch, name) ||
           ts_node_uses_name_as_value(node->data.if_stmt.else_branch, name);

  case EXPRTK_NODE_WHILE:
    return ts_node_uses_name_as_value(node->data.while_loop.condition, name) ||
           ts_node_uses_name_as_value(node->data.while_loop.body, name);

  case EXPRTK_NODE_FOR:
    return ts_node_uses_name_as_value(node->data.for_loop.init, name) ||
           ts_node_uses_name_as_value(node->data.for_loop.condition, name) ||
           ts_node_uses_name_as_value(node->data.for_loop.post, name) ||
           ts_node_uses_name_as_value(node->data.for_loop.body, name);

  case EXPRTK_NODE_DO_WHILE:
    return ts_node_uses_name_as_value(node->data.do_while.body, name) ||
           ts_node_uses_name_as_value(node->data.do_while.condition, name);

  case EXPRTK_NODE_FOR_IN:
    if (node->data.for_in.var_name && strcmp(node->data.for_in.var_name, name) == 0) return 1;
    return ts_node_uses_name_as_value(node->data.for_in.collection, name) ||
           ts_node_uses_name_as_value(node->data.for_in.body, name);

  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_uses_name_as_value(node->data.function.args[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_uses_name_as_value(node->data.member_call.object, name)) return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_uses_name_as_value(node->data.member_call.args[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MEMBER_ACCESS:
    return ts_node_uses_name_as_value(node->data.member_access.object, name);

  case EXPRTK_NODE_MEMBER_SET:
    return ts_node_uses_name_as_value(node->data.member_set.object, name) ||
           ts_node_uses_name_as_value(node->data.member_set.value, name);

  case EXPRTK_NODE_VECTOR:
    for (size_t i = 0; i < node->data.vector.count; ++i) {
      if (ts_node_uses_name_as_value(node->data.vector.elements[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_MAP_LITERAL:
    for (size_t i = 0; i < node->data.map_literal.count; ++i) {
      if (ts_node_uses_name_as_value(node->data.map_literal.values[i], name)) return 1;
    }
    return 0;

  case EXPRTK_NODE_INDEX:
    return ts_node_uses_name_as_value(node->data.index_access.array, name) ||
           ts_node_uses_name_as_value(node->data.index_access.index, name);

  case EXPRTK_NODE_SLICE:
    return ts_node_uses_name_as_value(node->data.slice.array, name) ||
           ts_node_uses_name_as_value(node->data.slice.start, name) ||
           ts_node_uses_name_as_value(node->data.slice.end, name);

  default:
    return 0;
  }
}

static int ts_node_needs_value_return_bridge(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return 0;
  if (node->type == EXPRTK_NODE_CLASS_DEF) return 1;
  if (node->type == EXPRTK_NODE_NEW) return 1;
  if (node->type == EXPRTK_NODE_THIS) return 1;
  if (node->type == EXPRTK_NODE_SUPER) return 1;
  if (node->type == EXPRTK_NODE_VARIABLE &&
      ts_function_call_is_known_class(c, node->data.variable.name)) {
    return 1;
  }

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i) {
      if (ts_node_needs_value_return_bridge(c, node->data.block.statements[i])) return 1;
    }
    return 0;
  case EXPRTK_NODE_IF:
    return ts_node_needs_value_return_bridge(c, node->data.if_stmt.condition) ||
           ts_node_needs_value_return_bridge(c, node->data.if_stmt.if_branch) ||
           ts_node_needs_value_return_bridge(c, node->data.if_stmt.else_branch);
  case EXPRTK_NODE_WHILE:
    return ts_node_needs_value_return_bridge(c, node->data.while_loop.condition) ||
           ts_node_needs_value_return_bridge(c, node->data.while_loop.body);
  case EXPRTK_NODE_FOR:
    return ts_node_needs_value_return_bridge(c, node->data.for_loop.init) ||
           ts_node_needs_value_return_bridge(c, node->data.for_loop.condition) ||
           ts_node_needs_value_return_bridge(c, node->data.for_loop.post) ||
           ts_node_needs_value_return_bridge(c, node->data.for_loop.body);
  case EXPRTK_NODE_DO_WHILE:
    return ts_node_needs_value_return_bridge(c, node->data.do_while.body) ||
           ts_node_needs_value_return_bridge(c, node->data.do_while.condition);
  case EXPRTK_NODE_FOR_IN:
    return 1;
  case EXPRTK_NODE_SWITCH:
    return 1;
  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    return 1;
  case EXPRTK_NODE_TRY_CATCH:
    return 1;
  case EXPRTK_NODE_THROW:
    return 1;
  case EXPRTK_NODE_FLOW:
    return ts_node_needs_value_return_bridge(c, node->data.flow.value);
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    return ts_node_needs_value_return_bridge(c, node->data.assignment.value);
  case EXPRTK_NODE_BINARY_OP:
    return ts_node_needs_value_return_bridge(c, node->data.binary.left) ||
           ts_node_needs_value_return_bridge(c, node->data.binary.right);
  case EXPRTK_NODE_FUNCTION_CALL:
    if (ts_function_call_is_known_class(c, node->data.function.name)) return 1;
    for (size_t i = 0; i < node->data.function.arg_count; ++i) {
      if (ts_node_needs_value_return_bridge(c, node->data.function.args[i])) return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_CALL:
    if (ts_node_needs_value_return_bridge(c, node->data.member_call.object)) return 1;
    for (size_t i = 0; i < node->data.member_call.arg_count; ++i) {
      if (ts_node_needs_value_return_bridge(c, node->data.member_call.args[i])) return 1;
    }
    return 0;
  case EXPRTK_NODE_MEMBER_SET:
    return ts_node_needs_value_return_bridge(c, node->data.member_set.object) ||
           ts_node_needs_value_return_bridge(c, node->data.member_set.value);
  case EXPRTK_NODE_MEMBER_ACCESS:
    return 1;
  case EXPRTK_NODE_INDEX:
    return ts_node_needs_value_return_bridge(c, node->data.index_access.array) ||
           ts_node_needs_value_return_bridge(c, node->data.index_access.index);
  case EXPRTK_NODE_INSTANCEOF:
    return ts_node_needs_value_return_bridge(c, node->data.instanceof_expr.object) ||
           ts_node_needs_value_return_bridge(c, node->data.instanceof_expr.class_expr);
  default:
    return 0;
  }
}

static int ts_func_alias_index(ts_mir_func_alias_t *aliases, size_t alias_count,
                               const char *param_name) {
  if (!aliases || !param_name) return -1;
  for (size_t i = 0; i < alias_count; ++i) {
    if (aliases[i].param_name && strcmp(aliases[i].param_name, param_name) == 0)
      return (int)i;
  }
  return -1;
}

static int ts_aliases_need_closure_env(ts_mir_compiler_t *c, ts_mir_func_alias_t *aliases,
                                       size_t alias_count) {
  if (!c || !aliases) return 0;
  for (size_t i = 0; i < alias_count; ++i) {
    ts_compiled_func_t *cf = ts_find_compiled_func(c, aliases[i].target_name);
    if (cf && cf->has_closure_env) return 1;
  }
  return 0;
}

/* Compile a script function as a separate MIR function.
 * Signature: double func_name(double arg0, double arg1, ...)
 * Or with closure: double func_name(void *ctx, void *closure_env, double arg0, double arg1, ...)
 * MUST be called when no other MIR function is open. */
static void ts_compile_script_func_with_aliases(ts_mir_compiler_t *c, const char *name,
                                                exprtk_node_t **arg_params, size_t arg_count,
                                                exprtk_node_t *body,
                                                ts_mir_func_alias_t *aliases,
                                                size_t alias_count) {
  if (ts_find_compiled_func(c, name)) return;
  if (arg_count > 16) return; /* sanity limit */
  if (alias_count == 0 && ts_node_calls_param_function(body, arg_params, arg_count)) return;
  for (size_t i = 0; i < alias_count; ++i) {
    if (ts_node_uses_name_as_value(body, aliases[i].param_name)) return;
  }
  if (ts_node_needs_value_return_bridge(c, body)) return;
  if (!ts_mir_ensure_compiled_func_capacity(c, c->compiled_func_count + 1)) return;

  /* 闭包分析：检查是否捕获外部变量 */
  ts_closure_analysis_t *analysis = ts_analyze_closure(body, arg_params, arg_count, &c->ts_ctx->env);

  /* 如果不能 JIT（嵌套闭包、太多变量等），跳过编译 */
  if (analysis && !analysis->can_jit) {
    ts_closure_analysis_free(analysis);
    return;
  }
  int needs_closure_env =
      (analysis && analysis->captured_count > 0) ||
      ts_aliases_need_closure_env(c, aliases, alias_count);

  ts_mir_compile_frame_t frame = ts_mir_capture_frame(c);

  /* Reset compiler state for the new function */
  ts_mir_begin_isolated_compile(c);

  /* 保存闭包分析结果到编译器 */
  c->closure_analysis = (void *)analysis;
  c->func_aliases = aliases;
  c->func_alias_count = alias_count;

  /* Create unique function name */
  char func_name[128];
  snprintf(func_name, sizeof(func_name), "%s_ts_%s", c->item_prefix, name);

  /* Build MIR function signature */
  MIR_type_t res_type = MIR_T_D;
  MIR_var_t mir_args[18];  // 最多 1 ctx + 1 closure_env + 16 参数
  size_t numeric_arg_count = 0;
  for (size_t i = 0; i < arg_count; i++) {
    const char *pname = arg_params[i]->data.variable.name;
    if (ts_func_alias_index(aliases, alias_count, pname) < 0) numeric_arg_count++;
  }
  uint32_t total_args = (uint32_t)numeric_arg_count;

  /* 如果需要闭包环境，添加 ctx 和 closure_env 作为前两个参数 */
  if (needs_closure_env) {
    mir_args[0].type = MIR_T_P;
    mir_args[0].name = "ctx";
    mir_args[0].size = 0;

    mir_args[1].type = MIR_T_P;
    mir_args[1].name = "closure_env";
    mir_args[1].size = 0;

    size_t out_i = 0;
    for (size_t i = 0; i < arg_count; i++) {
      const char *pname = arg_params[i]->data.variable.name;
      if (ts_func_alias_index(aliases, alias_count, pname) >= 0) continue;
      mir_args[out_i + 2].type = MIR_T_D;
      mir_args[out_i + 2].name = pname;
      mir_args[out_i + 2].size = 0;
      out_i++;
    }
    total_args = (uint32_t)(numeric_arg_count + 2);
  } else {
    size_t out_i = 0;
    for (size_t i = 0; i < arg_count; i++) {
      const char *pname = arg_params[i]->data.variable.name;
      if (ts_func_alias_index(aliases, alias_count, pname) >= 0) continue;
      mir_args[out_i].type = MIR_T_D;
      mir_args[out_i].name = pname;
      mir_args[out_i].size = 0;
      out_i++;
    }
  }

  MIR_item_t new_func =
      MIR_new_func_arr(c->ctx, func_name, 1, &res_type, total_args, mir_args);
  c->func = new_func;

  /* Create and register the call proto before compiling the body so recursive
   * calls can resolve to a complete MIR call target. */
  char proto_name[128];
  snprintf(proto_name, sizeof(proto_name), "%s_p_ts_%s", c->item_prefix, name);
  MIR_item_t proto =
      MIR_new_proto_arr(c->ctx, proto_name, 1, &res_type, total_args, mir_args);

  int idx = c->compiled_func_count++;
  c->compiled_funcs[idx].name = strdup(name);
  if (!c->compiled_funcs[idx].name) {
    ts_mir_fail(c, "JIT compile error: out of memory duplicating compiled function name '%s'",
                name ? name : "<unnamed>");
  }
  c->compiled_funcs[idx].mir_func = new_func;
  c->compiled_funcs[idx].proto = proto;
  c->compiled_funcs[idx].arg_count = numeric_arg_count;
  c->compiled_funcs[idx].has_closure_env = needs_closure_env;

  /* 如果有闭包环境，获取 ctx 和 closure_env 寄存器 */
  if (needs_closure_env) {
    c->ctx_reg = MIR_reg(c->ctx, "ctx", new_func->u.func);
    c->closure_env_reg = MIR_reg(c->ctx, "closure_env", new_func->u.func);
  }

  /* Map parameter names to their MIR registers */
  for (size_t i = 0; i < arg_count; i++) {
    const char *pname = arg_params[i]->data.variable.name;
    if (ts_func_alias_index(aliases, alias_count, pname) >= 0) continue;
    if (!ts_mir_bind_existing_reg(c, pname, MIR_reg(c->ctx, pname, new_func->u.func))) break;
  }

  /* 生成闭包 prologue：加载捕获的变量 */
  if (analysis && analysis->captured_count > 0) {
    ts_emit_closure_prologue(c);
  }

  /* Compile the function body */
  ts_compile_stmt(c, body);

  /* Default return 0.0 (in case body doesn't return) */
  MIR_reg_t ret_reg = new_temp_reg(c);
  MIR_append_insn(c->ctx, new_func,
                  MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, ret_reg),
                               MIR_new_double_op(c->ctx, 0.0)));
  MIR_append_insn(c->ctx, new_func, MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_reg)));

  MIR_finish_func(c->ctx);

  /* 清理闭包分析结果 */
  c->closure_analysis = NULL;
  c->func_aliases = NULL;
  c->func_alias_count = 0;
  ts_closure_analysis_free(analysis);

  /* Restore parent compiler state */
  ts_mir_restore_frame(c, &frame);
}

static void ts_compile_script_func(ts_mir_compiler_t *c, const char *name,
                                   exprtk_node_t **arg_params, size_t arg_count,
                                   exprtk_node_t *body) {
  ts_compile_script_func_with_aliases(c, name, arg_params, arg_count, body, NULL, 0);
}

static void ts_prescan_class_names(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  if (node->type == EXPRTK_NODE_CLASS_DEF && node->data.class_def.name) {
    ts_mir_add_class_name(c, node->data.class_def.name);
  }

  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; ++i) {
      ts_prescan_class_names(c, node->data.block.statements[i]);
    }
  }
}

/* Pre-scan AST for function definitions and compile them as MIR functions.
 * Must be called BEFORE the main function is created. */
static void ts_prescan_functions(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!node) return;

  if (node->type == EXPRTK_NODE_FUNCTION_DEFINITION) {
    if (node->data.func_def.name && node->data.func_def.body &&
        node->data.func_def.arg_count <= 16) {
      int all_vars = 1;
      for (size_t i = 0; i < node->data.func_def.arg_count; i++) {
        if (node->data.func_def.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
          all_vars = 0;
          break;
        }
      }
      if (all_vars) {
        ts_compile_script_func(c, node->data.func_def.name, node->data.func_def.arg_params,
                               node->data.func_def.arg_count, node->data.func_def.body);
      }
    }
  }

  /* Recurse into blocks to find nested function definitions */
  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; i++) {
      ts_prescan_functions(c, node->data.block.statements[i]);
    }
  }
}

static exprtk_func_t *ts_find_script_func_in_env(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return NULL;
  for (exprtk_env_t *env = &c->ts_ctx->env; env; env = env->parent) {
    for (exprtk_func_t *f = env->funcs; f; f = f->next) {
      if (f->name && strcmp(f->name, name) == 0 && f->is_script) return f;
    }
  }
  return NULL;
}

static exprtk_node_t *ts_find_function_def_node(exprtk_node_t *node, const char *name) {
  if (!node || !name) return NULL;
  if (node->type == EXPRTK_NODE_FUNCTION_DEFINITION &&
      node->data.func_def.name && strcmp(node->data.func_def.name, name) == 0) {
    return node;
  }
  if (node->type == EXPRTK_NODE_BLOCK) {
    for (size_t i = 0; i < node->data.block.count; ++i) {
      exprtk_node_t *found = ts_find_function_def_node(node->data.block.statements[i], name);
      if (found) return found;
    }
  }
  return NULL;
}

static exprtk_node_t *ts_find_script_func_node(ts_mir_compiler_t *c, const char *name) {
  return c ? ts_find_function_def_node(c->ast_root, name) : NULL;
}

static void ts_sanitize_name_part(const char *src, char *dst, size_t dst_size) {
  size_t out = 0;
  if (!dst || dst_size == 0) return;
  if (!src) src = "anon";
  for (size_t i = 0; src[i] && out + 1 < dst_size; ++i) {
    char ch = src[i];
    int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
             (ch >= '0' && ch <= '9') || ch == '_';
    dst[out++] = ok ? ch : '_';
  }
  dst[out] = '\0';
}

static int ts_build_hof_lambda_name(exprtk_node_t *node, char *out, size_t out_size) {
  if (!node || node->type != EXPRTK_NODE_FUNCTION_EXPRESSION || !out || out_size == 0) return 0;
  return snprintf(out, out_size, "__lambda_%zx", (size_t)(uintptr_t)node) > 0;
}

static int ts_function_expr_can_compile_for_hof(ts_mir_compiler_t *c, exprtk_node_t *node) {
  ts_closure_analysis_t *analysis = NULL;
  int ok = 0;

  if (!c || !node || node->type != EXPRTK_NODE_FUNCTION_EXPRESSION ||
      node->data.func_def.arg_count > 16 || !node->data.func_def.body) {
    return 0;
  }
  for (size_t i = 0; i < node->data.func_def.arg_count; ++i) {
    exprtk_node_t *param = node->data.func_def.arg_params[i];
    if (!param || param->type != EXPRTK_NODE_VARIABLE || !param->data.variable.name) return 0;
  }

  analysis = ts_analyze_closure(node->data.func_def.body, node->data.func_def.arg_params,
                                node->data.func_def.arg_count, &c->ts_ctx->env);
  ok = !analysis || analysis->can_jit;
  ts_closure_analysis_free(analysis);
  return ok;
}

static int ts_hof_resolve_function_arg(ts_mir_compiler_t *c, exprtk_node_t *arg,
                                       char lambda_name[64], const char **target_name) {
  if (!c || !arg || !target_name) return 0;

  if (arg->type == EXPRTK_NODE_VARIABLE && arg->data.variable.name) {
    ts_compiled_func_t *cf = ts_find_compiled_func(c, arg->data.variable.name);
    if (!cf) return 0;
    *target_name = arg->data.variable.name;
    return 1;
  }

  if (arg->type == EXPRTK_NODE_FUNCTION_EXPRESSION &&
      ts_function_expr_can_compile_for_hof(c, arg) &&
      ts_build_hof_lambda_name(arg, lambda_name, 64) &&
      ts_find_compiled_func(c, lambda_name)) {
    *target_name = lambda_name;
    return 1;
  }

  return 0;
}

static int ts_build_hof_specialization_key(const char *base_name,
                                           ts_mir_func_alias_t *aliases,
                                           size_t alias_count,
                                           char *out,
                                           size_t out_size) {
  char part[64];
  size_t used = 0;
  if (!base_name || !out || out_size == 0 || alias_count == 0) return 0;

  ts_sanitize_name_part(base_name, part, sizeof(part));
  used = (size_t)snprintf(out, out_size, "__hof_%s", part);
  if (used >= out_size) return 0;

  for (size_t i = 0; i < alias_count; ++i) {
    ts_sanitize_name_part(aliases[i].target_name, part, sizeof(part));
    used += (size_t)snprintf(out + used, out_size - used, "_%s", part);
    if (used >= out_size) return 0;
  }
  return 1;
}

static ts_compiled_func_t *ts_find_hof_specialized_call(ts_mir_compiler_t *c,
                                                        exprtk_node_t *call_node,
                                                        int numeric_arg_indices[16],
                                                        size_t *numeric_arg_count) {
  exprtk_func_t *callee = NULL;
  exprtk_node_t *callee_node = NULL;
  ts_mir_func_alias_t aliases[16];
  char lambda_names[16][64];
  size_t alias_count = 0;
  size_t numeric_count = 0;
  char key[256];

  if (!c || !call_node || call_node->type != EXPRTK_NODE_FUNCTION_CALL ||
      !call_node->data.function.name || !numeric_arg_count) {
    return NULL;
  }

  callee_node = ts_find_script_func_node(c, call_node->data.function.name);
  callee = callee_node ? NULL : ts_find_script_func_in_env(c, call_node->data.function.name);
  if (!callee_node && !callee) return NULL;

  size_t callee_arg_count =
      callee_node ? callee_node->data.func_def.arg_count : callee->data.script.arg_count;
  exprtk_node_t **callee_arg_params =
      callee_node ? callee_node->data.func_def.arg_params : callee->data.script.arg_params;
  exprtk_node_t *callee_body =
      callee_node ? callee_node->data.func_def.body : callee->data.script.body;

  if (!callee_body ||
      callee_arg_count != call_node->data.function.arg_count ||
      callee_arg_count > 16) {
    return NULL;
  }

  for (size_t i = 0; i < callee_arg_count; ++i) {
    exprtk_node_t *param = callee_arg_params[i];
    exprtk_node_t *arg = call_node->data.function.args[i];
    if (!param || param->type != EXPRTK_NODE_VARIABLE || !param->data.variable.name ||
        !arg) {
      return NULL;
    }

    if (ts_node_calls_param_function(callee_body, &param, 1)) {
      const char *target_name = NULL;
      if (!ts_hof_resolve_function_arg(c, arg, lambda_names[alias_count], &target_name) ||
          ts_node_uses_name_as_value(callee_body, param->data.variable.name)) {
        return NULL;
      }
      aliases[alias_count].param_name = param->data.variable.name;
      aliases[alias_count].target_name = target_name;
      alias_count++;
    } else {
      numeric_arg_indices[numeric_count++] = (int)i;
    }
  }

  if (alias_count == 0) return NULL;
  if (!ts_build_hof_specialization_key(call_node->data.function.name, aliases, alias_count,
                                       key, sizeof(key))) {
    return NULL;
  }

  *numeric_arg_count = numeric_count;
  return ts_find_compiled_func(c, key);
}

static void ts_prescan_hof_specializations(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return;

  if (node->type == EXPRTK_NODE_FUNCTION_CALL && node->data.function.name) {
    exprtk_node_t *callee_node = ts_find_script_func_node(c, node->data.function.name);
    exprtk_func_t *callee =
        callee_node ? NULL : ts_find_script_func_in_env(c, node->data.function.name);
    ts_mir_func_alias_t aliases[16];
    char lambda_names[16][64];
    size_t alias_count = 0;
    char key[256];

    if (callee_node || callee) {
      size_t callee_arg_count =
          callee_node ? callee_node->data.func_def.arg_count : callee->data.script.arg_count;
      exprtk_node_t **callee_arg_params =
          callee_node ? callee_node->data.func_def.arg_params : callee->data.script.arg_params;
      exprtk_node_t *callee_body =
          callee_node ? callee_node->data.func_def.body : callee->data.script.body;
      int all_vars = 1;
      if (!callee_body || callee_arg_count != node->data.function.arg_count ||
          callee_arg_count > 16) {
        all_vars = 0;
      }
      for (size_t i = 0; all_vars && i < callee_arg_count; ++i) {
        exprtk_node_t *param = callee_arg_params[i];
        exprtk_node_t *arg = node->data.function.args[i];
        if (!param || param->type != EXPRTK_NODE_VARIABLE || !param->data.variable.name) {
          all_vars = 0;
          break;
        }
        if (ts_node_calls_param_function(callee_body, &param, 1)) {
          const char *target_name = NULL;
          if (arg && arg->type == EXPRTK_NODE_FUNCTION_EXPRESSION &&
              ts_function_expr_can_compile_for_hof(c, arg) &&
              ts_build_hof_lambda_name(arg, lambda_names[alias_count],
                                       sizeof(lambda_names[alias_count])) &&
              !ts_find_compiled_func(c, lambda_names[alias_count])) {
            ts_compile_script_func(c, lambda_names[alias_count],
                                   arg->data.func_def.arg_params,
                                   arg->data.func_def.arg_count,
                                   arg->data.func_def.body);
          }
          if (!ts_hof_resolve_function_arg(c, arg, lambda_names[alias_count], &target_name) ||
              ts_node_uses_name_as_value(callee_body, param->data.variable.name)) {
            all_vars = 0;
            break;
          }
          aliases[alias_count].param_name = param->data.variable.name;
          aliases[alias_count].target_name = target_name;
          alias_count++;
        }
      }

      if (all_vars && alias_count > 0 &&
          ts_build_hof_specialization_key(node->data.function.name, aliases, alias_count,
                                          key, sizeof(key)) &&
          !ts_find_compiled_func(c, key)) {
        ts_compile_script_func_with_aliases(c, key, callee_arg_params, callee_arg_count,
                                            callee_body,
                                            aliases, alias_count);
      }
    }
  }

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; ++i)
      ts_prescan_hof_specializations(c, node->data.block.statements[i]);
    break;
  case EXPRTK_NODE_ASSIGNMENT:
  case EXPRTK_NODE_CONSTANT_DECL:
    ts_prescan_hof_specializations(c, node->data.assignment.value);
    break;
  case EXPRTK_NODE_FLOW:
    ts_prescan_hof_specializations(c, node->data.flow.value);
    break;
  case EXPRTK_NODE_BINARY_OP:
    ts_prescan_hof_specializations(c, node->data.binary.left);
    ts_prescan_hof_specializations(c, node->data.binary.right);
    break;
  case EXPRTK_NODE_IF:
    ts_prescan_hof_specializations(c, node->data.if_stmt.condition);
    ts_prescan_hof_specializations(c, node->data.if_stmt.if_branch);
    ts_prescan_hof_specializations(c, node->data.if_stmt.else_branch);
    break;
  case EXPRTK_NODE_WHILE:
    ts_prescan_hof_specializations(c, node->data.while_loop.condition);
    ts_prescan_hof_specializations(c, node->data.while_loop.body);
    break;
  case EXPRTK_NODE_FOR:
    ts_prescan_hof_specializations(c, node->data.for_loop.init);
    ts_prescan_hof_specializations(c, node->data.for_loop.condition);
    ts_prescan_hof_specializations(c, node->data.for_loop.post);
    ts_prescan_hof_specializations(c, node->data.for_loop.body);
    break;
  case EXPRTK_NODE_DO_WHILE:
    ts_prescan_hof_specializations(c, node->data.do_while.body);
    ts_prescan_hof_specializations(c, node->data.do_while.condition);
    break;
  case EXPRTK_NODE_FUNCTION_CALL:
    for (size_t i = 0; i < node->data.function.arg_count; ++i)
      ts_prescan_hof_specializations(c, node->data.function.args[i]);
    break;
  default:
    break;
  }
}

/* =========================================================================
 *  Variable Bridge Functions
 * ========================================================================= */

double ts_mir_load_var(void *ctx_ptr, const char *name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  return ts_mir_numeric_value(val);
}

void ts_mir_store_var(void *ctx_ptr, const char *name, double value) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val;
  val.type = EXPRTK_VAL_NUMBER;
  val.data.number = value;
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
    exprtk_map_set(&map, key, value);
  }

  exprtk_env_set(&ctx->env, target_name, map);
  exprtk_map_free(&map);
  return 0.0;
}

double ts_mir_string_assign(void *ctx_ptr, const char *target_name, const char *data,
                            int64_t len) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val;
  if (!ctx || !target_name || !data || len < 0) return 0.0;

  val = exprtk_val_str(tstr_v_from_buf((char *)data, (size_t)len));
  exprtk_env_set(&ctx->env, target_name, val);
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

static exprtk_value_t ts_mir_call_bridge(void *ctx_ptr, const char *name, size_t argc,
                                         double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (size_t i = 0; i < argc; i++) {
    args[i].type = EXPRTK_VAL_NUMBER;
    args[i].data.number = argv[i];
  }
  return exprtk_call_internal(name, argc, args, &ctx->env, &ctx->env.arena);
}

double ts_mir_call0(void *ctx_ptr, const char *name) {
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 0, NULL);
  return ts_mir_numeric_value(r);
}

double ts_mir_call1(void *ctx_ptr, const char *name, double a0) {
  double argv[1] = {a0};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 1, argv);
  return ts_mir_numeric_value(r);
}

double ts_mir_call2(void *ctx_ptr, const char *name, double a0, double a1) {
  double argv[2] = {a0, a1};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 2, argv);
  return ts_mir_numeric_value(r);
}

double ts_mir_call3(void *ctx_ptr, const char *name, double a0, double a1, double a2) {
  double argv[3] = {a0, a1, a2};
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, 3, argv);
  return ts_mir_numeric_value(r);
}

double ts_mir_calln(void *ctx_ptr, const char *name, int64_t argc, double *argv) {
  exprtk_value_t r = ts_mir_call_bridge(ctx_ptr, name, (size_t)argc, argv);
  return ts_mir_numeric_value(r);
}

double ts_mir_call_assign(void *ctx_ptr, const char *target_name, const char *name,
                          int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !name) return 0.0;

  exprtk_value_t result = ts_mir_call_bridge(ctx_ptr, name, (size_t)argc, argv);
  exprtk_env_set(&ctx->env, target_name, result);
  return ts_mir_numeric_value(result);
}

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
    if (!uuid_to_s(value.data.uuid, buf, (int)buf_size)) return 0;
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
    char *data = NULL;

    if (!ts_mir_runtime_value_text(lhs, l_buf, sizeof(l_buf), &l_data, &l_len) ||
        !ts_mir_runtime_value_text(rhs, r_buf, sizeof(r_buf), &r_data, &r_len)) {
      return 0;
    }

    data = (char *)mem_alloc(&env->arena, l_len + r_len + 1);
    if (!data) return 0;
    if (l_len > 0) memcpy(data, l_data, l_len);
    if (r_len > 0) memcpy(data + l_len, r_data, r_len);
    data[l_len + r_len] = '\0';
    *out = exprtk_val_str(tstr_v_from_buf(data, l_len + r_len));
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

static int ts_mir_template_append(exprtk_env_t *env, char **buf, size_t *len, size_t *cap,
                                  const char *data, size_t data_len) {
  char *next;
  size_t needed;

  if (!env || !buf || !len || !cap || (!data && data_len > 0)) return 0;
  needed = *len + data_len + 1;
  if (needed > *cap) {
    size_t next_cap = *cap ? *cap * 2 : 64;
    while (next_cap < needed) next_cap *= 2;
    next = (char *)mem_alloc(&env->arena, next_cap);
    if (!next) return 0;
    if (*buf && *len > 0) memcpy(next, *buf, *len);
    *buf = next;
    *cap = next_cap;
  }
  if (data_len > 0) memcpy(*buf + *len, data, data_len);
  *len += data_len;
  (*buf)[*len] = '\0';
  return 1;
}

static int ts_mir_template_append_value(exprtk_env_t *env, char **buf, size_t *len,
                                        size_t *cap, exprtk_value_t value) {
  char num_buf[64];
  int n;

  switch (value.type) {
  case EXPRTK_VAL_INTEGER:
    n = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value.data.integer);
    return n >= 0 && ts_mir_template_append(env, buf, len, cap, num_buf, (size_t)n);
  case EXPRTK_VAL_NUMBER:
    n = snprintf(num_buf, sizeof(num_buf), "%g", value.data.number);
    return n >= 0 && ts_mir_template_append(env, buf, len, cap, num_buf, (size_t)n);
  case EXPRTK_VAL_BOOL:
    return value.data.boolean
               ? ts_mir_template_append(env, buf, len, cap, "true", 4)
               : ts_mir_template_append(env, buf, len, cap, "false", 5);
  case EXPRTK_VAL_STRING:
    return ts_mir_template_append(env, buf, len, cap, value.data.string.data,
                                  value.data.string.len);
  case EXPRTK_VAL_BYTES:
    n = snprintf(num_buf, sizeof(num_buf), "bytes(%zu)", value.data.bytes.len);
    return n >= 0 && ts_mir_template_append(env, buf, len, cap, num_buf, (size_t)n);
  case EXPRTK_VAL_UUID:
    if (!uuid_to_s(value.data.uuid, num_buf, sizeof(num_buf))) return 0;
    return ts_mir_template_append(env, buf, len, cap, num_buf, strlen(num_buf));
  case EXPRTK_VAL_DATETIME: {
    time_t ts = turbo_datetime_to_time(&value.data.datetime);
    if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, num_buf, sizeof(num_buf)) < 0) return 0;
    return ts_mir_template_append(env, buf, len, cap, num_buf, strlen(num_buf));
  }
  case EXPRTK_VAL_DATE:
  case EXPRTK_VAL_TIME:
  case EXPRTK_VAL_DURATION:
  case EXPRTK_VAL_DECIMAL: {
    const char *text = NULL;
    size_t text_len = 0;
    if (!ts_mir_runtime_value_text(value, num_buf, sizeof(num_buf), &text, &text_len)) return 0;
    return ts_mir_template_append(env, buf, len, cap, text, text_len);
  }
  case EXPRTK_VAL_NULL:
    return ts_mir_template_append(env, buf, len, cap, "null", 4);
  case EXPRTK_VAL_VECTOR:
    return ts_mir_template_append(env, buf, len, cap, "[vector]", 8);
  case EXPRTK_VAL_MAP:
    return ts_mir_template_append(env, buf, len, cap, "[map]", 5);
  case EXPRTK_VAL_OBJECT:
    return ts_mir_template_append(env, buf, len, cap, "[object]", 8);
  case EXPRTK_VAL_LIST:
    return ts_mir_template_append(env, buf, len, cap, "[list]", 6);
  default:
    return ts_mir_template_append(env, buf, len, cap, "", 0);
  }
}

static int ts_mir_runtime_template_value(exprtk_node_t *node, exprtk_env_t *env,
                                      exprtk_value_t *out) {
  const char *str;
  size_t len;
  size_t i = 0;
  char *result = NULL;
  size_t result_len = 0;
  size_t result_cap = 0;

  if (!node || !env || !out || node->type != EXPRTK_NODE_TEMPLATE_STRING) return 0;
  str = node->data.template_string.template_str;
  len = node->data.template_string.len;
  if (!str) return 0;

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
      if (depth != 0) return 0;
      if (i - 1 > expr_start) {
        size_t expr_len = i - 1 - expr_start;
        char *expr_str = (char *)malloc(expr_len + 1);
        exprtk_node_t *expr_node;
        exprtk_value_t expr_value;
        int ok;
        if (!expr_str) return 0;
        memcpy(expr_str, str + expr_start, expr_len);
        expr_str[expr_len] = '\0';
        expr_node = exprtk_parse(expr_str, expr_len);
        free(expr_str);
        if (!expr_node) return 0;
        ok = ts_mir_runtime_value_arg(expr_node, env, &expr_value) &&
             ts_mir_template_append_value(env, &result, &result_len, &result_cap, expr_value);
        exprtk_free(expr_node);
        if (!ok) return 0;
      }
      continue;
    }

    if (!ts_mir_template_append(env, &result, &result_len, &result_cap, &str[i], 1)) {
      return 0;
    }
    i++;
  }

  if (!result) {
    result = (char *)mem_alloc(&env->arena, 1);
    if (!result) return 0;
    result[0] = '\0';
  }
  *out = exprtk_val_str(tstr_v_from_buf(result, result_len));
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
    exprtk_env_init_local(&catch_env);
    catch_env.parent = env;
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

static int ts_mir_runtime_define_func_in_env(exprtk_env_t *env, exprtk_node_t *node) {
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
        free(args);
        *out = throw_error(env, node, "Parent class has no static method '%s'",
                           node->data.super_expr.member);
        return 1;
      }
      if (!can_access_method(env, method, node)) {
        free(args);
        *out = throw_method_access_error(env, node, node->data.super_expr.member, method);
        return 1;
      }

      *out = eval_script_function(method, argc, args, env, env);
      free(args);
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
      free(args);
      *out = zero;
      return 1;
    }

    *out = eval_script_function(constructor, argc, args, env, env);
    free(args);
    return 1;
  }

  if (node->data.super_expr.is_call) {
    size_t argc = 0;
    exprtk_value_t *args = ts_mir_runtime_call_value_args(node, env, &argc);
    if (!args && node->data.super_expr.arg_count > 0) return 0;

    exprtk_func_t *method =
        exprtk_class_lookup_method_typed(parent, node->data.super_expr.member, 0, argc, args);
    if (!method) {
      free(args);
      *out = throw_error(env, node, "Parent class has no method '%s'",
                         node->data.super_expr.member);
      return 1;
    }
    if (!can_access_method(env, method, node)) {
      free(args);
      *out = throw_method_access_error(env, node, node->data.super_expr.member, method);
      return 1;
    }

    *out = eval_script_function(method, argc, args, env, env);
    free(args);
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
      exprtk_instance_set_field(object.data.instance_val.instance, node->data.member_set.member,
                                val);
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
    exprtk_instance_set_field(this_val.data.instance_val.instance, node->data.member_set.member,
                              val);
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
    exprtk_instance_set_field(this_val.data.instance_val.instance, node->data.member_set.member,
                              val);
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
    exprtk_env_set(env, node->data.assignment.name, *out);
    return 1;

  case EXPRTK_NODE_CONSTANT_DECL:
    if (!node->data.assignment.name ||
        !ts_mir_runtime_value_arg(node->data.assignment.value, env, out)) {
      return 0;
    }
    exprtk_env_set_constant(env, node->data.assignment.name, *out);
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
    *out = exprtk_call_internal(node->data.function.name, argc, args, env, &env->arena);
    free(args);
    return !env->aborted;
  }

  case EXPRTK_NODE_MEMBER_CALL: {
    exprtk_value_t object;
    exprtk_value_t *args = NULL;
    size_t argc = 0;

    if (!node->data.member_call.object || !node->data.member_call.method) return 0;

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
        *out = exprtk_call_internal(full_name, argc, args, env, &env->arena);
        free(args);
        return !env->aborted;
      }

      *out = exprtk_member_call_checked_value_nodes(
          node->data.member_call.object->data.variable.name, node->data.member_call.method,
          node->data.member_call.object, node, env);
      if (!env->aborted) return 1;
    }

    if (!ts_mir_runtime_value_arg(node->data.member_call.object, env, &object)) return 0;
    if (strcmp(node->data.member_call.method, "length") == 0 &&
        node->data.member_call.arg_count == 0) {
      if (object.type == EXPRTK_VAL_LIST) {
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
    double *data = NULL;

    for (size_t i = 0; i < node->data.vector.count; ++i) {
      exprtk_value_t value;
      exprtk_node_t *element = node->data.vector.elements[i];

      if (element && element->type == EXPRTK_NODE_SPREAD) {
        if (!ts_mir_runtime_value_arg(element->data.spread.child, env, &value)) {
          free(vals);
          return 0;
        }
        if (value.type == EXPRTK_VAL_VECTOR) {
          if (!ts_mir_value_array_grow(&vals, &cap, actual + value.data.vector.size)) {
            free(vals);
            return 0;
          }
          for (size_t j = 0; j < value.data.vector.size; ++j) {
            vals[actual++] = exprtk_val_num(value.data.vector.data[j]);
          }
        } else if (value.type == EXPRTK_VAL_LIST) {
          if (!ts_mir_value_array_grow(&vals, &cap, actual + value.data.list.count)) {
            free(vals);
            return 0;
          }
          for (size_t j = 0; j < value.data.list.count; ++j) {
            vals[actual++] = value.data.list.items[j];
          }
        } else {
          if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
            free(vals);
            return 0;
          }
          vals[actual++] = value;
        }
        continue;
      }

      if (!ts_mir_runtime_value_arg(element, env, &value) ||
          !ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
        free(vals);
        return 0;
      }
      vals[actual++] = value;
    }

    if (actual > 0) {
      data = (double *)mem_alloc(&env->arena, actual * sizeof(double));
      if (!data) {
        free(vals);
        return 0;
      }
      for (size_t i = 0; i < actual; ++i) {
        data[i] = ts_mir_numeric_value(vals[i]);
      }
    }
    free(vals);
    *out = exprtk_val_vec(data, actual);
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
      exprtk_map_set(&map, node->data.map_literal.keys[i], value);
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
      return 0;
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
    if (array.type == EXPRTK_VAL_BYTES) {
      *out = (size_t)idx < array.data.bytes.len
                 ? exprtk_val_int((unsigned char)array.data.bytes.data[idx])
                 : exprtk_val_num(0.0);
      return 1;
    }
    return 0;
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

    if (start < 0 || end < start) return 0;
    if (array.type == EXPRTK_VAL_VECTOR) {
      if ((size_t)end > array.data.vector.size) return 0;
      size_t count = (size_t)(end - start);
      double *data = NULL;
      if (count > 0) {
        data = (double *)mem_alloc(&env->arena, count * sizeof(double));
        if (!data) return 0;
        memcpy(data, array.data.vector.data + start, count * sizeof(double));
      }
      *out = exprtk_val_vec(data, count);
      return 1;
    }
    if (array.type == EXPRTK_VAL_LIST) {
      if ((size_t)end > array.data.list.count) return 0;
      size_t count = (size_t)(end - start);
      exprtk_value_t *items = NULL;
      if (count > 0) {
        items = (exprtk_value_t *)mem_alloc(&env->arena, count * sizeof(exprtk_value_t));
        if (!items) return 0;
        memcpy(items, array.data.list.items + start, count * sizeof(exprtk_value_t));
      }
      *out = exprtk_val_list_ex(items, count, 0);
      return 1;
    }
    return 0;
  }

  case EXPRTK_NODE_MEMBER_ACCESS: {
    exprtk_value_t object;
    const char *member = node->data.member_access.member;
    if (!member || !ts_mir_runtime_value_arg(node->data.member_access.object, env, &object)) {
      return 0;
    }
    if (strcmp(member, "length") == 0) {
      if (object.type == EXPRTK_VAL_LIST) {
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
        free(vals);
        return NULL;
      }
      if (spread.type == EXPRTK_VAL_VECTOR) {
        if (!ts_mir_value_array_grow(&vals, &cap, actual + spread.data.vector.size)) {
          free(vals);
          return NULL;
        }
        for (size_t j = 0; j < spread.data.vector.size; ++j) {
          vals[actual++] = exprtk_val_num(spread.data.vector.data[j]);
        }
      } else if (spread.type == EXPRTK_VAL_LIST) {
        if (!ts_mir_value_array_grow(&vals, &cap, actual + spread.data.list.count)) {
          free(vals);
          return NULL;
        }
        for (size_t j = 0; j < spread.data.list.count; ++j) {
          vals[actual++] = spread.data.list.items[j];
        }
      } else {
        if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
          free(vals);
          return NULL;
        }
        vals[actual++] = spread;
      }
      continue;
    }

    if (!ts_mir_value_array_grow(&vals, &cap, actual + 1)) {
      free(vals);
      return NULL;
    }
    if (!ts_mir_runtime_value_arg(arg, env, &vals[actual])) {
      if (!env->aborted && env->error_msg[0] == '\0') ts_mir_value_arg_error(env, arg);
      free(vals);
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

  result = exprtk_call_internal(name, argc, args, &ctx->env, &ctx->env.arena);
  exprtk_env_set(&ctx->env, target_name, result);
  free(args);
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

  if (!ctx || !node) return 0.0;
  if (!ts_mir_runtime_value_arg(node, &ctx->env, &result)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(result);
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
  return value;
}

static int ts_mir_eval_await_arg(exprtk_node_t *node, exprtk_env_t *env, exprtk_value_t *out) {
  exprtk_value_t *args = NULL;
  size_t argc = 0;

  if (!node || !env || !out) return 0;

  if (node->type == EXPRTK_NODE_FUNCTION_CALL && node->data.function.name) {
    args = ts_mir_runtime_call_value_args(node, env, &argc);
    if (!args && node->data.function.arg_count > 0) return 0;
    *out = exprtk_call_internal(node->data.function.name, argc, args, env, &env->arena);
    free(args);
    return env->flow == exprtk_FLOW_NORMAL && !env->aborted;
  }

  return ts_mir_runtime_value_arg(node, env, out);
}

double ts_mir_await_value(void *ctx_ptr, void *arg_node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *arg_node = (exprtk_node_t *)arg_node_ptr;
  exprtk_value_t value;

  if (!ctx || !arg_node) return 0.0;
  if (!ts_mir_eval_await_arg(arg_node, &ctx->env, &value)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, arg_node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  value = ts_mir_await_result(value);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(value);
}

double ts_mir_await_assign(void *ctx_ptr, const char *target_name, void *arg_node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *arg_node = (exprtk_node_t *)arg_node_ptr;
  exprtk_value_t value;

  if (!ctx || !target_name || !arg_node) return 0.0;
  if (!ts_mir_eval_await_arg(arg_node, &ctx->env, &value)) {
    if (!ctx->env.aborted && ctx->env.error_msg[0] == '\0')
      ts_mir_value_arg_error(&ctx->env, arg_node);
    ts_mir_promote_env_error(ctx);
    return 0.0;
  }

  value = ts_mir_await_result(value);
  exprtk_env_set(&ctx->env, target_name, value);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
  return ts_mir_numeric_value(value);
}

double ts_mir_function_expr_assign(void *ctx_ptr, const char *target_name, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  exprtk_value_t value;

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

/* =========================================================================
 *  Direct native function dispatch (no strcmp lookup)
 * ========================================================================= */

/* Direct call to exprtk_native_fn — env-registered functions */
double ts_mir_call_native(void *ctx_ptr, void *fn_ptr, void *user_data, int64_t argc,
                          double *argv) {
  (void)ctx_ptr;
  exprtk_native_fn fn = (exprtk_native_fn)fn_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (int64_t i = 0; i < argc; i++) {
    args[i].type = EXPRTK_VAL_NUMBER;
    args[i].data.number = argv[i];
  }
  exprtk_value_t r = fn((size_t)argc, args, user_data);
  return ts_mir_numeric_value(r);
}

/* Direct call to exprtk_builtin_fn — module/registry functions */
double ts_mir_call_builtin(void *ctx_ptr, void *fn_ptr, int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_builtin_fn fn = (exprtk_builtin_fn)fn_ptr;
  exprtk_value_t args[16];
  if (argc > 16) argc = 16;
  for (int64_t i = 0; i < argc; i++) {
    args[i].type = EXPRTK_VAL_NUMBER;
    args[i].data.number = argv[i];
  }
  exprtk_value_t r = fn((size_t)argc, args, &ctx->env, &ctx->env.arena);
  return ts_mir_numeric_value(r);
}

void ts_mir_oop_define_class(void *ctx_ptr, void *node_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_node_t *node = (exprtk_node_t *)node_ptr;
  if (!ctx || !node) return;
  exprtk_oop_define_class(node, &ctx->env);
  if (ctx->env.flow != exprtk_FLOW_NORMAL || ctx->env.aborted)
    ts_mir_promote_env_error(ctx);
}

double ts_mir_oop_class_alias(void *ctx_ptr, const char *target_name, const char *source_name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !source_name) return 0.0;

  exprtk_value_t result = exprtk_oop_alias_class(target_name, source_name, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_new_assign(void *ctx_ptr, const char *target_name, const char *class_name,
                             int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !class_name) return 0.0;

  exprtk_value_t instance = exprtk_oop_instantiate_numeric(class_name, (size_t)argc, argv, &ctx->env);
  if (ctx->env.aborted) return 0.0;
  exprtk_env_set(&ctx->env, target_name, instance);
  return ts_mir_numeric_value(instance);
}

double ts_mir_oop_member_call(void *ctx_ptr, const char *object_name, const char *method_name,
                              void *object_node, int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !method_name) return 0.0;

  exprtk_value_t result = exprtk_member_call_checked_numeric(
      object_name, method_name, (exprtk_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_value(void *ctx_ptr, const char *object_name,
                                    const char *method_name, void *object_node,
                                    void *call_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !method_name) return 0.0;

  exprtk_value_t result = exprtk_member_call_checked_value_nodes(
      object_name, method_name, (exprtk_node_t *)object_node,
      (exprtk_node_t *)call_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_mono_value(void *ctx_ptr, const char *object_name,
                                         const char *expected_class_name,
                                         const char *method_name, void *object_node,
                                         void *call_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !expected_class_name || !method_name) return 0.0;

  exprtk_value_t result = exprtk_oop_call_method_mono_checked_value_nodes(
      object_name, expected_class_name, method_name, (exprtk_node_t *)object_node,
      (exprtk_node_t *)call_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_cached(void *ctx_ptr, const char *object_name,
                                     const char *method_name, void *cache_ptr, void *object_node,
                                     int64_t argc, double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !method_name) return 0.0;

  exprtk_value_t object = exprtk_env_get(&ctx->env, object_name);
  exprtk_value_t result;
  if (object.type == EXPRTK_VAL_INSTANCE || object.type == EXPRTK_VAL_CLASS) {
    result = exprtk_oop_call_method_cached_checked_numeric(
        object_name, method_name, (exprtk_oop_method_cache_t *)cache_ptr,
        (exprtk_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  } else {
    result = exprtk_member_call_checked_numeric(
        object_name, method_name, (exprtk_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  }
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_assign(void *ctx_ptr, const char *target_name,
                                     const char *object_name, const char *method_name,
                                     void *cache_ptr, void *object_node, int64_t argc,
                                     double *argv) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !object_name || !method_name) return 0.0;

  exprtk_value_t object = exprtk_env_get(&ctx->env, object_name);
  exprtk_value_t result;
  if (object.type == EXPRTK_VAL_INSTANCE || object.type == EXPRTK_VAL_CLASS) {
    result = exprtk_oop_call_method_cached_checked_numeric(
        object_name, method_name, (exprtk_oop_method_cache_t *)cache_ptr,
        (exprtk_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  } else {
    result = exprtk_member_call_checked_numeric(
        object_name, method_name, (exprtk_node_t *)object_node, (size_t)argc, argv, &ctx->env);
  }
  exprtk_env_set(&ctx->env, target_name, result);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_call_assign_value(void *ctx_ptr, const char *target_name,
                                           const char *object_name, const char *method_name,
                                           void *object_node, void *call_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !target_name || !object_name || !method_name) return 0.0;

  exprtk_value_t result = exprtk_member_call_checked_value_nodes(
      object_name, method_name, (exprtk_node_t *)object_node,
      (exprtk_node_t *)call_node, &ctx->env);
  exprtk_env_set(&ctx->env, target_name, result);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_get(void *ctx_ptr, const char *object_name, const char *member_name,
                             void *cache_ptr, void *object_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !member_name) return 0.0;

  exprtk_value_t result = exprtk_oop_get_member_cached_checked(
      object_name, member_name, (exprtk_oop_field_cache_t *)cache_ptr,
      (exprtk_node_t *)object_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_member_set(void *ctx_ptr, const char *object_name, const char *member_name,
                             void *cache_ptr, void *object_node, double value) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !member_name) return 0.0;

  exprtk_value_t result = exprtk_oop_set_member_cached_checked_numeric(
      object_name, member_name, value, (exprtk_oop_field_cache_t *)cache_ptr,
      (exprtk_node_t *)object_node, &ctx->env);
  return ts_mir_numeric_value(result);
}

void *ts_mir_oop_num_ptr(void *ctx_ptr, const char *object_name, const char *member_name,
                         void *object_node, int64_t create_if_missing, void *slot_cache_ptr) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  ts_mir_oop_slot_cache_t *slot_cache = (ts_mir_oop_slot_cache_t *)slot_cache_ptr;
  if (!ctx || !object_name || !member_name || !slot_cache) return NULL;

  exprtk_value_t object = exprtk_env_get(&ctx->env, object_name);
  if (object.type != EXPRTK_VAL_INSTANCE || !object.data.instance_val.instance) {
    ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: '%s.%s' requires an instance numeric field",
             object_name, member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }

  exprtk_instance_t *instance = object.data.instance_val.instance;
  exprtk_value_t *slot = exprtk_instance_get_field_slot(instance, member_name);
  if (!slot && create_if_missing) {
    exprtk_instance_set_field(instance, member_name, exprtk_val_num(0.0));
    slot = exprtk_instance_get_field_slot(instance, member_name);
  }
  uintptr_t slot_addr = (uintptr_t)slot;
  uintptr_t slots_begin = (uintptr_t)instance->field_slots;
  uintptr_t slots_end = slots_begin + instance->field_slot_count * sizeof(*instance->field_slots);
  if (!slot || !instance->field_slots || slot_addr < slots_begin || slot_addr >= slots_end) {
    ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: instance has no numeric slot field '%s'", member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }

  if (slot->type == EXPRTK_VAL_INTEGER) {
    slot->type = EXPRTK_VAL_NUMBER;
    slot->data.number = (double)slot->data.integer;
  }
  if (slot->type != EXPRTK_VAL_NUMBER) {
    ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "JIT runtime error: field '%s' is not numeric", member_name);
    snprintf(ctx->env.error_msg, sizeof(ctx->env.error_msg), "%s", ctx->error_msg);
    ctx->env.aborted = 1;
    return NULL;
  }

  (void)object_node;
  slot_cache->instance = instance;
  slot_cache->index = (size_t)(slot - instance->field_slots);
  return instance;
}

double ts_mir_oop_instanceof(void *ctx_ptr, const char *object_name, const char *class_name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !object_name || !class_name) return 0.0;

  exprtk_value_t result = exprtk_oop_instanceof_name(object_name, class_name, &ctx->env);
  return ts_mir_numeric_value(result);
}

double ts_mir_oop_predicate(void *ctx_ptr, int64_t kind, const char *name, const char *member,
                            void *object_node) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  if (!ctx || !name) return 0.0;

  exprtk_value_t value;
  if (member) {
    exprtk_value_t object = exprtk_env_get(&ctx->env, name);
    if (exprtk_value_is_object_like(&object) && exprtk_map_has(&object, member)) {
      value = exprtk_map_get(&object, member);
    } else {
      value = exprtk_oop_get_member_checked(name, member, (exprtk_node_t *)object_node, &ctx->env);
    }
  } else {
    value = exprtk_env_get(&ctx->env, name);
  }

  switch (kind) {
    case 1:
      return value.type == EXPRTK_VAL_CLASS ? 1.0 : 0.0;
    case 2:
      return value.type == EXPRTK_VAL_INSTANCE ? 1.0 : 0.0;
    case 3:
      return value.type == EXPRTK_VAL_FUNCTION || value.type == EXPRTK_VAL_BOUND_METHOD ? 1.0
                                                                                         : 0.0;
    case 4:
      return value.type == EXPRTK_VAL_NULL ? 1.0 : 0.0;
    default:
      return 0.0;
  }
}

/* =========================================================================
 *  Vector Indexing Bridge
 * ========================================================================= */

double ts_mir_vec_get(void *ctx_ptr, const char *name, double index) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  if (val.type == EXPRTK_VAL_VECTOR) {
    int idx = (int)index;
    if (idx >= 0 && idx < (int)val.data.vector.size) return val.data.vector.data[idx];
  }
  return 0.0;
}

/* =========================================================================
 *  Function Definition Bridge
 * ========================================================================= */

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

static void ts_mir_promote_env_error(turbo_script_ctx_t *ctx) {
  if (!ctx) return;

  if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE)
    ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;

  if (ctx->error_msg[0] == '\0') {
    const char *msg = ctx->env.error_msg[0] ? ctx->env.error_msg : "JIT runtime error";
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg);
  }
  ctx->env.aborted = 1;
}

/* =========================================================================
 *  Closure Variable Loading Bridge
 * ========================================================================= */

/**
 * @brief 从闭包环境加载捕获的变量
 * 
 * @param ctx_ptr turbo_script_ctx_t* 指针
 * @param var_name 变量名
 * @param closure_env_ptr 闭包环境指针（exprtk_env_t*）
 * @return 变量值（数值类型），未找到返回 0.0
 */
double ts_mir_load_captured_var(void *ctx_ptr, const char *var_name, void *closure_env_ptr) {
  (void)ctx_ptr; // 当前未使用，保留用于统计
  
  if (!closure_env_ptr || !var_name) return 0.0;
  
  exprtk_env_t *closure_env = (exprtk_env_t *)closure_env_ptr;
  exprtk_value_t val = exprtk_env_get(closure_env, var_name);
  
  return ts_mir_numeric_value(val);
}

/* =========================================================================
 *  Member Access Bridge (obj.prop → double)
 * ========================================================================= */

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

/* =========================================================================
 *  Vector Data Pointer Bridge (for native indexing)
 * ========================================================================= */

void *ts_mir_vec_data(void *ctx_ptr, const char *name) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t val = exprtk_env_get(&ctx->env, name);
  if (val.type == EXPRTK_VAL_VECTOR && val.data.vector.data) return (void *)val.data.vector.data;
  return NULL;
}

/* =========================================================================
 *  Map Access by Key (O(1) hash lookup)
 * ========================================================================= */

double ts_mir_map_get_key(void *ctx_ptr, const char *obj_name, const char *key) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (exprtk_value_is_object_like(&obj)) {
    exprtk_value_t val = exprtk_map_get(&obj, key);
    return ts_mir_numeric_value(val);
  }
  return 0.0;
}

/* Returns a pointer to the value's number field inside the HTAB for direct memory access.
 * The pointer is cached in the JIT prologue — one call per field, not per access. */
void *ts_mir_map_num_ptr(void *ctx_ptr, const char *obj_name, const char *key) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)ctx_ptr;
  exprtk_value_t obj = exprtk_env_get(&ctx->env, obj_name);
  if (exprtk_value_is_object_like(&obj)) {
    /* We need a stable pointer into the HTAB entry.
     * Use exprtk_map_get_ptr which returns a pointer to the value inside the htab. */
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

/* =========================================================================
 *  Compile-time constant folding
 * Recursively evaluates constant expression trees, returns 1 if foldable.
 * ========================================================================= */

static int ts_binary_is_null_eq_compare(exprtk_node_t *node) {
  if (!node || node->type != EXPRTK_NODE_BINARY_OP || !node->data.binary.left) return 0;
  if (node->data.binary.op != exprtk_TOKEN_EQ && node->data.binary.op != exprtk_TOKEN_NE) return 0;
  return (node->data.binary.left && node->data.binary.left->type == EXPRTK_NODE_NULL) ||
         (node->data.binary.right && node->data.binary.right->type == EXPRTK_NODE_NULL);
}

static int ts_try_fold_constant(exprtk_node_t *node, double *out) {
  if (!node) return 0;

  if (node->type == EXPRTK_NODE_NUMBER) {
    *out = node->data.number;
    return 1;
  }

  if (node->type == EXPRTK_NODE_INTEGER) {
    *out = (double)node->data.integer;
    return 1;
  }

  if (node->type == EXPRTK_NODE_NULL) {
    *out = 0.0;
    return 1;
  }

  if (node->type == EXPRTK_NODE_BINARY_OP) {
    /* Unary operators */
    if (node->data.binary.left == NULL) {
      double r;
      if (!ts_try_fold_constant(node->data.binary.right, &r)) return 0;
      switch (node->data.binary.op) {
      case exprtk_TOKEN_MINUS:
        *out = -r;
        return 1;
      case exprtk_TOKEN_PLUS:
        *out = r;
        return 1;
      case exprtk_TOKEN_NOT:
        *out = (r == 0.0) ? 1.0 : 0.0;
        return 1;
      default:
        return 0;
      }
    }

    if (ts_binary_is_null_eq_compare(node)) {
      exprtk_node_t *left = node->data.binary.left;
      exprtk_node_t *right = node->data.binary.right;
      int left_const = left && (left->type == EXPRTK_NODE_NULL ||
                                left->type == EXPRTK_NODE_NUMBER ||
                                left->type == EXPRTK_NODE_INTEGER ||
                                left->type == EXPRTK_NODE_STRING);
      int right_const = right && (right->type == EXPRTK_NODE_NULL ||
                                  right->type == EXPRTK_NODE_NUMBER ||
                                  right->type == EXPRTK_NODE_INTEGER ||
                                  right->type == EXPRTK_NODE_STRING);
      int both_null = 0;
      if (!left_const || !right_const) return 0;
      both_null = left->type == EXPRTK_NODE_NULL && right->type == EXPRTK_NODE_NULL;
      *out = node->data.binary.op == exprtk_TOKEN_EQ ? (double)both_null
                                                     : (double)!both_null;
      return 1;
    }

    double l, r;
    if (!ts_try_fold_constant(node->data.binary.left, &l)) return 0;
    if (!ts_try_fold_constant(node->data.binary.right, &r)) return 0;

    switch (node->data.binary.op) {
    case exprtk_TOKEN_PLUS:
      *out = l + r;
      return 1;
    case exprtk_TOKEN_MINUS:
      *out = l - r;
      return 1;
    case exprtk_TOKEN_MULTIPLY:
      *out = l * r;
      return 1;
    case exprtk_TOKEN_DIVIDE:
      *out = (r != 0.0) ? l / r : 0.0;
      return 1;
    case exprtk_TOKEN_MOD:
      *out = fmod(l, r);
      return 1;
    case exprtk_TOKEN_POWER:
      *out = pow(l, r);
      return 1;
    case exprtk_TOKEN_LT:
      *out = (l < r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_GT:
      *out = (l > r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_LE:
      *out = (l <= r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_GE:
      *out = (l >= r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_EQ:
      *out = (l == r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_NE:
      *out = (l != r) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_AND:
      *out = (l != 0.0 && r != 0.0) ? 1.0 : 0.0;
      return 1;
    case exprtk_TOKEN_OR:
      *out = (l != 0.0 || r != 0.0) ? 1.0 : 0.0;
      return 1;
    default:
      return 0;
    }
  }

  return 0;
}

/* =========================================================================
 * Expression Compiler (Phase 1-7)
 * ========================================================================= */

static MIR_reg_t ts_emit_if_expression(ts_mir_compiler_t *c, exprtk_node_t *node) {
  MIR_reg_t res = new_temp_reg(c);
  MIR_label_t else_label = MIR_new_label(c->ctx);
  MIR_label_t end_label = MIR_new_label(c->ctx);

  ts_compile_branch_false(c, node->data.if_stmt.condition, else_label);
  if (c->failed) return res;

  if (node->data.if_stmt.if_branch) {
    MIR_reg_t then_reg = ts_compile_expr(c, node->data.if_stmt.if_branch);
    if (c->failed) return res;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_reg_op(c->ctx, then_reg)));
  } else {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 0.0)));
  }

  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));
  MIR_append_insn(c->ctx, c->func, else_label);

  if (node->data.if_stmt.else_branch) {
    MIR_reg_t else_reg = ts_compile_expr(c, node->data.if_stmt.else_branch);
    if (c->failed) return res;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_reg_op(c->ctx, else_reg)));
  } else {
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                 MIR_new_double_op(c->ctx, 0.0)));
  }

  MIR_append_insn(c->ctx, c->func, end_label);
  return res;
}

static MIR_reg_t ts_compile_expr(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || !node) return 0;
  if (c->failed) return ts_emit_zero_reg(c);

  /*  Try constant folding before anything else */
  {
    double folded;
    if (ts_try_fold_constant(node, &folded)) {
      MIR_reg_t r = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                   MIR_new_double_op(c->ctx, folded)));
      return r;
    }
  }

  /* Fast sub-dispatch for data access + assignment nodes. */
  {
    MIR_reg_t specialized = 0;
    if (ts_compile_data_access_and_assignment(c, node, &specialized)) return specialized;
  }

  switch (node->type) {
  case EXPRTK_NODE_NUMBER: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, node->data.number)));
    return r;
  }

  case EXPRTK_NODE_INTEGER: {
    MIR_reg_t r = new_temp_reg(c);
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, r),
                                 MIR_new_double_op(c->ctx, (double)node->data.integer)));
    return r;
  }

  case EXPRTK_NODE_VARIABLE:
    if (node->data.variable.name && ts_mir_var_is_dynamic(c, node->data.variable.name)) {
      MIR_reg_t r = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                                        MIR_new_reg_op(c->ctx, r),
                                        MIR_new_reg_op(c->ctx, c->ctx_reg),
                                        MIR_new_uint_op(c->ctx,
                                                        (uint64_t)(uintptr_t)node->data.variable.name)));
      return r;
    }
    return get_or_create_reg(c, node->data.variable.name);

  case EXPRTK_NODE_BINARY_OP: {
    /*  Unary operators (left == NULL) */
    if (node->data.binary.left == NULL) {
      MIR_reg_t operand = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = new_temp_reg(c);

      if (node->data.binary.op == exprtk_TOKEN_NOT) {
        /* !x => (x == 0.0) ? 1.0 : 0.0 */
        MIR_reg_t ireg = new_temp_ireg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DEQ, MIR_new_reg_op(c->ctx, ireg),
                                     MIR_new_reg_op(c->ctx, operand),
                                     MIR_new_double_op(c->ctx, 0.0)));
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, ireg)));
        return res;
      } else if (node->data.binary.op == exprtk_TOKEN_MINUS) {
        /* unary minus: -x => 0.0 - x */
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DSUB, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_double_op(c->ctx, 0.0),
                                     MIR_new_reg_op(c->ctx, operand)));
        return res;
      } else if (node->data.binary.op == exprtk_TOKEN_PLUS) {
        /* unary plus: +x => x (no-op) */
        return operand;
      }
      /* unknown unary: just return the operand */
      return operand;
    }

    if (ts_binary_is_null_eq_compare(node)) {
      return ts_emit_runtime_value_node(c, node);
    }

    /*  Logical AND (short-circuit) */
    if (node->data.binary.op == exprtk_TOKEN_AND) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t res = new_temp_reg(c);
      MIR_label_t false_label = MIR_new_label(c->ctx);
      MIR_label_t end_label = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_double_op(c->ctx, 0.0)));

      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t ireg = new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DNE, MIR_new_reg_op(c->ctx, ireg),
                                   MIR_new_reg_op(c->ctx, right), MIR_new_double_op(c->ctx, 0.0)));
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, ireg)));
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, false_label);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_double_op(c->ctx, 0.0)));

      MIR_append_insn(c->ctx, c->func, end_label);
      return res;
    }

    /*  Logical OR (short-circuit) */
    if (node->data.binary.op == exprtk_TOKEN_OR) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t res = new_temp_reg(c);
      MIR_label_t true_label = MIR_new_label(c->ctx);
      MIR_label_t end_label = MIR_new_label(c->ctx);

      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_double_op(c->ctx, 0.0)));

      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t ireg = new_temp_ireg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DNE, MIR_new_reg_op(c->ctx, ireg),
                                   MIR_new_reg_op(c->ctx, right), MIR_new_double_op(c->ctx, 0.0)));
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, ireg)));
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, true_label);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, res),
                                   MIR_new_double_op(c->ctx, 1.0)));

      MIR_append_insn(c->ctx, c->func, end_label);
      return res;
    }

    /*  Modulo via fmod() external call */
    if (node->data.binary.op == exprtk_TOKEN_MOD) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.fmod_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.fmod_import),
                                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, left),
                                        MIR_new_reg_op(c->ctx, right)));
      return res;
    }

    /*  Power via pow() external call */
    if (node->data.binary.op == exprtk_TOKEN_POWER) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_reg_t res = new_temp_reg(c);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.pow_proto),
                                        MIR_new_ref_op(c->ctx, c->ext.pow_import),
                                        MIR_new_reg_op(c->ctx, res), MIR_new_reg_op(c->ctx, left),
                                        MIR_new_reg_op(c->ctx, right)));
      return res;
    }

    if (node->data.binary.op == exprtk_TOKEN_EQ ||
        node->data.binary.op == exprtk_TOKEN_NE) {
      MIR_reg_t typeof_cmp = 0;
      if (ts_try_emit_typeof_oop_compare(c, node->data.binary.left, node->data.binary.right,
                                         node->data.binary.op, &typeof_cmp)) {
        return typeof_cmp;
      }
    }

    /* Compound assignments */
    MIR_reg_t res = 0;
    switch (node->data.binary.op) {
    case exprtk_TOKEN_ASSIGN_ADD:
    case exprtk_TOKEN_ASSIGN_SUB:
    case exprtk_TOKEN_ASSIGN_MUL:
    case exprtk_TOKEN_ASSIGN_DIV: {
      if (node->data.binary.left->type != EXPRTK_NODE_VARIABLE) return 0;
      ts_mir_clear_var_class(c, node->data.binary.left->data.variable.name);
      MIR_reg_t target = get_or_create_reg(c, node->data.binary.left->data.variable.name);
      MIR_reg_t rhs = ts_compile_expr(c, node->data.binary.right);
      MIR_insn_code_t op;
      if (node->data.binary.op == exprtk_TOKEN_ASSIGN_ADD) op = MIR_DADD;
      else if (node->data.binary.op == exprtk_TOKEN_ASSIGN_SUB) op = MIR_DSUB;
      else if (node->data.binary.op == exprtk_TOKEN_ASSIGN_MUL) op = MIR_DMUL;
      else op = MIR_DDIV;
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, target),
                                   MIR_new_reg_op(c->ctx, target), MIR_new_reg_op(c->ctx, rhs)));
      return target;
    }
    default: {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_insn_code_t op;
      int is_cmp = 0;
      switch (node->data.binary.op) {
      case exprtk_TOKEN_PLUS:
        op = MIR_DADD;
        break;
      case exprtk_TOKEN_MINUS:
        op = MIR_DSUB;
        break;
      case exprtk_TOKEN_MULTIPLY:
        op = MIR_DMUL;
        break;
      case exprtk_TOKEN_DIVIDE:
        op = MIR_DDIV;
        break;
      case exprtk_TOKEN_LT:
        op = MIR_DLT;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_GT:
        op = MIR_DGT;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_LE:
        op = MIR_DLE;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_GE:
        op = MIR_DGE;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_EQ:
        op = MIR_DEQ;
        is_cmp = 1;
        break;
      case exprtk_TOKEN_NE:
        op = MIR_DNE;
        is_cmp = 1;
        break;
      default:
        op = MIR_DADD;
        break;
      }
      if (is_cmp) {
        MIR_reg_t ireg = new_temp_ireg(c);
        res = new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, ireg),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_I2D, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, ireg)));
      } else {
        res = new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, op, MIR_new_reg_op(c->ctx, res),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
      }
    }
    }
    return res;
  }

  case EXPRTK_NODE_IF:
    return ts_emit_if_expression(c, node);

  case EXPRTK_NODE_BLOCK:
    ts_register_helper_written_vars(c, node);
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_INSTANCEOF: {
    if (node->data.instanceof_expr.object &&
        (node->data.instanceof_expr.class_name || node->data.instanceof_expr.class_expr)) {
      const char *obj_name = ts_emit_oop_receiver_to_temp(c, node->data.instanceof_expr.object);
      const char *class_name = node->data.instanceof_expr.class_name;
      if (!class_name && node->data.instanceof_expr.class_expr) {
        class_name = ts_emit_oop_receiver_to_temp(c, node->data.instanceof_expr.class_expr);
      }
      return obj_name && class_name ? ts_emit_oop_instanceof(c, obj_name, class_name)
                                    : ts_emit_unsupported_node(c, node);
    }
    return ts_emit_unsupported_node(c, node);
  }

  /* Phase 4+9: Function call compilation */
  case EXPRTK_NODE_FUNCTION_CALL: {
    size_t argc = node->data.function.arg_count;
    const char *name = node->data.function.name;
    if (!name) return 0;
    MIR_reg_t hof_res = ts_try_emit_hof_specialized_call(c, node);
    if (hof_res) return hof_res;
    const char *alias_name = ts_find_func_alias(c, name);
    if (alias_name) name = alias_name;

    if (strcmp(name, "await") == 0) {
      return ts_emit_await_value(c, argc > 0 ? node->data.function.args[0] : NULL);
    }

    if (ts_runtime_call_needs_value_eval(c, name, argc) ||
        ts_call_args_need_ast_eval(c, argc, node->data.function.args) ||
        ts_expr_list_contains_value_call(c, argc, node->data.function.args)) {
      return ts_emit_runtime_value_node(c, node);
    }

    int oop_predicate = ts_oop_predicate_kind(name);
    if (oop_predicate && argc == 1 &&
        ts_oop_predicate_can_preserve_value(oop_predicate, node->data.function.args[0])) {
      return ts_emit_oop_predicate(c, oop_predicate, node->data.function.args[0]);
    }

    MIR_reg_t arg_regs[16];
    argc = ts_compile_call_args(c, argc, node->data.function.args, arg_regs);

    MIR_reg_t res = new_temp_reg(c);

    /*  Check if function was compiled as native MIR */
    ts_compiled_func_t *cf = ts_find_compiled_func(c, name);
    if (cf && cf->arg_count == argc) {
      MIR_reg_t call_res = ts_emit_compiled_func_call(c, cf, argc, arg_regs);
      if (call_res) return call_res;
    }

    /*  direct MIR call for known math functions */
    if (ts_emit_direct_math_call(c, name, argc, arg_regs, res)) return res;

    /* Direct compile-time dispatch, otherwise use named runtime helper dispatch. */
    if (ts_emit_direct_resolved_call(c, name, argc, arg_regs, res)) return res;
    ts_emit_runtime_call(c, name, argc, arg_regs, res);
    return res;
  }

  /*  Member call compilation (module.method) */
  case EXPRTK_NODE_MEMBER_CALL: {
    if (!node->data.member_call.object || !node->data.member_call.method)
      return ts_emit_unsupported_node(c, node);
    if (ts_member_call_is_stream_chain(node)) {
      return ts_emit_runtime_value_node(c, node);
    }
    if (node->data.member_call.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_call.object->data.variable.name) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s",
               node->data.member_call.object->data.variable.name,
               node->data.member_call.method);
      if (ts_resolves_runtime_func(c, full_name) || ts_call_name_needs_value_eval(full_name)) {
        return ts_emit_runtime_value_node(c, node);
      }
    }
    if ((ts_call_args_need_ast_eval(c, node->data.member_call.arg_count,
                                    node->data.member_call.args) &&
         !ts_oop_call_args_need_value_bridge(c, node->data.member_call.arg_count,
                                             node->data.member_call.args)) ||
        ts_expr_list_contains_value_call(c, node->data.member_call.arg_count,
                                         node->data.member_call.args)) {
      return ts_emit_runtime_value_node(c, node);
    }
    int object_is_variable = node->data.member_call.object->type == EXPRTK_NODE_VARIABLE;
    const char *obj_name = ts_emit_oop_receiver_to_temp(c, node->data.member_call.object);
    if (!obj_name) return ts_emit_unsupported_node(c, node);

    /* Keep direct module/builtin dispatch, but let unresolved member calls use
     * the OOP-specific IR helper so receiver semantics are preserved. */
    size_t argc = node->data.member_call.arg_count;
    int needs_value_bridge =
        ts_oop_call_args_need_value_bridge(c, argc, node->data.member_call.args);

    MIR_reg_t res = new_temp_reg(c);
    MIR_reg_t arg_regs[16];
    if (!needs_value_bridge) {
      argc = ts_compile_call_args(c, argc, node->data.member_call.args, arg_regs);
    }
    if (object_is_variable && !needs_value_bridge) {
      char full_name[256];
      snprintf(full_name, sizeof(full_name), "%s.%s", obj_name, node->data.member_call.method);
      if (ts_emit_direct_resolved_call(c, full_name, argc, arg_regs, res)) return res;
    }

    const char *exact_class = ts_mir_find_var_class(c, obj_name);
    if (needs_value_bridge) {
      if (exact_class) {
        return ts_emit_oop_member_call_mono_value(c, obj_name, exact_class,
                                                  node->data.member_call.method,
                                                  node->data.member_call.object, node);
      }
      return ts_emit_oop_member_call_value(c, obj_name, node->data.member_call.method,
                                           node->data.member_call.object, node);
    }
    if (exact_class) {
      MIR_reg_t inline_res =
          ts_try_emit_oop_inline_method_call(c, obj_name, exact_class,
                                             node->data.member_call.method, argc, arg_regs);
      if (inline_res) return inline_res;
      int reload_after =
          !ts_oop_member_call_can_skip_reload(c, exact_class, node->data.member_call.method, argc);
      return ts_emit_oop_member_call_cached(c, obj_name, node->data.member_call.method,
                                            node->data.member_call.object, argc, arg_regs,
                                            reload_after);
    }
    return ts_emit_oop_member_call_cached(c, obj_name, node->data.member_call.method,
                                          node->data.member_call.object, argc, arg_regs, 1);
  }

  /* String/vector/map literals need real MIR/helper lowering before JIT can accept them. */
  case EXPRTK_NODE_STRING:
  case EXPRTK_NODE_VECTOR:
  case EXPRTK_NODE_SLICE:
  case EXPRTK_NODE_TEMPLATE_STRING:
  case EXPRTK_NODE_MAP_LITERAL:
  case EXPRTK_NODE_FUNCTION_EXPRESSION:
  case EXPRTK_NODE_SPREAD:
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_TRY_CATCH:
    ts_register_helper_written_vars(c, node);
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_THROW:
    return ts_emit_runtime_value_node(c, node);

  /* Complex assignment/control nodes are rejected unless explicitly lowered above. */
  case EXPRTK_NODE_MEMBER_SET:
    if (node->data.member_set.object &&
        node->data.member_set.object->type == EXPRTK_NODE_VARIABLE &&
        node->data.member_set.member && node->data.member_set.value) {
      return ts_emit_oop_member_set(c, node->data.member_set.object->data.variable.name,
                                    node->data.member_set.member, node->data.member_set.object,
                                    node->data.member_set.value);
    }
    return ts_emit_unsupported_node(c, node);

  case EXPRTK_NODE_DESTRUCTURING_ASSIGNMENT:
    {
      MIR_reg_t res = ts_emit_destructure_assignment(c, node);
      if (res) return res;
    }
    ts_register_destructure_targets(c, node->data.destructuring.targets);
    return ts_emit_runtime_value_node(c, node);

  case EXPRTK_NODE_REST_PARAMETER:
    return ts_emit_unsupported_node(c, node);

  default: {
    return ts_emit_unsupported_node(c, node);
  }
  }
}

/* =========================================================================
 *  Optimized condition branching — emit direct DBXX for comparisons,
 * avoiding the I2D + DBEQ 0.0 round-trip in hot loops.
 * Jumps to false_label when the condition evaluates to false.
 * ========================================================================= */

static void ts_compile_branch_false(ts_mir_compiler_t *c, exprtk_node_t *node,
                                    MIR_label_t false_label) {
  if (!c || c->failed || !node) return;

  /* Binary comparison → single DBXX branch instruction */
  if (node->type == EXPRTK_NODE_BINARY_OP) {
    /* NOT: unary, left is NULL — branch false when operand is true (non-zero) */
    if (node->data.binary.op == exprtk_TOKEN_NOT && node->data.binary.left == NULL) {
      MIR_reg_t val = ts_compile_expr(c, node->data.binary.right);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, val), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    if (ts_binary_is_null_eq_compare(node)) {
      MIR_reg_t cond = ts_emit_runtime_value_node(c, node);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                                   MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    if (node->data.binary.left != NULL) {
      MIR_insn_code_t bop = 0;
      /* Map "condition is false" to the negated branch:
       * a < b  false → a >= b → DBGE
       * a > b  false → a <= b → DBLE
       * a <= b false → a > b  → DBGT
       * a >= b false → a < b  → DBLT
       * a == b false → a != b → DBNE
       * a != b false → a == b → DBEQ */
      switch (node->data.binary.op) {
      case exprtk_TOKEN_LT:
        bop = MIR_DBGE;
        break;
      case exprtk_TOKEN_GT:
        bop = MIR_DBLE;
        break;
      case exprtk_TOKEN_LE:
        bop = MIR_DBGT;
        break;
      case exprtk_TOKEN_GE:
        bop = MIR_DBLT;
        break;
      case exprtk_TOKEN_EQ:
        bop = MIR_DBNE;
        break;
      case exprtk_TOKEN_NE:
        bop = MIR_DBEQ;
        break;
      default:
        break;
      }
      if (bop) {
        MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
        MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, bop, MIR_new_label_op(c->ctx, false_label),
                                     MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
        return;
      }

      /* AND short-circuit: if left is false, jump to false_label;
       * then if right is false, jump to false_label */
      if (node->data.binary.op == exprtk_TOKEN_AND) {
        ts_compile_branch_false(c, node->data.binary.left, false_label);
        ts_compile_branch_false(c, node->data.binary.right, false_label);
        return;
      }

      /* OR short-circuit: if left is true, skip to end (success);
       * then if right is false, jump to false_label */
      if (node->data.binary.op == exprtk_TOKEN_OR) {
        MIR_label_t true_label = MIR_new_label(c->ctx);
        ts_compile_branch_true(c, node->data.binary.left, true_label);
        ts_compile_branch_false(c, node->data.binary.right, false_label);
        MIR_append_insn(c->ctx, c->func, true_label);
        return;
      }
    }
  }

  /* Generic condition: compile expression, branch if == 0.0 */
  MIR_reg_t cond = ts_compile_expr(c, node);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DBEQ, MIR_new_label_op(c->ctx, false_label),
                               MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
}

/*  Branch to true_label when condition is true (helper for OR) */
static void ts_compile_branch_true(ts_mir_compiler_t *c, exprtk_node_t *node,
                                   MIR_label_t true_label) {
  if (!c || c->failed || !node) return;

  if (node->type == EXPRTK_NODE_BINARY_OP && node->data.binary.left != NULL) {
    if (ts_binary_is_null_eq_compare(node)) {
      MIR_reg_t cond = ts_emit_runtime_value_node(c, node);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
      return;
    }

    MIR_insn_code_t bop = 0;
    /* Direct branch when condition IS true */
    switch (node->data.binary.op) {
    case exprtk_TOKEN_LT:
      bop = MIR_DBLT;
      break;
    case exprtk_TOKEN_GT:
      bop = MIR_DBGT;
      break;
    case exprtk_TOKEN_LE:
      bop = MIR_DBLE;
      break;
    case exprtk_TOKEN_GE:
      bop = MIR_DBGE;
      break;
    case exprtk_TOKEN_EQ:
      bop = MIR_DBEQ;
      break;
    case exprtk_TOKEN_NE:
      bop = MIR_DBNE;
      break;
    default:
      break;
    }
    if (bop) {
      MIR_reg_t left = ts_compile_expr(c, node->data.binary.left);
      MIR_reg_t right = ts_compile_expr(c, node->data.binary.right);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, bop, MIR_new_label_op(c->ctx, true_label),
                                   MIR_new_reg_op(c->ctx, left), MIR_new_reg_op(c->ctx, right)));
      return;
    }
  }

  /* Generic condition: compile expression, branch if != 0.0 */
  MIR_reg_t cond = ts_compile_expr(c, node);
  MIR_append_insn(c->ctx, c->func,
                  MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, true_label),
                               MIR_new_reg_op(c->ctx, cond), MIR_new_double_op(c->ctx, 0.0)));
}

/* =========================================================================
 * Statement Compiler (Phase 1-5)
 * ========================================================================= */

static void ts_compile_stmt(ts_mir_compiler_t *c, exprtk_node_t *node) {
  if (!c || c->failed || !node) return;

  switch (node->type) {
  case EXPRTK_NODE_BLOCK:
    for (size_t i = 0; i < node->data.block.count; i++) {
      ts_compile_stmt(c, node->data.block.statements[i]);
    }
    break;

  case EXPRTK_NODE_CLASS_DEF:
    ts_mir_add_class_name(c, node->data.class_def.name);
    ts_emit_oop_define_class(c, node);
    break;

  case EXPRTK_NODE_IF: {
    MIR_label_t else_label = node->data.if_stmt.else_branch ? MIR_new_label(c->ctx) : NULL;
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /*  direct branch — no I2D + DBEQ round-trip */
    ts_compile_branch_false(c, node->data.if_stmt.condition, else_label ? else_label : end_label);

    if (node->data.if_stmt.if_branch) ts_compile_stmt(c, node->data.if_stmt.if_branch);

    if (else_label) {
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));
      MIR_append_insn(c->ctx, c->func, else_label);
      ts_compile_stmt(c, node->data.if_stmt.else_branch);
    }

    MIR_append_insn(c->ctx, c->func, end_label);
    break;
  }

  case EXPRTK_NODE_WHILE: {
    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* Push loop frame for break/continue */
    if (c->loop_depth < MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = loop_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    /*  direct branch */
    ts_compile_branch_false(c, node->data.while_loop.condition, end_label);

    if (node->data.while_loop.body) ts_compile_stmt(c, node->data.while_loop.body);

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  case EXPRTK_NODE_FOR: {
    if (node->data.for_loop.init) ts_compile_stmt(c, node->data.for_loop.init);

    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t continue_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* Push loop frame: continue jumps to post, break jumps to end */
    if (c->loop_depth < MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = continue_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    if (node->data.for_loop.condition) {
      /*  direct branch */
      ts_compile_branch_false(c, node->data.for_loop.condition, end_label);
    }

    if (node->data.for_loop.body) ts_compile_stmt(c, node->data.for_loop.body);

    MIR_append_insn(c->ctx, c->func, continue_label);
    if (node->data.for_loop.post) ts_compile_stmt(c, node->data.for_loop.post);

    MIR_append_insn(c->ctx, c->func,
                    MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  /*  do-while loop */
  case EXPRTK_NODE_DO_WHILE: {
    MIR_label_t loop_label = MIR_new_label(c->ctx);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    if (c->loop_depth < MAX_LOOP_DEPTH) {
      c->loop_stack[c->loop_depth].break_label = end_label;
      c->loop_stack[c->loop_depth].continue_label = loop_label;
      c->loop_depth++;
    }

    MIR_append_insn(c->ctx, c->func, loop_label);

    if (node->data.do_while.body) ts_compile_stmt(c, node->data.do_while.body);

    /*  direct branch back when condition is true */
    ts_compile_branch_true(c, node->data.do_while.condition, loop_label);

    MIR_append_insn(c->ctx, c->func, end_label);

    if (c->loop_depth > 0) c->loop_depth--;
    break;
  }

  /*  break/continue/return */
  case EXPRTK_NODE_FLOW: {
    if (node->data.flow.type == exprtk_TOKEN_BREAK && c->loop_depth > 0) {
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_JMP,
                       MIR_new_label_op(c->ctx, c->loop_stack[c->loop_depth - 1].break_label)));
    } else if (node->data.flow.type == exprtk_TOKEN_CONTINUE && c->loop_depth > 0) {
      MIR_append_insn(
          c->ctx, c->func,
          MIR_new_insn(c->ctx, MIR_JMP,
                       MIR_new_label_op(c->ctx, c->loop_stack[c->loop_depth - 1].continue_label)));
    } else if (node->data.flow.type == exprtk_TOKEN_RETURN) {
      /* Emit epilogue (store vars back) then return */
      MIR_reg_t ret_val;
      if (node->data.flow.value) {
        ret_val = ts_compile_expr(c, node->data.flow.value);
      } else {
        ret_val = new_temp_reg(c);
        MIR_append_insn(c->ctx, c->func,
                        MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, ret_val),
                                     MIR_new_double_op(c->ctx, 0.0)));
      }
      /* Store all variables back to env before returning (skip in compiled functions) */
      if (c->ctx_reg) {
        for (int i = 0; i < c->var_count; i++) {
          if (c->vars[i]->dynamic_value) continue;
          if (!c->vars[i]->dirty) continue;
          MIR_append_insn(
              c->ctx, c->func,
                                MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                MIR_new_reg_op(c->ctx, c->ctx_reg),
                                MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
        }
      }
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_ret_insn(c->ctx, 1, MIR_new_reg_op(c->ctx, ret_val)));
    }
    break;
  }

  /* Phase 6+9: Function definition — already compiled in pre-scan, just register in env */
  case EXPRTK_NODE_FUNCTION_DEFINITION: {
    /* The MIR function was already compiled during ts_prescan_functions.
     * Register it in env so runtime helper dispatch and reflection see it. */
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 4, MIR_new_ref_op(c->ctx, c->ext.define_func_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.define_func_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)node)));
    break;
  }

  /* For-in: native compilation for pre-bound vectors; other collections are unsupported. */
  case EXPRTK_NODE_FOR_IN: {
    const char *iter_name = node->data.for_in.var_name;
    exprtk_node_t *collection = node->data.for_in.collection;
    const char *vec_name = NULL;
    int native = 0;

    if (collection && collection->type == EXPRTK_NODE_VARIABLE)
      vec_name = collection->data.variable.name;

    if (vec_name && c->ctx_reg) {
      exprtk_value_t existing = exprtk_env_get(&c->ts_ctx->env, vec_name);
      if (existing.type == EXPRTK_VAL_VECTOR && existing.data.vector.data) {
        native = 1;
        int64_t len = (int64_t)existing.data.vector.size;

        /* Reuse Phase 10 vec_data pointer cache */
        MIR_reg_t ptr_reg = ts_mir_get_or_add_vec_ptr(c, vec_name);

        if (ptr_reg) {
          MIR_reg_t iter_reg = get_or_create_reg(c, iter_name);
          MIR_reg_t i_reg = new_temp_ireg(c);

          /* i = 0 */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_MOV, MIR_new_reg_op(c->ctx, i_reg),
                                       MIR_new_int_op(c->ctx, 0)));

          MIR_label_t loop_label = MIR_new_label(c->ctx);
          MIR_label_t continue_label = MIR_new_label(c->ctx);
          MIR_label_t end_label = MIR_new_label(c->ctx);

          if (c->loop_depth < MAX_LOOP_DEPTH) {
            c->loop_stack[c->loop_depth].break_label = end_label;
            c->loop_stack[c->loop_depth].continue_label = continue_label;
            c->loop_depth++;
          }

          MIR_append_insn(c->ctx, c->func, loop_label);

          /* if (i >= len) goto end */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_BGE, MIR_new_label_op(c->ctx, end_label),
                                       MIR_new_reg_op(c->ctx, i_reg), MIR_new_int_op(c->ctx, len)));

          /* iter_var = data[i] — native memory load */
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_DMOV, MIR_new_reg_op(c->ctx, iter_reg),
                                       MIR_new_mem_op(c->ctx, MIR_T_D, 0, ptr_reg, i_reg, 8)));

          /* compile body */
          if (node->data.for_in.body) ts_compile_stmt(c, node->data.for_in.body);

          /* continue target: i++ */
          MIR_append_insn(c->ctx, c->func, continue_label);
          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_ADD, MIR_new_reg_op(c->ctx, i_reg),
                                       MIR_new_reg_op(c->ctx, i_reg), MIR_new_int_op(c->ctx, 1)));

          MIR_append_insn(c->ctx, c->func,
                          MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, loop_label)));
          MIR_append_insn(c->ctx, c->func, end_label);

          if (c->loop_depth > 0) c->loop_depth--;
        } else {
          native = 0;
        }
      }
    }

    if (!native) {
      if (iter_name) (void)get_or_create_reg(c, iter_name);
      ts_register_helper_written_vars(c, node->data.for_in.body);
      (void)ts_emit_runtime_value_node(c, node);
    }
    break;
  }

  /*  Switch statement — native MIR (chain of compare-and-branch) */
  case EXPRTK_NODE_SWITCH: {
    MIR_reg_t val = ts_compile_expr(c, node->data.switch_stmt.value);
    MIR_label_t end_label = MIR_new_label(c->ctx);

    /* cases array: [case_val0, body0, case_val1, body1, ...], length = case_count * 2 */
    size_t num_cases = node->data.switch_stmt.case_count;
    for (size_t i = 0; i < num_cases; i++) {
      exprtk_node_t *case_val = node->data.switch_stmt.cases[i * 2];
      exprtk_node_t *case_body = node->data.switch_stmt.cases[i * 2 + 1];
      MIR_label_t next_case = MIR_new_label(c->ctx);

      MIR_reg_t cv = ts_compile_expr(c, case_val);
      /* If val != cv, skip to next case */
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_DBNE, MIR_new_label_op(c->ctx, next_case),
                                   MIR_new_reg_op(c->ctx, val), MIR_new_reg_op(c->ctx, cv)));

      ts_compile_stmt(c, case_body);
      MIR_append_insn(c->ctx, c->func,
                      MIR_new_insn(c->ctx, MIR_JMP, MIR_new_label_op(c->ctx, end_label)));

      MIR_append_insn(c->ctx, c->func, next_case);
    }

    /* Default case */
    if (node->data.switch_stmt.default_case) {
      ts_compile_stmt(c, node->data.switch_stmt.default_case);
    }

    MIR_append_insn(c->ctx, c->func, end_label);
    break;
  }

  default:
    ts_compile_expr(c, node);
    break;
  }
}

/* =========================================================================
 *  External Call Setup
 * ========================================================================= */

static void ts_setup_externals(ts_mir_compiler_t *c) {
  MIR_context_t ctx = c->ctx;

  /* fmod(double, double) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    c->ext.fmod_proto = MIR_new_proto_arr(ctx, "p_fmod", 1, &res, 2, args);
    c->ext.fmod_import = MIR_new_import(ctx, "fmod");
  }

  /* pow(double, double) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
    c->ext.pow_proto = MIR_new_proto_arr(ctx, "p_pow", 1, &res, 2, args);
    c->ext.pow_import = MIR_new_import(ctx, "pow");
  }

  /* ts_mir_load_var(void *ctx, const char *name) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.load_var_proto = MIR_new_proto_arr(ctx, "p_load_var", 1, &res, 2, args);
    c->ext.load_var_import = MIR_new_import(ctx, "ts_mir_load_var");
  }

  /* ts_mir_store_var(void *ctx, const char *name, double value) -> void */
  {
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "value", 0}};
    c->ext.store_var_proto = MIR_new_proto_arr(ctx, "p_store_var", 0, NULL, 3, args);
    c->ext.store_var_import = MIR_new_import(ctx, "ts_mir_store_var");
  }

  /* ts_mir_assign_var(void *ctx, const char *target, const char *source) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "source", 0}};
    c->ext.assign_var_proto = MIR_new_proto_arr(ctx, "p_assign_var", 1, &res, 3, args);
    c->ext.assign_var_import = MIR_new_import(ctx, "ts_mir_assign_var");
  }

  /* ts_mir_call0(void *ctx, const char *name) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.call0_proto = MIR_new_proto_arr(ctx, "p_call0", 1, &res, 2, args);
    c->ext.call0_import = MIR_new_import(ctx, "ts_mir_call0");
  }

  /* ts_mir_call1(void *ctx, const char *name, double a0) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "a0", 0}};
    c->ext.call1_proto = MIR_new_proto_arr(ctx, "p_call1", 1, &res, 3, args);
    c->ext.call1_import = MIR_new_import(ctx, "ts_mir_call1");
  }

  /* ts_mir_call2(void *ctx, const char *name, double a0, double a1) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "a0", 0}, {MIR_T_D, "a1", 0}};
    c->ext.call2_proto = MIR_new_proto_arr(ctx, "p_call2", 1, &res, 4, args);
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
    c->ext.call3_proto = MIR_new_proto_arr(ctx, "p_call3", 1, &res, 5, args);
    c->ext.call3_import = MIR_new_import(ctx, "ts_mir_call3");
  }

  /* ts_mir_calln(void *ctx, const char *name, int64_t argc, double *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.calln_proto = MIR_new_proto_arr(ctx, "p_calln", 1, &res, 4, args);
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
    c->ext.call_assign_proto = MIR_new_proto_arr(ctx, "p_call_assign", 1, &res, 5, args);
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
        MIR_new_proto_arr(ctx, "p_call_value_assign", 1, &res, 4, args);
    c->ext.call_value_assign_import = MIR_new_import(ctx, "ts_mir_call_value_assign");
  }
  /* ts_mir_value_expr(void *ctx, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.value_expr_proto = MIR_new_proto_arr(ctx, "p_value_expr", 1, &res, 2, args);
    c->ext.value_expr_import = MIR_new_import(ctx, "ts_mir_value_expr");
  }
  /* ts_mir_value_expr_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "node", 0}};
    c->ext.value_expr_assign_proto =
        MIR_new_proto_arr(ctx, "p_value_expr_assign", 1, &res, 3, args);
    c->ext.value_expr_assign_import = MIR_new_import(ctx, "ts_mir_value_expr_assign");
  }
  /* ts_mir_await_value(void *ctx, void *arg_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "arg_node", 0}};
    c->ext.await_value_proto = MIR_new_proto_arr(ctx, "p_await_value", 1, &res, 2, args);
    c->ext.await_value_import = MIR_new_import(ctx, "ts_mir_await_value");
  }
  /* ts_mir_await_assign(void *ctx, const char *target, void *arg_node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "arg_node", 0}};
    c->ext.await_assign_proto = MIR_new_proto_arr(ctx, "p_await_assign", 1, &res, 3, args);
    c->ext.await_assign_import = MIR_new_import(ctx, "ts_mir_await_assign");
  }
  /* ts_mir_function_expr_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "node", 0}};
    c->ext.function_expr_assign_proto =
        MIR_new_proto_arr(ctx, "p_function_expr_assign", 1, &res, 3, args);
    c->ext.function_expr_assign_import = MIR_new_import(ctx, "ts_mir_function_expr_assign");
  }
  /* ts_mir_try_catch_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "node", 0}};
    c->ext.try_catch_assign_proto =
        MIR_new_proto_arr(ctx, "p_try_catch_assign", 1, &res, 3, args);
    c->ext.try_catch_assign_import = MIR_new_import(ctx, "ts_mir_try_catch_assign");
  }

  /* ts_mir_vec_get(void *ctx, const char *name, double index) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_D, "index", 0}};
    c->ext.vec_get_proto = MIR_new_proto_arr(ctx, "p_vec_get", 1, &res, 3, args);
    c->ext.vec_get_import = MIR_new_import(ctx, "ts_mir_vec_get");
  }

  /* ts_mir_vector_assign(void *ctx, const char *target, int64_t count, double *values) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_I64, "count", 0},
                         {MIR_T_P, "values", 0}};
    c->ext.vec_assign_proto = MIR_new_proto_arr(ctx, "p_vec_assign", 1, &res, 4, args);
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
    c->ext.destruct_var_proto = MIR_new_proto_arr(ctx, "p_destruct_var", 1, &res, 4, args);
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
        MIR_new_proto_arr(ctx, "p_destruct_vector", 1, &res, 5, args);
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
    c->ext.string_assign_proto = MIR_new_proto_arr(ctx, "p_string_assign", 1, &res, 4, args);
    c->ext.string_assign_import = MIR_new_import(ctx, "ts_mir_string_assign");
  }

  /* ts_mir_template_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "target", 0},
                         {MIR_T_P, "node", 0}};
    c->ext.template_assign_proto =
        MIR_new_proto_arr(ctx, "p_template_assign", 1, &res, 3, args);
    c->ext.template_assign_import = MIR_new_import(ctx, "ts_mir_template_assign");
  }

  /* ts_mir_define_func(void *ctx, void *node) -> void */
  {
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.define_func_proto = MIR_new_proto_arr(ctx, "p_define_func", 0, NULL, 2, args);
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
    c->ext.member_get_proto = MIR_new_proto_arr(ctx, "p_member_get", 1, &res, 5, args);
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
        MIR_new_proto_arr(ctx, "p_member_get_assign", 1, &res, 6, args);
    c->ext.member_get_assign_import = MIR_new_import(ctx, "ts_mir_member_get_assign");
  }

  /* ts_mir_vec_data(void *ctx, const char *name) -> void* (returned as i64) */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}};
    c->ext.vec_data_proto = MIR_new_proto_arr(ctx, "p_vec_data", 1, &res, 2, args);
    c->ext.vec_data_import = MIR_new_import(ctx, "ts_mir_vec_data");
  }

  /* ts_mir_map_get_key(void *ctx, const char *obj_name, const char *key) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "key", 0}};
    c->ext.map_get_key_proto = MIR_new_proto_arr(ctx, "p_map_get_key", 1, &res, 3, args);
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
    c->ext.map_assign_proto = MIR_new_proto_arr(ctx, "p_map_assign", 1, &res, 5, args);
    c->ext.map_assign_import = MIR_new_import(ctx, "ts_mir_map_assign");
  }

  /* ts_mir_map_value_assign(void *ctx, const char *target, void *node) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target", 0}, {MIR_T_P, "node", 0}};
    c->ext.map_value_assign_proto =
        MIR_new_proto_arr(ctx, "p_map_value_assign", 1, &res, 3, args);
    c->ext.map_value_assign_import = MIR_new_import(ctx, "ts_mir_map_value_assign");
  }

  /* ts_mir_map_num_ptr(void *ctx, const char *obj_name, const char *key) -> void* (as i64) */
  {
    MIR_type_t res = MIR_T_I64;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "obj_name", 0}, {MIR_T_P, "key", 0}};
    c->ext.map_num_ptr_proto = MIR_new_proto_arr(ctx, "p_map_num_ptr", 1, &res, 3, args);
    c->ext.map_num_ptr_import = MIR_new_import(ctx, "ts_mir_map_num_ptr");
  }

  /* ts_mir_load_captured_var(void *ctx, const char *name, void *closure_env) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "name", 0}, {MIR_T_P, "closure_env", 0}};
    c->ext.load_captured_proto = MIR_new_proto_arr(ctx, "p_load_captured", 1, &res, 3, args);
    c->ext.load_captured_import = MIR_new_import(ctx, "ts_mir_load_captured_var");
  }

  /*  Direct math function imports — double f(double) */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t a1[1] = {{MIR_T_D, "x", 0}};
#define MATH1(NAME, PNAME)                                                                         \
  c->ext.NAME##_proto = MIR_new_proto_arr(ctx, "p_" #NAME, 1, &res, 1, a1);                        \
  c->ext.NAME##_import = MIR_new_import(ctx, PNAME)
    MATH1(sin, "sin");
    MATH1(cos, "cos");
    MATH1(sqrt, "sqrt");
    MATH1(fabs, "fabs");
    MATH1(floor, "floor");
    MATH1(ceil, "ceil");
    MATH1(log, "log");
    MATH1(exp, "exp");
    MATH1(round, "round");
    MATH1(tan, "tan");
    MATH1(asin, "asin");
    MATH1(acos, "acos");
    MATH1(atan, "atan");
#undef MATH1
  }
  /*  double f(double, double) — fmax, fmin, atan2 */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t a2[2] = {{MIR_T_D, "a", 0}, {MIR_T_D, "b", 0}};
#define MATH2(NAME, PNAME)                                                                         \
  c->ext.NAME##_proto = MIR_new_proto_arr(ctx, "p_" #NAME, 1, &res, 2, a2);                        \
  c->ext.NAME##_import = MIR_new_import(ctx, PNAME)
    MATH2(fmax, "fmax");
    MATH2(fmin, "fmin");
    MATH2(atan2, "atan2");
#undef MATH2
  }

  /*  ts_mir_call_native(void *ctx, void *fn, void *ud, i64 argc, void *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[5] = {{MIR_T_P, "ctx", 0},
                         {MIR_T_P, "fn", 0},
                         {MIR_T_P, "ud", 0},
                         {MIR_T_I64, "argc", 0},
                         {MIR_T_P, "argv", 0}};
    c->ext.call_native_proto = MIR_new_proto_arr(ctx, "p_call_native", 1, &res, 5, args);
    c->ext.call_native_import = MIR_new_import(ctx, "ts_mir_call_native");
  }
  /*  ts_mir_call_builtin(void *ctx, void *fn, i64 argc, void *argv) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[4] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "fn", 0}, {MIR_T_I64, "argc", 0}, {MIR_T_P, "argv", 0}};
    c->ext.call_builtin_proto = MIR_new_proto_arr(ctx, "p_call_builtin", 1, &res, 4, args);
    c->ext.call_builtin_import = MIR_new_import(ctx, "ts_mir_call_builtin");
  }

  /* ts_mir_oop_define_class(void *ctx, void *node) -> void */
  {
    MIR_var_t args[2] = {{MIR_T_P, "ctx", 0}, {MIR_T_P, "node", 0}};
    c->ext.oop_define_class_proto =
        MIR_new_proto_arr(ctx, "p_oop_define_class", 0, NULL, 2, args);
    c->ext.oop_define_class_import = MIR_new_import(ctx, "ts_mir_oop_define_class");
  }

  /* ts_mir_oop_class_alias(void *ctx, const char *target, const char *source) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "target_name", 0}, {MIR_T_P, "source_name", 0}};
    c->ext.oop_alias_class_proto =
        MIR_new_proto_arr(ctx, "p_oop_class_alias", 1, &res, 3, args);
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
        MIR_new_proto_arr(ctx, "p_oop_new_assign", 1, &res, 5, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_call", 1, &res, 6, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_call_cached", 1, &res, 7, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_call_assign", 1, &res, 8, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_call_value", 1, &res, 5, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_call_mono_value", 1, &res, 6, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_call_assign_value", 1, &res, 6, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_get", 1, &res, 5, args);
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
        MIR_new_proto_arr(ctx, "p_oop_member_set", 1, &res, 6, args);
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
    c->ext.oop_num_ptr_proto = MIR_new_proto_arr(ctx, "p_oop_num_ptr", 1, &res, 6, args);
    c->ext.oop_num_ptr_import = MIR_new_import(ctx, "ts_mir_oop_num_ptr");
  }

  /* ts_mir_oop_instanceof(void *ctx, const char *object, const char *class) -> double */
  {
    MIR_type_t res = MIR_T_D;
    MIR_var_t args[3] = {
        {MIR_T_P, "ctx", 0}, {MIR_T_P, "object_name", 0}, {MIR_T_P, "class_name", 0}};
    c->ext.oop_instanceof_proto =
        MIR_new_proto_arr(ctx, "p_oop_instanceof", 1, &res, 3, args);
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
        MIR_new_proto_arr(ctx, "p_oop_predicate", 1, &res, 5, args);
    c->ext.oop_predicate_import = MIR_new_import(ctx, "ts_mir_oop_predicate");
  }
}

static void ts_load_externals(MIR_context_t ctx) {
  MIR_load_external(ctx, "fmod", (void *)fmod);
  MIR_load_external(ctx, "pow", (void *)pow);
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

/* =========================================================================
 *  Variable Prologue/Epilogue Emission
 * ========================================================================= */

static void ts_emit_var_prologue(ts_mir_compiler_t *c) {
  /* Prepend load_var calls at the beginning of the function (after func entry).
   * Insert in reverse order so they end up in the correct order. */
  for (int i = c->var_count - 1; i >= 0; i--) {
    if (c->vars[i]->dynamic_value) continue;
    MIR_prepend_insn(c->ctx, c->func,
                     MIR_new_call_insn(
                         c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.load_var_proto),
                         MIR_new_ref_op(c->ctx, c->ext.load_var_import),
                         MIR_new_reg_op(c->ctx, c->vars[i]->reg),
                         MIR_new_reg_op(c->ctx, c->ctx_reg),
                         MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name)));
  }
}

static void ts_emit_var_epilogue(ts_mir_compiler_t *c) {
  /* Only numeric MIR writes are dirty. Helper-evaluated values may be strings,
   * maps, class values, or instances already stored in env. */
  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    if (!c->vars[i]->dirty) continue;
    MIR_append_insn(c->ctx, c->func,
                    MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.store_var_proto),
                                      MIR_new_ref_op(c->ctx, c->ext.store_var_import),
                                      MIR_new_reg_op(c->ctx, c->ctx_reg),
                                      MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vars[i]->name),
                                      MIR_new_reg_op(c->ctx, c->vars[i]->reg)));
  }
}

/*  Prepend vec_data pointer loads at function start */
static void ts_emit_vec_prologue(ts_mir_compiler_t *c) {
  for (int i = c->vec_ptr_count - 1; i >= 0; i--) {
    MIR_prepend_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 5, MIR_new_ref_op(c->ctx, c->ext.vec_data_proto),
                          MIR_new_ref_op(c->ctx, c->ext.vec_data_import),
                          MIR_new_reg_op(c->ctx, c->vec_ptrs[i].ptr_reg),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->vec_ptrs[i].name)));
  }
}

/*  Prepend map_num_ptr pointer loads at function start */
static void ts_emit_map_prologue(ts_mir_compiler_t *c) {
  for (int i = c->map_ptr_count - 1; i >= 0; i--) {
    MIR_prepend_insn(
        c->ctx, c->func,
        MIR_new_call_insn(c->ctx, 6, MIR_new_ref_op(c->ctx, c->ext.map_num_ptr_proto),
                          MIR_new_ref_op(c->ctx, c->ext.map_num_ptr_import),
                          MIR_new_reg_op(c->ctx, c->map_ptrs[i].ptr_reg),
                          MIR_new_reg_op(c->ctx, c->ctx_reg),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->map_ptrs[i].obj_name),
                          MIR_new_uint_op(c->ctx, (uint64_t)(uintptr_t)c->map_ptrs[i].key_name)));
  }
}

/* =========================================================================
 * Public API: Compile, Exec, Run
 * ========================================================================= */

static int turbo_script_compile_mir_backend(turbo_script_ctx_t *ctx, const char *script,
                                            int use_interp) {
  uint64_t start_time = 0;
  MIR_context_t mir_ctx = NULL;

  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT compile error: script is NULL");
    return -1;
  }
  
  // 开始计时（如果启用统计）
  if (ctx->jit_stats_enabled) {
    start_time = ts_get_time_us();
  }
  
  if (use_interp) {
    if (!ctx->mir_interp_ctx) ctx->mir_interp_ctx = MIR_init();
    mir_ctx = ctx->mir_interp_ctx;
    ctx->mir_interp_last_func = NULL;
  } else {
    if (!ctx->mir_ctx) ctx->mir_ctx = MIR_init();
    mir_ctx = ctx->mir_ctx;
    ctx->mir_last_fn = NULL;
  }

  exprtk_node_t *ast = turbo_script_parse_with_error(ctx, script);
  if (!ast) return -1;

  if (exprtk_validate(ast, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    ctx->error_code = TURBO_SCRIPT_ERROR_VALIDATE;
    exprtk_free(ast);
    return -1;
  }

  char mod_name[64];
  snprintf(mod_name, sizeof(mod_name), "ts_jit_mod_%d", ctx->mir_mod_idx++);

  MIR_module_t mod = MIR_new_module(mir_ctx, mod_name);

  ts_mir_compiler_t compiler = {0};
  compiler.ctx = mir_ctx;
  compiler.module = mod;
  compiler.ts_ctx = ctx;
  compiler.ast_root = ast;
  snprintf(compiler.item_prefix, sizeof(compiler.item_prefix), "%s", mod_name);

  /*  Setup external call prototypes and imports (before func) */
  ts_setup_externals(&compiler);

  /* Class names are needed before function pre-compilation so functions that
   * return OOP values stay on the value-preserving runtime-call path. */
  ts_prescan_class_names(&compiler, ast);

  /*  Pre-compile script functions already registered in env */
  for (exprtk_func_t *f = ctx->env.funcs; f; f = f->next) {
    if (f->is_script && f->data.script.body && f->data.script.arg_count <= 16) {
      int all_vars = 1;
      for (size_t i = 0; i < f->data.script.arg_count; i++) {
        if (f->data.script.arg_params[i]->type != EXPRTK_NODE_VARIABLE) {
          all_vars = 0;
          break;
        }
      }
      if (all_vars) {
        ts_compile_script_func(&compiler, f->name, f->data.script.arg_params,
                               f->data.script.arg_count, f->data.script.body);
      }
    }
  }

  /*  Pre-scan AST for function definitions and compile them */
  ts_prescan_functions(&compiler, ast);

  /* Pre-compile monomorphic higher-order calls after ordinary functions exist. */
  ts_prescan_hof_specializations(&compiler, ast);

  /*  Function signature: double main(void *ctx_ptr) */
  MIR_type_t res_type = MIR_T_D;
  MIR_var_t func_args[1] = {{MIR_T_P, "ctx_ptr", 0}};
  char main_name[128];
  snprintf(main_name, sizeof(main_name), "%s_main", compiler.item_prefix);
  MIR_item_t func = MIR_new_func_arr(mir_ctx, main_name, 1, &res_type, 1, func_args);
  compiler.func = func;

  /*  Get the ctx_ptr register */
  compiler.ctx_reg = MIR_reg(mir_ctx, "ctx_ptr", func->u.func);

  ts_prescan_variables(&compiler, ast);

  /* Compile the AST */
  ts_compile_stmt(&compiler, ast);

  /*  Emit prologue — prepend load_var calls at function start
   * (must be after compile so we know which variables exist) */
  ts_emit_var_prologue(&compiler);

  /*  Prepend vec_data pointer loads (after var prologue, so they run first) */
  ts_emit_vec_prologue(&compiler);

  /*  Prepend map field pointer loads */
  ts_emit_map_prologue(&compiler);

  /*  Emit epilogue — store all variables back to env */
  ts_emit_var_epilogue(&compiler);

  /* Default return 0.0 */
  char ret_name[32];
  snprintf(ret_name, sizeof(ret_name), "_t%d", compiler.tmp_count++);
  MIR_reg_t ret_reg = MIR_new_func_reg(mir_ctx, func->u.func, MIR_T_D, ret_name);
  MIR_append_insn(mir_ctx, func,
                  MIR_new_insn(mir_ctx, MIR_DMOV, MIR_new_reg_op(mir_ctx, ret_reg),
                               MIR_new_double_op(mir_ctx, 0.0)));
  MIR_append_insn(mir_ctx, func,
                  MIR_new_ret_insn(mir_ctx, 1, MIR_new_reg_op(mir_ctx, ret_reg)));

  MIR_finish_func(mir_ctx);
  MIR_finish_module(mir_ctx);

  if (compiler.failed) {
    exprtk_free(ast);
    ts_mir_destroy_compiler_storage(&compiler);
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE)
      ctx->error_code = TURBO_SCRIPT_ERROR_JIT;
    return -1;
  }

  /*  Keep AST alive -- don't free it. Node pointers are baked into JIT code.
   * Variable name strings and monomorphic OOP class strings are also baked in as pointer
   * immediates for helper calls. We intentionally keep them alive for the MIR context
   * lifetime. */
  /* Track the compiled AST in the context to be freed on teardown */
  if (ctx->compiled_ast_count >= ctx->compiled_ast_capacity) {
    size_t new_cap = ctx->compiled_ast_capacity == 0 ? 16 : ctx->compiled_ast_capacity * 2;
    exprtk_node_t **new_asts = (exprtk_node_t **)realloc(ctx->compiled_asts, new_cap * sizeof(exprtk_node_t *));
    if (new_asts) {
      ctx->compiled_asts = new_asts;
      ctx->compiled_ast_capacity = new_cap;
    }
  }
  if (ctx->compiled_ast_count < ctx->compiled_ast_capacity) {
    ctx->compiled_asts[ctx->compiled_ast_count++] = ast;
  }

  MIR_load_module(mir_ctx, mod);

  if (use_interp) {
    if (!ctx->mir_interp_externals_loaded) {
      ts_load_externals(mir_ctx);
      ctx->mir_interp_externals_loaded = 1;
    }
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);
    ctx->mir_interp_last_func = func;
  } else {
    /*  Reuse gen context across compiles — init once, finish in turbo_script_free */
    if (!ctx->mir_gen_initialized) {
      MIR_gen_init(mir_ctx);
      ts_load_externals(mir_ctx);
      ctx->mir_gen_initialized = 1;
    }
    MIR_link(mir_ctx, MIR_set_gen_interface, NULL);

    /*  Cache the compiled function pointer for fast exec_jit */
    ctx->mir_last_fn = func->addr;
  }

  ts_mir_destroy_compiler_storage(&compiler);
  
  // 记录编译统计
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.compile_count++;
    ctx->jit_stats.total_compile_time_us += ts_get_time_us() - start_time;
  }
  
  return 0;
}

CXX_C_API int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, 0);
}

CXX_C_API int turbo_script_compile_mir_interp(turbo_script_ctx_t *ctx, const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, 1);
}

static void ts_jit_reset_runtime_state(turbo_script_ctx_t *ctx) {
  ctx->env.aborted = 0;
  ctx->env.flow = exprtk_FLOW_NORMAL;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;
  ctx->env.error_msg[0] = '\0';
}

static int ts_jit_finish_runtime_state(turbo_script_ctx_t *ctx) {
  if (ctx->env.aborted || ctx->env.flow == exprtk_FLOW_THROW) {
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE) {
      ctx->error_code = TURBO_SCRIPT_ERROR_RUNTIME;
    }
    if (ctx->error_msg[0] == '\0') {
      const char *msg = ctx->env.error_msg[0] ? ctx->env.error_msg : "JIT runtime error";
      snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", msg);
    }
    return -1;
  }
  return 0;
}

static int ts_jit_exec_fn(turbo_script_ctx_t *ctx, void *fn_ptr) {
  typedef double (*jit_fn_t)(void *);

  if (!ctx || !fn_ptr) return -1;
  ts_jit_reset_runtime_state(ctx);
  ((jit_fn_t)fn_ptr)((void *)ctx);
  return ts_jit_finish_runtime_state(ctx);
}

CXX_C_API int turbo_script_exec_jit(turbo_script_ctx_t *ctx) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ctx->mir_last_fn) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT exec error: no compiled module");
    return -1;
  }

  /*  Direct call via cached pointer — no module list traversal */
  return ts_jit_exec_fn(ctx, ctx->mir_last_fn);
}

CXX_C_API int turbo_script_exec_mir_interp_result(turbo_script_ctx_t *ctx, double *result_out) {
  MIR_val_t args[1];
  MIR_val_t result;

  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ctx->mir_interp_ctx || !ctx->mir_interp_last_func) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "MIR interp exec error: no compiled module");
    return -1;
  }

  ts_jit_reset_runtime_state(ctx);
  memset(&result, 0, sizeof(result));
  args[0].a = (void *)ctx;
  MIR_interp_arr(ctx->mir_interp_ctx, ctx->mir_interp_last_func, &result, 1, args);
  if (result_out) *result_out = result.d;
  return ts_jit_finish_runtime_state(ctx);
}

CXX_C_API int turbo_script_exec_mir_interp(turbo_script_ctx_t *ctx) {
  return turbo_script_exec_mir_interp_result(ctx, NULL);
}

CXX_C_API int turbo_script_run_mir_interp(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "MIR interp run error: script is NULL");
    return -1;
  }

  if (turbo_script_compile_mir_interp(ctx, script) != 0) return -1;
  return turbo_script_exec_mir_interp(ctx);
}

/* =========================================================================
 *  FNV-1a hash for compile cache
 * ========================================================================= */

static uint64_t ts_hash_script(const char *s) {
  uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
  size_t len = 0;
  for (; *s; s++, len++) {
    h ^= (uint8_t)*s;
    h *= 0x100000001b3ULL;  // FNV-1a prime
  }
  h ^= len;  // 混合长度，减少短脚本冲突
  return h;
}

CXX_C_API int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script) {
  uint64_t exec_start_time = 0;
  
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT run error: script is NULL");
    return -1;
  }

  /*  Check compile cache — skip parse/compile on hit */
  uint64_t hash = ts_hash_script(script);
  uint32_t slot = (uint32_t)(hash % TS_JIT_CACHE_SIZE);
  if (ctx->jit_cache[slot].hash == hash &&
      ctx->jit_cache[slot].script &&
      strcmp(ctx->jit_cache[slot].script, script) == 0 &&
      ctx->jit_cache[slot].fn_ptr) {
    ctx->jit_cache[slot].access_count++;  // 更新 LRU 访问计数
    
    // 记录缓存命中统计
    if (ctx->jit_stats_enabled) {
      ctx->jit_stats.cache_hit_count++;
      ctx->jit_stats.exec_count++;
      exec_start_time = ts_get_time_us();
    }
    
    int result = ts_jit_exec_fn(ctx, ctx->jit_cache[slot].fn_ptr);
    
    // 记录执行时间
    if (ctx->jit_stats_enabled) {
      ctx->jit_stats.total_exec_time_us += ts_get_time_us() - exec_start_time;
    }
    
    return result;
  }

  // 记录缓存未命中
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.cache_miss_count++;
  }

  /* Cache miss — full compile. Unsupported scripts must fail instead of
   * silently running through the interpreter. */
  if (turbo_script_compile_mir(ctx, script) != 0) return -1;

  /* Store in cache */
  free(ctx->jit_cache[slot].script);
  ctx->jit_cache[slot].script = strdup(script);
  ctx->jit_cache[slot].hash = hash;
  ctx->jit_cache[slot].fn_ptr = ctx->mir_last_fn;
  ctx->jit_cache[slot].access_count = 1;  // 初始化访问计数

  // 记录执行统计
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.exec_count++;
    exec_start_time = ts_get_time_us();
  }
  
  int result = turbo_script_exec_jit(ctx);
  
  // 记录执行时间
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.total_exec_time_us += ts_get_time_us() - exec_start_time;
  }
  
  return result;
}
