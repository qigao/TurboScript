#include "turbo_script.h"
#include "exprtk.h"
#include "ts_plugin.h"
#include "ts_plugin_loader.h"
#include "turbo_buffer.h"
#include "turbo_fs.h"
#include "turbo_script_internal.h"
#include <mir-gen.h>
#include <mir.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ts_print_value(const exprtk_value_t *val, int repl_mode);
static void set_error_msg(turbo_script_ctx_t *ctx, const char *msg);
static void set_error(turbo_script_ctx_t *ctx, turbo_script_error_code_t code, const char *msg);
static void clear_error(turbo_script_ctx_t *ctx);
exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value, exprtk_env_t *dst_env);
exprtk_env_t *exprtk_env_snapshot(exprtk_env_t *env);

const char *turbo_script_version(void) { return TURBO_SCRIPT_VERSION_STRING; }

static int ts_copy_path(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0 || !src) return -1;

  size_t len = strlen(src);
  if (len >= dst_size) return -1;

  memcpy(dst, src, len + 1);
  return 0;
}

static int ts_push_script_dir(turbo_script_ctx_t *ctx, const char *path, char **prev_dir) {
  char dirname[TURBO_FS_MAX_PATH];
  char *new_dir = NULL;

  if (!ctx || !path || !prev_dir) return -1;
  if (turbo_fs_path_dirname(path, dirname, sizeof(dirname)) != 0) return -1;

  new_dir = strdup(dirname);
  if (!new_dir) return -1;

  *prev_dir = ctx->current_script_dir;
  ctx->current_script_dir = new_dir;
  return 0;
}

static void ts_pop_script_dir(turbo_script_ctx_t *ctx, char *prev_dir) {
  if (!ctx) return;

  free(ctx->current_script_dir);
  ctx->current_script_dir = prev_dir;
}

static int ts_resolve_script_path(turbo_script_ctx_t *ctx, const char *name, char *resolved,
                                  size_t resolved_size) {
  if (!ctx || !name || !resolved || resolved_size == 0) return -1;

  while (name[0] == '.' && (name[1] == '/' || name[1] == '\\')) {
    name += 2;
  }

  if (turbo_fs_path_is_absolute(name) || !ctx->current_script_dir || !ctx->current_script_dir[0]) {
    return ts_copy_path(resolved, resolved_size, name);
  }

  return turbo_fs_path_join(resolved, resolved_size, ctx->current_script_dir, name);
}

static int ts_prepare_expr(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx || !script) return -1;

  if (ctx->expr && ctx->expr_source && strcmp(ctx->expr_source, script) == 0) {
    clear_error(ctx);
    return 0;
  }

  if (ctx->expr) {
    exprtk_free(ctx->expr);
    ctx->expr = NULL;
  }
  free(ctx->expr_source);
  ctx->expr_source = NULL;

  ctx->expr = turbo_script_parse_with_error(ctx, script);
  if (!ctx->expr) return -1;

  ctx->expr_source = strdup(script);
  if (!ctx->expr_source) {
    exprtk_free(ctx->expr);
    ctx->expr = NULL;
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return -1;
  }

  return 0;
}

void turbo_script_free(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  if (ctx->expr) exprtk_free(ctx->expr);
  free(ctx->expr_source);

  // Free JIT cache copied script strings
  for (int i = 0; i < TS_JIT_CACHE_SIZE; ++i) {
    free(ctx->jit_cache[i].script);
  }

  // Free compiled ASTs
  if (ctx->compiled_asts) {
    for (size_t i = 0; i < ctx->compiled_ast_count; ++i) {
      if (ctx->compiled_asts[i]) {
        exprtk_free(ctx->compiled_asts[i]);
      }
    }
    free(ctx->compiled_asts);
  }

  // Free imported modules
  imported_module_t *mod = ctx->imports;
  while (mod) {
    imported_module_t *next = mod->next;
    free(mod->name);
    if (mod->expr) exprtk_free(mod->expr);
    if (mod->has_exports && exprtk_value_is_object_like(&mod->exports)) exprtk_map_free(&mod->exports);
    free(mod);
    mod = next;
  }

  /* Phase 15: finish gen context if it was initialized */
  if (ctx->mir_ctx) {
    if (ctx->mir_gen_initialized) MIR_gen_finish(ctx->mir_ctx);
    MIR_finish(ctx->mir_ctx);
  }
  if (ctx->mir_interp_ctx) {
    MIR_finish(ctx->mir_interp_ctx);
  }
  if (ctx->script_mir_ctx) {
    MIR_finish(ctx->script_mir_ctx);
  }

  exprtk_env_free(&ctx->env);

  // Unload plugins after env teardown, so module/function pointers are no longer referenced.
  for (size_t i = 0; i < ctx->plugin_count; ++i) {
    ts_plugin_unload(ctx->plugins[i]);
    free(ctx->loaded_names[i]);
  }

  free(ctx->current_script_dir);
  mem_destroy(&ctx->scratch_arena);
  free(ctx);
}

static void set_error_msg(turbo_script_ctx_t *ctx, const char *msg) {
  if (!ctx) return;
  if (msg) {
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
    return;
  }
  ctx->error_msg[0] = '\0';
}

static void set_error(turbo_script_ctx_t *ctx, turbo_script_error_code_t code, const char *msg) {
  if (!ctx) return;
  ctx->error_code = code;
  set_error_msg(ctx, msg);
}

static void clear_error(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  set_error_msg(ctx, NULL);
}

/* Zero-value shorthand */
#define TS_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})
#define TS_ERROR(ctx, code, msg)                                                                   \
  do {                                                                                             \
    set_error(ctx, code, msg);                                                                     \
    (ctx)->env.aborted = 1;                                                                        \
  } while (0)

exprtk_node_t *turbo_script_parse_with_error(turbo_script_ctx_t *ctx, const char *script) {
  mem_pool_t *arena = NULL;
  exprtk_node_t *root = NULL;
  int err = 0;

  if (!ctx || !script) return NULL;

  clear_error(ctx);

  arena = (mem_pool_t *)malloc(sizeof(*arena));
  if (!arena) {
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return NULL;
  }
  if (mem_init(arena, 4096) != 0) {
    free(arena);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return NULL;
  }

  root = exprtk_parse_ext(script, 0, arena, &err, ctx->error_msg, sizeof(ctx->error_msg));
  if (!err && root) return root;

  if (ctx->error_msg[0] == '\0') set_error(ctx, TURBO_SCRIPT_ERROR_PARSE, "Parse error");
  else ctx->error_code = TURBO_SCRIPT_ERROR_PARSE;
  mem_destroy(arena);
  free(arena);
  return NULL;
}

/* ── Plugin helpers ───────────────────────────────────────────────── */

/* Check if a plugin with the given import name is already loaded */
static int ts_plugin_already_loaded(turbo_script_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->plugin_count; ++i) {
    if (ctx->loaded_names[i] && strcmp(ctx->loaded_names[i], name) == 0) return 1;
  }
  return 0;
}

/* Build plugin filename from logical name and platform suffix. */
static const char *ts_plugin_file_name(mem_pool_t *a, const char *name, const char *suffix,
                                       int use_target_suffix) {
  size_t name_len, suffix_len, target_suffix_len;
  char *buf;
  const char target_suffix[] = "_plugin";

  if (!a || !name || !suffix) return NULL;

  name_len = strlen(name);
  suffix_len = strlen(suffix);
  target_suffix_len = use_target_suffix ? strlen(target_suffix) : 0;
  buf = mem_alloc(a, name_len + target_suffix_len + suffix_len + 1);
  if (buf) sprintf(buf, "%s%s%s", name, use_target_suffix ? target_suffix : "", suffix);
  return buf;
}

/* Load a plugin by logical name ("io", "fin", ...) */
static int ts_load_plugin(turbo_script_ctx_t *ctx, const char *name) {
  char *loaded_name = NULL;
  static const char *const suffixes[] = {
#ifdef _WIN32
      ".dll",
#elif defined(__APPLE__)
      ".dylib",
#else
      ".so",
#endif
      NULL};
  ts_plugin_handle_t *h = NULL;

  if (name && strcmp(name, "regex") == 0) return -1;

  if (ts_plugin_already_loaded(ctx, name)) return 0;
  if (ctx->plugin_count >= TS_MAX_PLUGINS) return -1;

  for (size_t i = 0; suffixes[i] != NULL; ++i) {
    const char *dll = ts_plugin_file_name(&ctx->scratch_arena, name, suffixes[i], 0);
    if (!dll) continue;
    h = ts_plugin_load(dll);
    if (h) break;

    dll = ts_plugin_file_name(&ctx->scratch_arena, name, suffixes[i], 1);
    if (!dll) continue;
    h = ts_plugin_load(dll);
    if (h) break;
  }
  if (!h) return -1;

  if (ts_plugin_init(h, &ctx->env, &ctx->scratch_arena) != 0) {
    ts_plugin_unload(h);
    return -1;
  }

  loaded_name = strdup(name);
  if (!loaded_name) {
    ts_plugin_unload(h);
    return -1;
  }

  size_t idx = ctx->plugin_count;
  ctx->plugins[idx] = h;
  ctx->loaded_names[idx] = loaded_name;
  ctx->plugin_count++;
  return 0;
}

static int ts_is_script_import(const char *name) {
  size_t len;
  if (!name || !*name) return 0;
  len = strlen(name);
  return len >= 4 && strcmp(name + len - 4, ".tbs") == 0;
}

static int ts_script_already_imported(const turbo_script_ctx_t *ctx, const char *name) {
  const imported_module_t *mod = ctx ? ctx->imports : NULL;
  while (mod) {
    if (mod->name && strcmp(mod->name, name) == 0) return 1;
    mod = mod->next;
  }
  return 0;
}

static const imported_module_t *ts_find_imported_script(const turbo_script_ctx_t *ctx, const char *name,
                                                        int isolated) {
  const imported_module_t *mod = ctx ? ctx->imports : NULL;
  while (mod) {
    if (mod->isolated == isolated && mod->name && strcmp(mod->name, name) == 0) return mod;
    mod = mod->next;
  }
  return NULL;
}

static int ts_mark_script_imported(turbo_script_ctx_t *ctx, const char *name, exprtk_value_t exports,
                                   int has_exports, int isolated) {
  imported_module_t *mod = (imported_module_t *)calloc(1, sizeof(*mod));
  if (!mod) return -1;
  mod->name = strdup(name);
  if (!mod->name) {
    free(mod);
    return -1;
  }
  mod->exports = has_exports ? exprtk_value_clone_to_env(exports, &ctx->env) : exports;
  mod->has_exports = has_exports;
  mod->isolated = isolated;
  mod->next = ctx->imports;
  ctx->imports = mod;
  return 0;
}

static int ts_lookup_named_script_function(exprtk_env_t *env, const char *name, exprtk_value_t *value_out) {
  if (!env || !name || !value_out) return -1;

  while (env) {
    exprtk_func_t *fn = env->funcs;
    while (fn) {
      if (fn->is_script && fn->name && strcmp(fn->name, name) == 0) {
        exprtk_value_t val;
        val.type = EXPRTK_VAL_FUNCTION;
        val.data.function.arg_params = fn->data.script.arg_params;
        val.data.function.arg_count = fn->data.script.arg_count;
        val.data.function.body = fn->data.script.body;
        val.data.function.closure_env = env;
        *value_out = val;
        return 0;
      }
      fn = fn->next;
    }
    env = env->parent;
  }

  return -1;
}

static exprtk_value_t ts_rebind_function_closures(exprtk_value_t value, exprtk_env_t *from,
                                                  exprtk_env_t *to) {
  if (!from || !to) return value;

  switch (value.type) {
  case EXPRTK_VAL_FUNCTION:
    if (value.data.function.closure_env == from) value.data.function.closure_env = to;
    break;
  case EXPRTK_VAL_LIST:
    for (size_t i = 0; i < value.data.list.count; ++i) {
      value.data.list.items[i] = ts_rebind_function_closures(value.data.list.items[i], from, to);
    }
    break;
  case EXPRTK_VAL_MAP:
  case EXPRTK_VAL_OBJECT: {
    exprtk_map_iter_t it = exprtk_map_iter_begin(&value);
    const char *key = NULL;
    exprtk_value_t child;
    while (exprtk_map_iter_next(&it, &key, &child)) {
      exprtk_map_set(&value, key, ts_rebind_function_closures(child, from, to));
    }
    break;
  }
  default:
    break;
  }

  return value;
}

static exprtk_value_t ts_export(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  exprtk_value_t value;
  exprtk_env_t *import_env = ctx ? ctx->current_import_env : NULL;

  if (!ctx || !ctx->current_import_name) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_RUNTIME, "export: only valid while importing a script module");
    return TS_ZERO;
  }

  if (argc != 1 && argc != 2) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "export: expected export(name) or export(name, value)");
    return TS_ZERO;
  }
  if (args[0].type != EXPRTK_VAL_STRING) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "export: name must be a string");
    return TS_ZERO;
  }

  if (argc == 2) {
    value = exprtk_value_clone_to_env(args[1], import_env ? import_env : &ctx->env);
  } else if (import_env && exprtk_env_has(import_env, args[0].data.string.data)) {
    value = exprtk_value_clone_to_env(exprtk_env_get(import_env, args[0].data.string.data),
                                      import_env ? import_env : &ctx->env);
  } else if (ts_lookup_named_script_function(import_env ? import_env : &ctx->env, args[0].data.string.data,
                                             &value) != 0) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "export: unknown symbol");
    return TS_ZERO;
  }

  if (ctx->current_import_exports.type != EXPRTK_VAL_MAP)
    ctx->current_import_exports = exprtk_val_map();

  exprtk_map_set(&ctx->current_import_exports, args[0].data.string.data, value);
  ctx->current_import_has_exports = 1;
  return value;
}

static int ts_run_script_file_in_env(turbo_script_ctx_t *ctx, const char *filename,
                                     exprtk_env_t *exec_env, exprtk_value_t *exports_out,
                                     int *has_exports_out) {
  turbo_fs_buf_t buf;
  exprtk_node_t *ast = NULL;
  char *prev_dir = NULL;
  const char *prev_import_name = NULL;
  exprtk_value_t prev_import_exports = exprtk_val_num(0);
  int prev_import_has_exports = 0;
  exprtk_env_t *prev_import_env = NULL;
  int import_state_pushed = 0;
  int rc = -1;

  if (turbo_fs_read_file(filename, &buf) != 0) {
    set_error(ctx, TURBO_SCRIPT_ERROR_IO, "import: failed to read script");
    return -1;
  }

  char *script = (char *)malloc(buf.len + 1);
  if (!script) {
    turbo_fs_buf_free(&buf);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "import: out of memory");
    return -1;
  }

  memcpy(script, buf.base, buf.len);
  script[buf.len] = '\0';
  turbo_fs_buf_free(&buf);

  prev_import_name = ctx->current_import_name;
  prev_import_exports = ctx->current_import_exports;
  prev_import_has_exports = ctx->current_import_has_exports;
  prev_import_env = ctx->current_import_env;

  if (ts_push_script_dir(ctx, filename, &prev_dir) != 0) {
    set_error(ctx, TURBO_SCRIPT_ERROR_IO, "import: path too long");
    goto done;
  }

  ctx->current_import_name = filename;
  ctx->current_import_exports = exprtk_val_map();
  ctx->current_import_has_exports = 0;
  ctx->current_import_env = exec_env;
  import_state_pushed = 1;

  ast = turbo_script_parse_with_error(ctx, script);
  if (!ast) {
    char parse_error[sizeof(ctx->error_msg)];
    strncpy(parse_error, ctx->error_msg, sizeof(parse_error) - 1);
    parse_error[sizeof(parse_error) - 1] = '\0';
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "import '%s': %s", filename, parse_error);
    goto done;
  }

  if (exprtk_validate(ast, exec_env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    ctx->error_code = TURBO_SCRIPT_ERROR_VALIDATE;
    goto done;
  }

  exprtk_free(ast);
  ast = NULL;

  if (exec_env == &ctx->env) {
    rc = turbo_script_run_mir_interp(ctx, script);
  } else {
    exprtk_env_t *saved_import_env = ctx->current_import_env;
    exprtk_env_t *saved_parent = exec_env->parent;

    // Swap exec_env and ctx->env to transfer ownership of internal tables
    exprtk_env_t tmp = ctx->env;
    ctx->env = *exec_env;
    *exec_env = tmp;

    // Set parent of current env to the one now stored in exec_env (the original ctx->env)
    ctx->env.parent = exec_env;
    ctx->current_import_env = &ctx->env;

    rc = turbo_script_run_mir_interp(ctx, script);

    // Rebind internal functions in the module environment (currently in ctx->env)
    // so their closure scopes point to the stable exec_env instead of the temporary &ctx->env.
    exprtk_func_t *f = ctx->env.funcs;
    while (f) {
      if (f->is_script && f->closure_env == &ctx->env) {
        f->closure_env = exec_env;
      }
      f = f->next;
    }

    // Rebind exported function closures first while ctx->env holds the module env
    ctx->current_import_exports =
        ts_rebind_function_closures(ctx->current_import_exports, &ctx->env, exec_env);

    // Restore parent pointer of module env
    ctx->env.parent = saved_parent;

    // Swap back to restore original ctx->env (parent env) and update exec_env with the new module env
    tmp = ctx->env;
    ctx->env = *exec_env;
    *exec_env = tmp;

    ctx->current_import_env = saved_import_env;
  }

done:
  if (import_state_pushed) {
    if (exports_out) *exports_out = ctx->current_import_exports;
    else if (exprtk_value_is_object_like(&ctx->current_import_exports)) exprtk_map_free(&ctx->current_import_exports);
    if (has_exports_out) *has_exports_out = ctx->current_import_has_exports;
    ctx->current_import_name = prev_import_name;
    ctx->current_import_exports = prev_import_exports;
    ctx->current_import_has_exports = prev_import_has_exports;
    ctx->current_import_env = prev_import_env;
  }
  if (ctx->current_script_dir != prev_dir) ts_pop_script_dir(ctx, prev_dir);
  if (ast) exprtk_free(ast);
  free(script);
  return rc;
}

static exprtk_value_t ts_import_script_common(turbo_script_ctx_t *ctx, const char *name, int isolated) {
  exprtk_value_t exports = exprtk_val_num(0);
  int has_exports = 0;
  char resolved[TURBO_FS_MAX_PATH];
  const imported_module_t *mod = NULL;

  if (ts_resolve_script_path(ctx, name, resolved, sizeof(resolved)) != 0) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_IO, "import: path too long");
    return TS_ZERO;
  }

  mod = ts_find_imported_script(ctx, resolved, isolated);
  if (!mod) {
    exprtk_env_t *exec_env = &ctx->env;
    exprtk_env_t *isolated_env = NULL;

    if (isolated) {
      isolated_env = exprtk_env_snapshot(&ctx->env);
      if (!isolated_env) {
        TS_ERROR(ctx, TURBO_SCRIPT_ERROR_OOM, "import_module: out of memory");
        return TS_ZERO;
      }
      isolated_env->parent = &ctx->env;
      isolated_env->funcs = NULL;
      exec_env = isolated_env;
    }

    if (ts_run_script_file_in_env(ctx, resolved, exec_env, &exports, &has_exports) != 0) {
      ctx->env.aborted = 1;
      return TS_ZERO;
    }
    if (ts_mark_script_imported(ctx, resolved, exports, has_exports, isolated) != 0) {
      if (exprtk_value_is_object_like(&exports)) exprtk_map_free(&exports);
      TS_ERROR(ctx, TURBO_SCRIPT_ERROR_OOM, "import: out of memory");
      return TS_ZERO;
    }
    if (exprtk_value_is_object_like(&exports)) exprtk_map_free(&exports);
    mod = ts_find_imported_script(ctx, resolved, isolated);
  }

  if (mod && mod->has_exports)
    return exprtk_value_clone_to_env(mod->exports, &ctx->env);
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
}

static exprtk_value_t ts_import(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "import: expected 1 string arg");
    return TS_ZERO;
  }

  char *name = tstr_v_to_arena(args[0].data.string, &ctx->scratch_arena);
  if (!name) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_OOM, "import: out of memory");
    return TS_ZERO;
  }

  if (ts_is_script_import(name)) {
    return ts_import_script_common(ctx, name, 0);
  }

  if (strcmp(name, "math") == 0 || strcmp(name, "string") == 0 ||
      strcmp(name, "stats") == 0 || strcmp(name, "io") == 0 ||
      strcmp(name, "core") == 0 || strcmp(name, "regex") == 0) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
  }

  if (ts_load_plugin(ctx, name) == 0) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
  }

  TS_ERROR(ctx, TURBO_SCRIPT_ERROR_PLUGIN, "import: failed to load plugin");
  return TS_ZERO;
}

static exprtk_value_t ts_import_module(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  char *name = NULL;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "import_module: expected 1 string arg");
    return TS_ZERO;
  }

  name = tstr_v_to_arena(args[0].data.string, &ctx->scratch_arena);
  if (!name) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_OOM, "import_module: out of memory");
    return TS_ZERO;
  }
  if (!ts_is_script_import(name)) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "import_module: script path required");
    return TS_ZERO;
  }

  return ts_import_script_common(ctx, name, 1);
}

static exprtk_value_t ts_print(size_t argc, exprtk_value_t *args, void *user_data) {
  (void)user_data;
  for (size_t i = 0; i < argc; ++i) {
    if (i > 0) printf(" ");
    ts_print_value(&args[i], 0);
  }
  printf("\n");
  fflush(stdout);
  return TS_ZERO;
}

static void ts_print_vector(const exprtk_value_t *val) {
  size_t n = val->data.vector.size;
  printf("[");
  for (size_t j = 0; j < n; ++j) {
    if (j > 0) printf(", ");
    printf("%g", val->data.vector.data[j]);
  }
  printf("]");
}

static void ts_print_value(const exprtk_value_t *val, int repl_mode) {
  switch (val->type) {
  case EXPRTK_VAL_NUMBER:
    printf("%g", val->data.number);
    break;
  case EXPRTK_VAL_INTEGER:
    printf("%lld", (long long)val->data.integer);
    break;
  case EXPRTK_VAL_BOOL:
    printf("%s", val->data.boolean ? "true" : "false");
    break;
  case EXPRTK_VAL_STRING:
    printf("%.*s", (int)val->data.string.len, val->data.string.data);
    break;
  case EXPRTK_VAL_BYTES:
    printf("bytes(%zu)", val->data.bytes.len);
    break;
  case EXPRTK_VAL_UUID: {
    char text[UUID4_STR_BUFFER_SIZE];
    if (uuid_to_s(val->data.uuid, text, sizeof(text))) printf("%s", text);
    else printf("[uuid]");
    break;
  }
  case EXPRTK_VAL_DATETIME: {
    char text[64];
    time_t ts = turbo_datetime_to_time(&val->data.datetime);
    if (ts != (time_t)-1 && turbo_datetime_format_rfc822(ts, text, sizeof(text)) >= 0)
      printf("%s", text);
    else
      printf("[datetime]");
    break;
  }
  case EXPRTK_VAL_DATE:
    printf("%04d-%02d-%02d", val->data.date.year, val->data.date.month, val->data.date.day);
    break;
  case EXPRTK_VAL_TIME:
    if (val->data.time.millisecond > 0)
      printf("%02d:%02d:%02d.%03d", val->data.time.hour, val->data.time.minute,
             val->data.time.second, val->data.time.millisecond);
    else
      printf("%02d:%02d:%02d", val->data.time.hour, val->data.time.minute,
             val->data.time.second);
    break;
  case EXPRTK_VAL_DURATION: {
    int64_t rem = val->data.duration_ms < 0 ? -val->data.duration_ms : val->data.duration_ms;
    int64_t h = rem / 3600000;
    int64_t m;
    int64_t s;
    rem %= 3600000;
    m = rem / 60000;
    rem %= 60000;
    s = rem / 1000;
    rem %= 1000;
    printf("%s%lld:%02lld:%02lld.%03lld", val->data.duration_ms < 0 ? "-" : "",
           (long long)h, (long long)m, (long long)s, (long long)rem);
    break;
  }
  case EXPRTK_VAL_DECIMAL: {
    exprtk_decimal_t dec = val->data.decimal;
    char digits[32];
    char *p = digits + sizeof(digits);
    uint64_t mag;
    size_t digit_count;
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
    if (negative) printf("-");
    if (dec.scale == 0) {
      printf("%s", p);
    } else if ((size_t)dec.scale >= digit_count) {
      size_t zeros = (size_t)dec.scale - digit_count;
      printf("0.");
      while (zeros-- > 0) printf("0");
      printf("%s", p);
    } else {
      size_t whole = digit_count - (size_t)dec.scale;
      printf("%.*s.%s", (int)whole, p, p + whole);
    }
    break;
  }
  case EXPRTK_VAL_VECTOR:
    ts_print_vector(val);
    break;
  case EXPRTK_VAL_MAP:
    if (repl_mode) printf("{map}");
    else printf("{map:%zu}", exprtk_map_count(val));
    break;
  case EXPRTK_VAL_OBJECT:
    if (repl_mode) printf("{object}");
    else printf("{object:%zu}", exprtk_map_count(val));
    break;
  case EXPRTK_VAL_LIST:
    if (repl_mode) printf("[list]");
    else printf("[list:%zu]", val->data.list.count);
    break;
  case EXPRTK_VAL_NULL:
    printf("null");
    break;
  case EXPRTK_VAL_FUNCTION:
    printf("[function]");
    break;
  default:
    if (repl_mode) printf("[unknown]");
    else printf("[unknown type %d]", val->type);
    break;
  }
}

turbo_script_ctx_t *turbo_script_init(turbo_script_init_flags_t flags) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)calloc(1, sizeof(turbo_script_ctx_t));
  if (!ctx) return NULL;

  exprtk_env_init(&ctx->env);
  mem_init(&ctx->scratch_arena, 4096);

  exprtk_env_register_func(&ctx->env, "import", ts_import, ctx);
  exprtk_env_register_func(&ctx->env, "import_module", ts_import_module, ctx);
  exprtk_env_register_func(&ctx->env, "export", ts_export, ctx);

  if (flags == TURBO_SCRIPT_INIT_DEFAULT) {
    exprtk_env_register_func(&ctx->env, "print", ts_print, ctx);
    (void)ts_load_plugin(ctx, "parser");
  }

  turbo_script_register_modules();
  turbo_script_register_mir(ctx);

  // 检查环境变量启用 JIT 统计
  const char *stats_env = getenv("TS_JIT_STATS");
  if (stats_env && (strcmp(stats_env, "1") == 0 || strcmp(stats_env, "true") == 0)) {
    turbo_script_enable_jit_stats(ctx, 1);
  }

  return ctx;
}

int turbo_script_load_plugin(turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx) return -1;
  if (!name) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "plugin name is NULL");
    return -1;
  }
  clear_error(ctx);
  if (ts_load_plugin(ctx, name) != 0) {
    set_error(ctx, TURBO_SCRIPT_ERROR_PLUGIN, "failed to load plugin");
    return -1;
  }
  return 0;
}

int turbo_script_run(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  if (!script) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "script is NULL");
    return -1;
  }
  clear_error(ctx);
  return turbo_script_run_mir_interp(ctx, script);
}

int turbo_script_repl_run(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  if (!script) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "script is NULL");
    return -1;
  }

  /* Reset scratch arena from previous run */
  mem_reset(&ctx->scratch_arena);
  clear_error(ctx);
  ctx->env.aborted = 0;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;

  if (ts_prepare_expr(ctx, script) != 0) {
    return -1;
  }

  double mir_result = 0.0;
  int ret = turbo_script_compile_mir_interp(ctx, script);
  if (ret == 0) ret = turbo_script_exec_mir_interp_result(ctx, &mir_result);
  if (!ctx->env.aborted && ctx->expr) {
    int is_block = ctx->expr->type == EXPRTK_NODE_BLOCK;
    int empty_block = is_block && ctx->expr->data.block.count == 0;
    if (!empty_block) {
      exprtk_value_t res = exprtk_val_num(mir_result);
      ts_print_value(&res, 1);
      printf("\n");
    }
  }

  return ret;
}

int turbo_script_run_and_print(turbo_script_ctx_t *ctx, const char *script) {
  return turbo_script_repl_run(ctx, script);
}

turbo_script_compiled_t *turbo_script_compile(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return NULL;
  if (!script) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "script is NULL");
    return NULL;
  }

  clear_error(ctx);

  exprtk_node_t *ast = turbo_script_parse_with_error(ctx, script);
  if (!ast) {
    return NULL;
  }

  if (exprtk_validate(ast, &ctx->env, ctx->error_msg, sizeof(ctx->error_msg)) != 0) {
    ctx->error_code = TURBO_SCRIPT_ERROR_VALIDATE;
    exprtk_free(ast);
    return NULL;
  }

  turbo_script_compiled_t *compiled =
      (turbo_script_compiled_t *)malloc(sizeof(turbo_script_compiled_t));
  if (!compiled) {
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    exprtk_free(ast);
    return NULL;
  }
  compiled->source = strdup(script);
  if (!compiled->source) {
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    exprtk_free(ast);
    free(compiled);
    return NULL;
  }
  exprtk_free(ast);
  return compiled;
}

int turbo_script_exec(turbo_script_ctx_t *ctx, turbo_script_compiled_t *compiled) {
  if (!ctx) return -1;
  if (!compiled || !compiled->source) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "compiled script is NULL");
    return -1;
  }

  return turbo_script_run_mir_interp(ctx, compiled->source);
}

bool turbo_script_value_as_bool(exprtk_value_t val) {
  switch (val.type) {
  case EXPRTK_VAL_BOOL:
    return val.data.boolean != 0;
  case EXPRTK_VAL_INTEGER:
    return val.data.integer != 0;
  case EXPRTK_VAL_NUMBER:
    return fabs(val.data.number) > 1e-9;
  case EXPRTK_VAL_STRING:
    return val.data.string.len > 0;
  case EXPRTK_VAL_BYTES:
    return val.data.bytes.len > 0;
  case EXPRTK_VAL_UUID:
  case EXPRTK_VAL_DATETIME:
  case EXPRTK_VAL_DATE:
  case EXPRTK_VAL_TIME:
  case EXPRTK_VAL_DURATION:
  case EXPRTK_VAL_DECIMAL:
    return true;
  case EXPRTK_VAL_VECTOR:
    return val.data.vector.size > 0;
  case EXPRTK_VAL_MAP:
  case EXPRTK_VAL_OBJECT:
    return exprtk_map_count(&val) > 0;
  case EXPRTK_VAL_LIST:
    return val.data.list.count > 0;
  case EXPRTK_VAL_NULL:
    return false;
  case EXPRTK_VAL_FUNCTION:
    return true;
  default:
    return false;
  }
}

void turbo_script_compiled_free(turbo_script_compiled_t *compiled) {
  if (!compiled) return;
  free(compiled->source);
  free(compiled);
}

int turbo_script_run_file(turbo_script_ctx_t *ctx, const char *filename) {
  char *prev_dir = NULL;
  if (!ctx) return -1;
  if (!filename) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "filename is NULL");
    return -1;
  }

  clear_error(ctx);
  turbo_fs_buf_t buf;
  if (turbo_fs_read_file(filename, &buf) != 0) {
    set_error(ctx, TURBO_SCRIPT_ERROR_IO, "run_file: failed to read file");
    return -1;
  }

  /* Ensure null-termination */
  char *script = (char *)malloc(buf.len + 1);
  if (!script) {
    turbo_fs_buf_free(&buf);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "run_file: out of memory");
    return -1;
  }

  memcpy(script, buf.base, buf.len);
  script[buf.len] = '\0';
  turbo_fs_buf_free(&buf);

  if (ts_push_script_dir(ctx, filename, &prev_dir) != 0) {
    free(script);
    set_error(ctx, TURBO_SCRIPT_ERROR_IO, "run_file: path too long");
    return -1;
  }

  int ret = turbo_script_run(ctx, script);
  ts_pop_script_dir(ctx, prev_dir);
  free(script);
  return ret;
}

void ts_bind_num(turbo_script_ctx_t *ctx, const char *name, double value) {
  if (!ctx) return;
  exprtk_value_t v = {EXPRTK_VAL_NUMBER, .data.number = value};
  exprtk_env_set(&ctx->env, name, v);
}

void ts_bind_str(turbo_script_ctx_t *ctx, const char *name, const char *value) {
  if (!ctx || !name || !value) return;
  char *buf = tstr_v_to_arena(tstr_v_from_cstr(value), &ctx->env.arena);
  if (buf) {
    size_t len = strlen(buf);
    exprtk_value_t v = {EXPRTK_VAL_STRING, .data.string = tstr_v_from_buf(buf, len)};
    exprtk_env_set(&ctx->env, name, v);
  }
}

double ts_get_num(turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx) return 0.0;
  exprtk_value_t v = exprtk_env_get(&ctx->env, name);
  if (v.type == EXPRTK_VAL_NUMBER) return v.data.number;
  if (v.type == EXPRTK_VAL_INTEGER) return (double)v.data.integer;
  if (v.type == EXPRTK_VAL_BOOL) return v.data.boolean ? 1.0 : 0.0;
  return 0.0;
}

const char *turbo_script_get_error(turbo_script_ctx_t *ctx) { return ctx ? ctx->error_msg : ""; }

turbo_script_error_code_t turbo_script_get_error_code(turbo_script_ctx_t *ctx) {
  return ctx ? ctx->error_code : TURBO_SCRIPT_ERROR_ARGUMENT;
}

int ts_bind_vec(turbo_script_ctx_t *ctx, const char *name, const double *data, size_t len) {
  if (!ctx || !name || !data || len == 0) return -1;
  double *buf = (double *)mem_alloc(&ctx->env.arena, len * sizeof(double));
  if (!buf) return -1;
  memcpy(buf, data, len * sizeof(double));
  exprtk_value_t v = {EXPRTK_VAL_VECTOR, .data.vector = {buf, len}};
  exprtk_env_set(&ctx->env, name, v);
  return 0;
}

int ts_get_vec(turbo_script_ctx_t *ctx, const char *name, const double **data, size_t *len) {
  if (!ctx || !name || !data || !len) return -1;
  exprtk_value_t v = exprtk_env_get(&ctx->env, name);
  if (v.type != EXPRTK_VAL_VECTOR) return -1;
  *data = v.data.vector.data;
  *len = v.data.vector.size;
  return 0;
}

const char *ts_get_str(turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx || !name) return NULL;
  exprtk_value_t v = exprtk_env_get(&ctx->env, name);
  if (v.type != EXPRTK_VAL_STRING) return NULL;
  return v.data.string.data;
}

void ts_bind_func(turbo_script_ctx_t *ctx, const char *name, turbo_script_func_t fn,
                  void *user_data) {
  if (!ctx || !name || !fn) return;
  exprtk_env_register_func(&ctx->env, name, fn, user_data);
}

/* ========================================================================
 * JIT Statistics Implementation
 * ======================================================================== */

void turbo_script_enable_jit_stats(turbo_script_ctx_t *ctx, int enable) {
  if (!ctx) return;
  
  ctx->jit_stats_enabled = enable ? 1 : 0;
  
  // 如果启用，清零统计数据
  if (enable) {
    memset(&ctx->jit_stats, 0, sizeof(ctx->jit_stats));
  }
}

const turbo_script_jit_stats_t *turbo_script_get_jit_stats(turbo_script_ctx_t *ctx) {
  if (!ctx || !ctx->jit_stats_enabled) return NULL;
  
  // 返回内部统计结构（类型兼容）
  return (const turbo_script_jit_stats_t *)&ctx->jit_stats;
}

void turbo_script_reset_jit_stats(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  
  memset(&ctx->jit_stats, 0, sizeof(ctx->jit_stats));
}

void turbo_script_print_jit_stats(turbo_script_ctx_t *ctx, FILE *fp) {
  if (!ctx || !fp) return;
  
  if (!ctx->jit_stats_enabled) {
    fprintf(fp, "\n=== JIT Statistics (Disabled) ===\n");
    fprintf(fp, "Enable with turbo_script_enable_jit_stats() or TS_JIT_STATS=1\n\n");
    return;
  }
  
  const ts_jit_stats_t *stats = &ctx->jit_stats;
  uint64_t total_cache_ops = stats->cache_hit_count + stats->cache_miss_count;
  
  fprintf(fp, "\n=== TurboScript JIT Statistics ===\n");
  fprintf(fp, "Compile count:       %llu\n", (unsigned long long)stats->compile_count);
  fprintf(fp, "Exec count:          %llu\n", (unsigned long long)stats->exec_count);
  
  if (total_cache_ops > 0) {
    double hit_rate = 100.0 * stats->cache_hit_count / total_cache_ops;
    fprintf(fp, "Cache hits:          %llu (%.1f%%)\n", 
            (unsigned long long)stats->cache_hit_count, hit_rate);
    fprintf(fp, "Cache misses:        %llu (%.1f%%)\n",
            (unsigned long long)stats->cache_miss_count, 100.0 - hit_rate);
  } else {
    fprintf(fp, "Cache hits:          0\n");
    fprintf(fp, "Cache misses:        0\n");
  }
  
  if (stats->compile_count > 0) {
    double avg_compile = (double)stats->total_compile_time_us / stats->compile_count;
    fprintf(fp, "Avg compile time:    %.0f μs\n", avg_compile);
  }
  
  if (stats->exec_count > 0) {
    double avg_exec = (double)stats->total_exec_time_us / stats->exec_count;
    fprintf(fp, "Avg exec time:       %.3f μs\n", avg_exec);
  }
  
  fprintf(fp, "Var sync count:      %llu\n", (unsigned long long)stats->var_sync_count);
  fprintf(fp, "==================================\n\n");
}
