/* Public MIR compile and execution API. */

#include "turbo_script_mir_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

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
 * Public API: Compile, Exec, Run
 * ========================================================================= */

static int ts_track_compiled_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast) {
  if (!ctx || !ast) return 0;
  for (size_t i = 0; i < ctx->compiled_ast_count; ++i) {
    if (ctx->compiled_asts[i] == ast) return 1;
  }

  if (ctx->compiled_ast_count >= ctx->compiled_ast_capacity) {
    size_t new_cap = ctx->compiled_ast_capacity == 0 ? 16 : ctx->compiled_ast_capacity * 2;
    exprtk_node_t **new_asts =
        (exprtk_node_t **)realloc(ctx->compiled_asts, new_cap * sizeof(exprtk_node_t *));
    if (!new_asts) return 0;
    ctx->compiled_asts = new_asts;
    ctx->compiled_ast_capacity = new_cap;
  }

  ctx->compiled_asts[ctx->compiled_ast_count++] = ast;
  return 1;
}

static int turbo_script_compile_mir_backend(turbo_script_ctx_t *ctx, const char *script,
                                            exprtk_node_t *provided_ast, int use_interp) {
  uint64_t start_time = 0;
  MIR_context_t mir_ctx = NULL;
  exprtk_node_t *ast = provided_ast;
  int owns_ast = provided_ast == NULL;

  if (!ctx) return -1;
  if (ctx->memory_exhausted) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "memory policy: context is exhausted and must be recreated");
    return -1;
  }
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!ast && !script) {
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

  if (!ast) {
    ast = turbo_script_parse_with_error(ctx, script);
    if (!ast) return -1;
  }

  if (exprtk_validate(ast, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    ctx->error_code = TURBO_SCRIPT_ERROR_VALIDATE;
    if (owns_ast) exprtk_free(ast);
    return -1;
  }
  if (!ts_track_compiled_ast(ctx, ast)) {
    if (owns_ast) exprtk_free(ast);
    ctx->error_code = TURBO_SCRIPT_ERROR_OOM;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "JIT compile error: out of memory");
    return -1;
  }
  if (ctx->expr == ast) ctx->expr_in_compiled_asts = 1;

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
  ts_mir_init_externals(&compiler);

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
    ts_mir_destroy_compiler_storage(&compiler);
    if (ctx->error_code == TURBO_SCRIPT_ERROR_NONE)
      ctx->error_code = TURBO_SCRIPT_ERROR_JIT;
    return -1;
  }

  /*  Keep AST alive -- don't free it. Node pointers are baked into JIT code.
   * Variable name strings and monomorphic OOP class strings are also baked in as pointer
   * immediates for helper calls. We intentionally keep them alive for the MIR context
   * lifetime. */
  MIR_load_module(mir_ctx, mod);

  if (use_interp) {
    if (!ctx->mir_interp_externals_loaded) {
      ts_mir_load_externals(mir_ctx);
      ctx->mir_interp_externals_loaded = 1;
    }
    MIR_link(mir_ctx, MIR_set_interp_interface, NULL);
    ctx->mir_interp_last_func = func;
  } else {
    /*  Reuse gen context across compiles — init once, finish in turbo_script_free */
    if (!ctx->mir_gen_initialized) {
      MIR_gen_init(mir_ctx);
      ts_mir_load_externals(mir_ctx);
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

TURBO_SCRIPT_C_API int turbo_script_compile_mir(turbo_script_ctx_t *ctx, const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, NULL, 0);
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir_interp(turbo_script_ctx_t *ctx, const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, NULL, 1);
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast,
                                           const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, ast, 0);
}

TURBO_SCRIPT_C_API int turbo_script_compile_mir_interp_ast(turbo_script_ctx_t *ctx, exprtk_node_t *ast,
                                                  const char *script) {
  return turbo_script_compile_mir_backend(ctx, script, ast, 1);
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

TURBO_SCRIPT_C_API int turbo_script_exec_jit(turbo_script_ctx_t *ctx) {
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

TURBO_SCRIPT_C_API int turbo_script_exec_mir_interp_result(turbo_script_ctx_t *ctx, double *result_out) {
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

TURBO_SCRIPT_C_API int turbo_script_exec_mir_interp(turbo_script_ctx_t *ctx) {
  return turbo_script_exec_mir_interp_result(ctx, NULL);
}

TURBO_SCRIPT_C_API int turbo_script_run_mir_interp(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  ctx->error_msg[0] = '\0';
  if (!script) {
    ctx->error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "MIR interp run error: script is NULL");
    return -1;
  }
  if (ctx->expr && ctx->expr_source && strcmp(ctx->expr_source, script) == 0) {
    if (turbo_script_compile_mir_interp_ast(ctx, ctx->expr, script) != 0) return -1;
    return turbo_script_exec_mir_interp(ctx);
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

TURBO_SCRIPT_C_API int turbo_script_run_jit(turbo_script_ctx_t *ctx, const char *script) {
  uint64_t exec_start_time = 0;
  
  if (!ctx) return -1;
  if (ctx->memory_exhausted) {
    ctx->error_code = TURBO_SCRIPT_ERROR_STATE;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg),
             "memory policy: context is exhausted and must be recreated");
    return -1;
  }
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
    ctx->jit_cache[slot].access_count++;  // 直接映射缓存；access_count 仅供未来 LRU 使用
    
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
    
    return ts_memory_finish_run(ctx, result);
  }

  // 记录缓存未命中
  if (ctx->jit_stats_enabled) {
    ctx->jit_stats.cache_miss_count++;
  }

  /* Cache miss — full compile. Unsupported scripts must fail instead of
   * silently running through the interpreter. */
  if (turbo_script_compile_mir(ctx, script) != 0) return -1;

  /* Store in cache. A failed script copy leaves the slot empty so a later
   * lookup cannot match a partial (script == NULL) entry. */
  free(ctx->jit_cache[slot].script);
  ctx->jit_cache[slot].script = strdup(script);
  if (!ctx->jit_cache[slot].script) {
    ctx->jit_cache[slot].hash = 0;
    ctx->jit_cache[slot].fn_ptr = NULL;
    ctx->jit_cache[slot].access_count = 0;
  } else {
    ctx->jit_cache[slot].hash = hash;
    ctx->jit_cache[slot].fn_ptr = ctx->mir_last_fn;
    ctx->jit_cache[slot].access_count = 1;  // 初始化访问计数
  }

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
  
  return ts_memory_finish_run(ctx, result);
}
