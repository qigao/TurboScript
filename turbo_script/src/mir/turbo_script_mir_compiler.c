/**
 * @file turbo_script_mir_compiler.c
 * @brief Core compiler infrastructure for the TurboScript MIR backend.
 *
 * This module owns backend-wide helpers that are already used by other MIR
 * modules during the incremental split from turbo_script_mir.c.
 */

#include "turbo_script_mir_internal.h"
#include <mir.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ts_mir_fail(ts_mir_compiler_t *c, const char *fmt, ...) {
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

MIR_reg_t ts_mir_new_temp_reg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_t%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_D, name);
}

MIR_reg_t ts_mir_new_temp_ireg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_i%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_I64, name);
}

MIR_reg_t ts_mir_new_temp_preg(ts_mir_compiler_t *c) {
  char name[32];
  snprintf(name, sizeof(name), "_p%d", c->tmp_count++);
  return MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_I64, name);
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

MIR_reg_t ts_mir_get_or_create_reg(ts_mir_compiler_t *c, const char *name) {
  ts_mir_var_entry_t *entry = NULL;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) return c->vars[i]->reg;
  }

  if (!ts_mir_ensure_var_capacity(c, c->var_count + 1))
    return ts_mir_new_temp_reg(c);

  entry = (ts_mir_var_entry_t *)calloc(1, sizeof(*entry));
  if (!entry) {
    ts_mir_fail(c, "JIT compile error: out of memory allocating variable entry for '%s'",
                name ? name : "<unnamed>");
    return ts_mir_new_temp_reg(c);
  }

  entry->name = strdup(name);
  if (!entry->name) {
    free(entry);
    ts_mir_fail(c, "JIT compile error: out of memory duplicating variable name '%s'",
                name ? name : "<unnamed>");
    return ts_mir_new_temp_reg(c);
  }
  entry->reg = MIR_new_func_reg(c->ctx, c->func->u.func, MIR_T_D, name);
  entry->dirty = 0;
  entry->dynamic_value = 0;
  c->vars[c->var_count++] = entry;
  return entry->reg;
}

int ts_mir_bind_existing_reg(ts_mir_compiler_t *c, const char *name, MIR_reg_t reg) {
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

void ts_mir_mark_var_dirty(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dirty = 1;
      return;
    }
  }
}

void ts_mir_mark_var_clean(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dirty = 0;
      return;
    }
  }
}

void ts_mir_mark_all_vars_dirty(ts_mir_compiler_t *c) {
  if (!c) return;

  for (int i = 0; i < c->var_count; i++) {
    if (c->vars[i]->dynamic_value) continue;
    c->vars[i]->dirty = 1;
  }
}

void ts_mir_clear_dirty_flags(ts_mir_compiler_t *c) {
  if (!c) return;

  for (int i = 0; i < c->var_count; i++) {
    c->vars[i]->dirty = 0;
  }
}

void ts_mir_mark_var_dynamic(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dynamic_value = 1;
      return;
    }
  }
}

void ts_mir_mark_var_numeric(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) {
      c->vars[i]->dynamic_value = 0;
      return;
    }
  }
}

int ts_mir_var_is_dynamic(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;

  for (int i = 0; i < c->var_count; i++) {
    if (strcmp(c->vars[i]->name, name) == 0) return c->vars[i]->dynamic_value;
  }
  return 0;
}

int ts_mir_add_class_name(ts_mir_compiler_t *c, const char *name) {
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

int ts_mir_set_var_class(ts_mir_compiler_t *c, const char *var_name,
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

void ts_mir_clear_var_class(ts_mir_compiler_t *c, const char *var_name) {
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

int ts_mir_is_known_class_name(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return 0;
  for (int i = 0; i < c->class_name_count; ++i) {
    if (strcmp(c->class_names[i], name) == 0) return 1;
  }
  return 0;
}

const char *ts_mir_find_var_class(ts_mir_compiler_t *c, const char *var_name) {
  if (!c || !var_name) return NULL;
  for (int i = 0; i < c->class_type_count; ++i) {
    if (strcmp(c->class_types[i].var_name, var_name) == 0) return c->class_types[i].class_name;
  }
  return NULL;
}

const char *ts_mir_new_hidden_receiver_name(ts_mir_compiler_t *c) {
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

MIR_reg_t ts_mir_get_or_add_vec_ptr(ts_mir_compiler_t *c, const char *name) {
  MIR_reg_t ptr_reg = 0;

  for (int i = 0; i < c->vec_ptr_count; i++) {
    if (strcmp(c->vec_ptrs[i].name, name) == 0)
      return c->vec_ptrs[i].ptr_reg;
  }

  if (!ts_mir_ensure_vec_ptr_capacity(c, c->vec_ptr_count + 1))
    return 0;

  ptr_reg = ts_mir_new_temp_ireg(c);
  c->vec_ptrs[c->vec_ptr_count].name = name;
  c->vec_ptrs[c->vec_ptr_count].ptr_reg = ptr_reg;
  c->vec_ptr_count++;
  return ptr_reg;
}

MIR_reg_t ts_mir_get_or_add_map_ptr(ts_mir_compiler_t *c, const char *obj_name,
                                    const char *key_name) {
  MIR_reg_t ptr_reg = 0;

  for (int i = 0; i < c->map_ptr_count; i++) {
    if (c->map_ptrs[i].obj_name == obj_name && strcmp(c->map_ptrs[i].key_name, key_name) == 0)
      return c->map_ptrs[i].ptr_reg;
  }

  if (!ts_mir_ensure_map_ptr_capacity(c, c->map_ptr_count + 1))
    return 0;

  ptr_reg = ts_mir_new_temp_ireg(c);
  c->map_ptrs[c->map_ptr_count].obj_name = obj_name;
  c->map_ptrs[c->map_ptr_count].key_name = key_name;
  c->map_ptrs[c->map_ptr_count].ptr_reg = ptr_reg;
  c->map_ptr_count++;
  return ptr_reg;
}

MIR_reg_t ts_mir_get_or_add_oop_ptr(ts_mir_compiler_t *c, const char *obj_name,
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

  ptr_reg = ts_mir_new_temp_preg(c);
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

ts_compiled_func_t *ts_find_compiled_func(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return NULL;
  for (int i = 0; i < c->compiled_func_count; i++) {
    if (c->compiled_funcs[i].name && strcmp(c->compiled_funcs[i].name, name) == 0)
      return &c->compiled_funcs[i];
  }
  return NULL;
}

const char *ts_find_func_alias(ts_mir_compiler_t *c, const char *name) {
  if (!c || !name) return NULL;
  for (size_t i = 0; i < c->func_alias_count; ++i) {
    if (c->func_aliases[i].param_name &&
        strcmp(c->func_aliases[i].param_name, name) == 0) {
      return c->func_aliases[i].target_name;
    }
  }
  return NULL;
}

int ts_mir_reserve_compiled_func(ts_mir_compiler_t *c) {
  if (!c) return 0;
  return ts_mir_ensure_compiled_func_capacity(c, c->compiled_func_count + 1);
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

void ts_mir_destroy_compiler_storage(ts_mir_compiler_t *c) {
  if (!c)
    return;

  ts_mir_discard_current_vars(c);
  ts_mir_destroy_compiled_funcs(c);
  ts_mir_destroy_pointer_caches(c);
  ts_mir_destroy_class_names(c);
  ts_mir_discard_class_types(c);
}

ts_mir_compile_frame_t ts_mir_capture_frame(const ts_mir_compiler_t *c) {
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

void ts_mir_begin_isolated_compile(ts_mir_compiler_t *c) {
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

void ts_mir_restore_frame(ts_mir_compiler_t *c, const ts_mir_compile_frame_t *frame) {
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

void ts_emit_var_prologue(ts_mir_compiler_t *c) {
  /* Prepend load_var calls at the beginning of the function. */
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

void ts_emit_var_epilogue(ts_mir_compiler_t *c) {
  /* Only numeric MIR writes are dirty. Helper-evaluated values are already stored in env. */
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

void ts_emit_vec_prologue(ts_mir_compiler_t *c) {
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

void ts_emit_map_prologue(ts_mir_compiler_t *c) {
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
