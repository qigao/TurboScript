#include "turbo_script.h"
#include "exprtk.h"
#include "ts_plugin.h"
#include "ts_plugin_loader.h"
#include "salts_buffer.h"
#include "salts_fs.h"
#include "salts_thread.h"
#include "turbo_script_internal.h"
#include "turbo_script_timer.h"
#include "turbo_script_task.h"
#include "host/turbo_script_host_internal.h"
#include <mir-gen.h>
#include <mir.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ts_print_value(const exprtk_value_t *val, int repl_mode);
static void set_error_msg(turbo_script_ctx_t *ctx, const char *msg);
static void set_error(turbo_script_ctx_t *ctx, turbo_script_error_code_t code, const char *msg);
static void clear_error(turbo_script_ctx_t *ctx);
exprtk_env_t *exprtk_env_snapshot(exprtk_env_t *env);
static void ts_context_destroy_final(turbo_script_ctx_t *ctx);
static SALTS_THREAD_LOCAL unsigned char ts_context_thread_owner_token;

static const void *ts_context_current_thread_token(void) {
  return &ts_context_thread_owner_token;
}

int ts_context_is_owner_thread(const turbo_script_ctx_t *ctx) {
  return ctx && ctx->owner_thread_token == ts_context_current_thread_token();
}

const char *turbo_script_version(void) { return TURBO_SCRIPT_VERSION_STRING; }

static int ts_copy_path(char *dst, size_t dst_size, const char *src) {
  if (!dst || dst_size == 0 || !src) return -1;

  size_t len = strlen(src);
  if (len >= dst_size) return -1;

  memcpy(dst, src, len + 1);
  return 0;
}

static int ts_push_script_dir(turbo_script_ctx_t *ctx, const char *path, char **prev_dir) {
  char dirname[SALTS_FS_MAX_PATH];
  char *new_dir = NULL;

  if (!ctx || !path || !prev_dir) return -1;
  if (salts_fs_path_dirname(path, dirname, sizeof(dirname)) != 0) return -1;

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

  if (salts_fs_path_is_absolute(name) || !ctx->current_script_dir || !ctx->current_script_dir[0]) {
    return ts_copy_path(resolved, resolved_size, name);
  }

  return salts_fs_path_join(resolved, resolved_size, ctx->current_script_dir, name);
}

static int ts_prepare_expr(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx || !script) return -1;

  if (ctx->expr && ctx->expr_source && strcmp(ctx->expr_source, script) == 0) {
    clear_error(ctx);
    return 0;
  }

  if (ctx->expr) {
    if (!ctx->expr_in_compiled_asts) {
      exprtk_free(ctx->expr);
    }
    ctx->expr = NULL;
    ctx->expr_in_compiled_asts = 0;
  }
  free(ctx->expr_source);
  ctx->expr_source = NULL;

  ctx->expr = turbo_script_parse_with_error(ctx, script);
  if (!ctx->expr) return -1;

  ctx->expr_source = strdup(script);
  if (!ctx->expr_source) {
    exprtk_free(ctx->expr);
    ctx->expr = NULL;
    ctx->expr_in_compiled_asts = 0;
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return -1;
  }

  return 0;
}

static void ts_context_destroy_final(turbo_script_ctx_t *ctx) {
  if (!ctx) return;

  ts_task_scheduler_destroy(ctx);
  ts_timer_scheduler_destroy(ctx);
  if (ctx->executor) {
    (void)salts_coro_executor_destroy(ctx->executor);
    ctx->executor = NULL;
  }

  // Free JIT cache copied script strings
  for (int i = 0; i < TS_JIT_CACHE_SIZE; ++i) {
    free(ctx->jit_cache[i].script);
  }

  /* MIR code stores pointers into compiled AST nodes. Finish MIR contexts before
   * tearing down AST arenas that those contexts may reference. */
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

  /* expr is freed by compiled_asts[] loop below if expr_in_compiled_asts == 1;
   * otherwise free it directly here. */
  if (ctx->expr && !ctx->expr_in_compiled_asts) exprtk_free(ctx->expr);
  free(ctx->expr_source);

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

  exprtk_env_free(&ctx->env);

  // Unload plugins after env teardown, so module/function pointers are no longer referenced.
  for (size_t i = 0; i < ctx->plugin_count; ++i) {
    ts_plugin_unload(ctx->plugins[i]);
    free(ctx->loaded_names[i]);
  }

  free(ctx->current_script_dir);
  mem_destroy(&ctx->scratch_arena);
  ts_host_registry_destroy(ctx);
  free(ctx);
}

void ts_context_retain(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  (void)atomic_fetch_add_explicit(&ctx->ref_count, 1U, memory_order_relaxed);
}

void ts_context_release(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  if (atomic_fetch_sub_explicit(&ctx->ref_count, 1U, memory_order_acq_rel) == 1U)
    ts_context_destroy_final(ctx);
}

void turbo_script_free(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  if (atomic_exchange_explicit(&ctx->closing, 1, memory_order_acq_rel)) return;
  ts_timer_scheduler_shutdown(ctx);
  ts_task_scheduler_shutdown(ctx);
  if (ctx->executor) {
    (void)salts_coro_executor_shutdown(ctx->executor);
    (void)salts_coro_executor_wait(ctx->executor);
  }
  ts_context_release(ctx);
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

static int ts_memory_policy_valid(const turbo_script_memory_policy_t *policy) {
  return policy && policy->profile >= TURBO_SCRIPT_MEMORY_BATCH &&
         policy->profile <= TURBO_SCRIPT_MEMORY_SANDBOX &&
         policy->max_context_bytes > 0 && policy->max_task_bytes > 0 &&
         policy->max_external_value_bytes > 0 &&
         policy->scratch_trim_threshold_bytes > 0 &&
         policy->max_external_value_bytes <= policy->max_task_bytes &&
         policy->max_task_bytes <= policy->max_context_bytes;
}

int turbo_script_memory_policy_init(turbo_script_memory_profile_t profile,
                                    turbo_script_memory_policy_t *policy) {
  static const turbo_script_memory_policy_t policies[] = {
      {TURBO_SCRIPT_MEMORY_BATCH, 512U * 1024U * 1024U, 64U * 1024U * 1024U,
       16U * 1024U * 1024U, 4U * 1024U * 1024U},
      {TURBO_SCRIPT_MEMORY_INTERACTIVE, 128U * 1024U * 1024U, 16U * 1024U * 1024U,
       4U * 1024U * 1024U, 512U * 1024U},
      {TURBO_SCRIPT_MEMORY_SERVICE, 256U * 1024U * 1024U, 16U * 1024U * 1024U,
       4U * 1024U * 1024U, 512U * 1024U},
      {TURBO_SCRIPT_MEMORY_STREAMING, 128U * 1024U * 1024U, 8U * 1024U * 1024U,
       1U * 1024U * 1024U, 256U * 1024U},
      {TURBO_SCRIPT_MEMORY_SANDBOX, 32U * 1024U * 1024U, 4U * 1024U * 1024U,
       256U * 1024U, 64U * 1024U},
  };
  if (!policy || profile < TURBO_SCRIPT_MEMORY_BATCH ||
      profile > TURBO_SCRIPT_MEMORY_SANDBOX)
    return -1;
  *policy = policies[(size_t)profile];
  return 0;
}

int turbo_script_get_memory_stats(turbo_script_ctx_t *ctx,
                                  turbo_script_memory_stats_t *stats) {
  size_t root_bytes;
  size_t task_bytes;
  size_t scratch_bytes;
  size_t total;
  if (!ctx || !stats) return -1;
  /* Closure snapshots (function expressions, isolated imports, class closures)
   * are attached to the root env closure chain and retained until the context
   * is destroyed. Count their arenas so the context quota covers them too. */
  root_bytes = mem_pool_total_used(&ctx->env.arena);
  for (const exprtk_env_t *closure = ctx->env.next_closure; closure;
       closure = closure->next_closure) {
    if (root_bytes > SIZE_MAX - mem_pool_total_used(&closure->arena)) return -1;
    root_bytes += mem_pool_total_used(&closure->arena);
  }
  task_bytes = ts_task_memory_used(ctx);
  scratch_bytes = mem_pool_total_used(&ctx->scratch_arena);
  if (root_bytes > SIZE_MAX - task_bytes || root_bytes + task_bytes > SIZE_MAX - scratch_bytes)
    return -1;
  total = root_bytes + task_bytes + scratch_bytes;
  if (total > ctx->peak_context_bytes) ctx->peak_context_bytes = total;
  stats->context_bytes = root_bytes;
  stats->task_bytes = task_bytes;
  stats->scratch_bytes = scratch_bytes;
  stats->peak_context_bytes = ctx->peak_context_bytes;
  return 0;
}

int turbo_script_set_memory_policy(turbo_script_ctx_t *ctx,
                                   const turbo_script_memory_policy_t *policy) {
  turbo_script_memory_stats_t stats;
  if (!ctx || !ts_memory_policy_valid(policy)) return -1;
  if (ctx->memory_exhausted || turbo_script_task_active_count(ctx) != 0 ||
      turbo_script_timer_active_count(ctx) != 0)
    return -1;
  if (turbo_script_get_memory_stats(ctx, &stats) != 0 ||
      stats.context_bytes > policy->max_context_bytes ||
      stats.task_bytes > policy->max_context_bytes - stats.context_bytes ||
      ts_task_memory_max_used(ctx) > policy->max_task_bytes ||
      stats.scratch_bytes > policy->max_context_bytes)
    return -1;
  ctx->memory_policy = *policy;
  ctx->env.max_external_value_bytes = policy->max_external_value_bytes;
  return 0;
}

int ts_memory_finish_run(turbo_script_ctx_t *ctx, int result) {
  turbo_script_memory_stats_t stats;
  if (!ctx) return -1;
  /* Reclaim captured closure scopes created by this thread that no live
   * function value references. Short-lived closures created during this run
   * are freed here instead of accumulating on the root closure chain until
   * the context is destroyed. The sweep only touches snapshots owned by the
   * calling thread, so concurrent timer/task callbacks on the event-loop
   * thread are never disturbed. */
  exprtk_env_sweep_closures(&ctx->env);
  if (mem_pool_total_used(&ctx->scratch_arena) >=
      ctx->memory_policy.scratch_trim_threshold_bytes) {
    mem_reset(&ctx->scratch_arena);
    mem_trim(&ctx->scratch_arena);
  }
  if (turbo_script_get_memory_stats(ctx, &stats) != 0 ||
      stats.context_bytes > ctx->memory_policy.max_context_bytes ||
      stats.task_bytes > ctx->memory_policy.max_context_bytes - stats.context_bytes) {
    ctx->memory_exhausted = 1;
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "memory policy: context quota exceeded");
    ctx->env.aborted = 1;
    return -1;
  }
  return result;
}

/* Zero-value shorthand */
#define TS_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})
#define TS_ERROR(ctx, code, msg)                                                                   \
  do {                                                                                             \
    set_error(ctx, code, msg);                                                                     \
    (ctx)->env.aborted = 1;                                                                        \
  } while (0)

static int ts_is_ident_start(unsigned char c) { return isalpha(c) || c == '_'; }

static int ts_is_ident_part(unsigned char c) { return isalnum(c) || c == '_'; }

static const char *ts_skip_ws_and_comments(const char *p) {
  for (;;) {
    while (*p && isspace((unsigned char)*p)) ++p;
    if (p[0] == '/' && p[1] == '/') {
      p += 2;
      while (*p && *p != '\n') ++p;
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      p += 2;
      while (*p && !(p[0] == '*' && p[1] == '/')) ++p;
      if (*p) p += 2;
      continue;
    }
    return p;
  }
}

static int ts_brace_starts_map_arg(const char *brace) {
  const char *p = ts_skip_ws_and_comments(brace + 1);
  if (p[0] == '.' && p[1] == '.' && p[2] == '.') return 1;
  if (!ts_is_ident_start((unsigned char)*p)) return 0;
  ++p;
  while (ts_is_ident_part((unsigned char)*p)) ++p;
  p = ts_skip_ws_and_comments(p);
  return *p == ':';
}

static char ts_prev_significant_char(const char *start, const char *p) {
  while (p > start) {
    --p;
    if (!isspace((unsigned char)*p)) return *p;
  }
  return '\0';
}

static int ts_buf_append(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
  if (n == 0) return 1;
  if (*len + n + 1 > *cap) {
    size_t new_cap = *cap ? *cap : 256;
    char *new_buf;
    while (*len + n + 1 > new_cap) new_cap *= 2;
    new_buf = (char *)realloc(*buf, new_cap);
    if (!new_buf) return 0;
    *buf = new_buf;
    *cap = new_cap;
  }
  memcpy(*buf + *len, data, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static char *ts_normalize_braced_map_args(const char *script, int *changed_out) {
  const char *p = script;
  const char *chunk = script;
  char *out = NULL;
  size_t out_len = 0;
  size_t out_cap = 0;
  int changed = 0;

  if (changed_out) *changed_out = 0;

  while (*p) {
    if (*p == '"' || *p == '`' || *p == '\'') {
      char quote = *p++;
      while (*p) {
        if (*p == '\\' && p[1]) {
          p += 2;
          continue;
        }
        if (*p++ == quote) break;
      }
      continue;
    }
    if (p[0] == '/' && p[1] == '/') {
      p += 2;
      while (*p && *p != '\n') ++p;
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      p += 2;
      while (*p && !(p[0] == '*' && p[1] == '/')) ++p;
      if (*p) p += 2;
      continue;
    }
    if (*p == '{') {
      char prev = ts_prev_significant_char(script, p);
      if ((prev == '(' || prev == ',') && ts_brace_starts_map_arg(p)) {
        if (!ts_buf_append(&out, &out_len, &out_cap, chunk, (size_t)(p - chunk)) ||
            !ts_buf_append(&out, &out_len, &out_cap, "map", 3)) {
          free(out);
          if (changed_out) *changed_out = 1;
          return NULL;
        }
        chunk = p;
        changed = 1;
      }
    }
    ++p;
  }

  if (!changed) return NULL;
  if (changed_out) *changed_out = 1;
  if (!ts_buf_append(&out, &out_len, &out_cap, chunk, (size_t)(p - chunk))) {
    free(out);
    return NULL;
  }
  return out;
}

static exprtk_node_t *ts_parse_with_error_impl(turbo_script_ctx_t *ctx,
                                               const char *script,
                                               int fail_arena_allocation) {
  mem_pool_t *arena = NULL;
  exprtk_node_t *root = NULL;
  char *normalized = NULL;
  const char *parse_script = script;
  int normalized_changed = 0;
  int err = 0;

  if (!ctx || !script) return NULL;

  clear_error(ctx);

  arena = fail_arena_allocation ? NULL : (mem_pool_t *)malloc(sizeof(*arena));
  if (!arena) {
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return NULL;
  }
  if (mem_init(arena, 4096) != 0) {
    free(arena);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return NULL;
  }

  normalized = ts_normalize_braced_map_args(script, &normalized_changed);
  if (normalized_changed && !normalized) {
    mem_destroy(arena);
    free(arena);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "Out of memory");
    return NULL;
  }
  if (normalized) parse_script = normalized;

  root = exprtk_parse_ext(parse_script, 0, arena, &err, ctx->error_msg, sizeof(ctx->error_msg));
  free(normalized);
  if (!err && root) return root;

  if (ctx->error_msg[0] == '\0') set_error(ctx, TURBO_SCRIPT_ERROR_PARSE, "Parse error");
  else ctx->error_code = TURBO_SCRIPT_ERROR_PARSE;
  mem_destroy(arena);
  free(arena);
  return NULL;
}

exprtk_node_t *turbo_script_parse_with_error(turbo_script_ctx_t *ctx,
                                             const char *script) {
  return ts_parse_with_error_impl(ctx, script, 0);
}

exprtk_node_t *turbo_script_parse_with_error_test_oom(turbo_script_ctx_t *ctx,
                                                      const char *script) {
  return ts_parse_with_error_impl(ctx, script, 1);
}

/* ── Plugin helpers ───────────────────────────────────────────────── */

typedef enum {
  TS_PLUGIN_LOAD_OK = 0,
  TS_PLUGIN_LOAD_FAILED = -1,
  TS_PLUGIN_LOAD_DENIED = -2,
} ts_plugin_load_result_t;

enum { TS_MAX_PLUGIN_NAME_LENGTH = 128 };

static int ts_is_valid_plugin_name(const char *name) {
  size_t length = 0;
  unsigned char ch;
  if (!name || !*name) return 0;
  ch = (unsigned char)name[0];
  if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'))
    return 0;
  while (name[length]) {
    ch = (unsigned char)name[length];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
      return 0;
    if (++length > TS_MAX_PLUGIN_NAME_LENGTH) return 0;
  }
  return 1;
}

static int ts_is_builtin_module(const char *name) {
  if (!name) return 0;
  return strcmp(name, "math") == 0 || strcmp(name, "string") == 0 ||
         strcmp(name, "stats") == 0 || strcmp(name, "io") == 0 ||
         strcmp(name, "core") == 0 || strcmp(name, "regex") == 0 ||
         strcmp(name, "timer") == 0;
}

/* Check if a plugin with the given import name is already loaded */
static int ts_plugin_already_loaded(turbo_script_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->plugin_count; ++i) {
    if (ctx->loaded_names[i] && strcmp(ctx->loaded_names[i], name) == 0) return 1;
  }
  return 0;
}

/* Build plugin filename from logical name and platform suffix. */
static const char *ts_plugin_file_name(mem_pool_t *a, const char *name, const char *suffix,
                                       int use_tbs_prefix) {
  size_t name_len, suffix_len, prefix_len;
  char *buf;
  const char tbs_prefix[] = "tbs_";

  if (!a || !name || !suffix) return NULL;

  name_len = strlen(name);
  suffix_len = strlen(suffix);
  prefix_len = use_tbs_prefix ? strlen(tbs_prefix) : 0;
  buf = mem_alloc(a, prefix_len + name_len + suffix_len + 1);
  if (buf) sprintf(buf, "%s%s%s", use_tbs_prefix ? tbs_prefix : "", name, suffix);
  return buf;
}

static const char *ts_plugin_file_stem(const char *name) {
  /* Keep dependency DLL names distinct from plugin DLL names while preserving
   * the public logical names used by import() and descriptor validation. */
  if (name && strcmp(name, "crypto") == 0) return "crypto_plugin";
  if (name && strcmp(name, "rules_forge") == 0) return "rules_forge_plugin";
  return name;
}

/* Load a plugin by logical name ("io", "fin", ...) */
static int ts_load_plugin(turbo_script_ctx_t *ctx, const char *name,
                          ts_plugin_error_t *plugin_error) {
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
  int load_error;
  ts_plugin_handle_t *h = NULL;

  if (plugin_error) memset(plugin_error, 0, sizeof(*plugin_error));
  if (!ctx || !name || !*name) return TS_PLUGIN_LOAD_FAILED;
  if (!ts_is_valid_plugin_name(name)) {
    if (plugin_error) {
      plugin_error->code = TS_PLUGIN_ERROR_INVALID_ARGUMENT;
      plugin_error->stage = TS_PLUGIN_STAGE_ARGUMENT;
      snprintf(plugin_error->message, sizeof(plugin_error->message),
               "invalid plugin name");
    }
    return TS_PLUGIN_LOAD_FAILED;
  }
  if (ts_is_builtin_module(name)) return TS_PLUGIN_LOAD_OK;

  if (ts_plugin_already_loaded(ctx, name)) return TS_PLUGIN_LOAD_OK;
  if (ctx->plugin_count >= TS_MAX_PLUGINS) return TS_PLUGIN_LOAD_FAILED;
  if (ctx->plugin_authorizer &&
      !ctx->plugin_authorizer(name, ctx->plugin_authorizer_data))
    return TS_PLUGIN_LOAD_DENIED;

  for (size_t i = 0; suffixes[i] != NULL; ++i) {
    const char *dll = ts_plugin_file_name(&ctx->scratch_arena,
                                          ts_plugin_file_stem(name), suffixes[i], 0);
    if (!dll) continue;
    load_error = ts_plugin_load_ex(dll, name, &h, plugin_error);
    if (load_error == TS_PLUGIN_ERROR_NONE) break;
    if (load_error != TS_PLUGIN_ERROR_OPEN) return TS_PLUGIN_LOAD_FAILED;

    dll = ts_plugin_file_name(&ctx->scratch_arena, name, suffixes[i], 1);
    if (!dll) continue;
    if (ts_plugin_load_ex(dll, name, &h, plugin_error) == TS_PLUGIN_ERROR_NONE) break;
  }
  if (!h) return TS_PLUGIN_LOAD_FAILED;

  if (ts_plugin_init_ex(h, &ctx->env, &ctx->scratch_arena, plugin_error) !=
      TS_PLUGIN_ERROR_NONE) {
    ts_plugin_unload(h);
    return TS_PLUGIN_LOAD_FAILED;
  }

  loaded_name = strdup(name);
  if (!loaded_name) {
    ts_plugin_unload(h);
    return TS_PLUGIN_LOAD_FAILED;
  }

  size_t idx = ctx->plugin_count;
  ctx->plugins[idx] = h;
  ctx->loaded_names[idx] = loaded_name;
  ctx->plugin_count++;
  return TS_PLUGIN_LOAD_OK;
}

static const char *ts_plugin_stage_name(ts_plugin_error_stage_t stage) {
  switch (stage) {
    case TS_PLUGIN_STAGE_ARGUMENT: return "argument";
    case TS_PLUGIN_STAGE_OPEN: return "open";
    case TS_PLUGIN_STAGE_SYMBOL: return "symbol";
    case TS_PLUGIN_STAGE_ABI: return "ABI";
    case TS_PLUGIN_STAGE_INITIALIZE: return "initialize";
    case TS_PLUGIN_STAGE_NONE: return "unknown";
  }
  return "unknown";
}

static void ts_set_plugin_load_error(turbo_script_ctx_t *ctx, const char *operation,
                                     const char *name, int load_result,
                                     const ts_plugin_error_t *plugin_error) {
  const char *reason = load_result == TS_PLUGIN_LOAD_DENIED
                           ? "denied by host policy"
                           : "failed to load native plugin";
  if (!ctx) return;
  ctx->error_code = TURBO_SCRIPT_ERROR_PLUGIN;
  if (load_result != TS_PLUGIN_LOAD_DENIED && plugin_error &&
      plugin_error->code != TS_PLUGIN_ERROR_NONE) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s '%s': %s stage failed: %s",
             operation, name ? name : "", ts_plugin_stage_name(plugin_error->stage),
             plugin_error->message[0] ? plugin_error->message : reason);
  } else {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s '%s': %s", operation,
             name ? name : "", reason);
  }
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
        exprtk_value_t val = {0};
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

static exprtk_value_t ts_export(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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
  salts_fs_buf_t buf;
  exprtk_node_t *ast = NULL;
  char *prev_dir = NULL;
  const char *prev_import_name = NULL;
  exprtk_value_t prev_import_exports = exprtk_val_num(0);
  int prev_import_has_exports = 0;
  exprtk_env_t *prev_import_env = NULL;
  int import_state_pushed = 0;
  int rc = -1;

  if (salts_fs_read_file(filename, &buf) != 0) {
    set_error(ctx, TURBO_SCRIPT_ERROR_IO, "import: failed to read script");
    return -1;
  }

  char *script = (char *)malloc(buf.len + 1);
  if (!script) {
    salts_fs_buf_free(&buf);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "import: out of memory");
    return -1;
  }

  memcpy(script, buf.base, buf.len);
  script[buf.len] = '\0';
  salts_fs_buf_free(&buf);

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
  char resolved[SALTS_FS_MAX_PATH];
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

static exprtk_value_t ts_import(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  int load_result;
  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "import: expected 1 string arg");
    return TS_ZERO;
  }

  char *name = vstr_to_arena(args[0].data.string, &ctx->scratch_arena);
  if (!name) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_OOM, "import: out of memory");
    return TS_ZERO;
  }

  if (ts_is_script_import(name)) {
    return ts_import_script_common(ctx, name, 0);
  }

  if (ts_is_builtin_module(name)) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
  }

  ts_plugin_error_t plugin_error = {0};
  load_result = ts_load_plugin(ctx, name, &plugin_error);
  if (load_result == TS_PLUGIN_LOAD_OK) {
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 1.0};
  }

  ts_set_plugin_load_error(ctx, "import", name, load_result, &plugin_error);
  ctx->env.aborted = 1;
  return TS_ZERO;
}

static exprtk_value_t ts_import_module(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  char *name = NULL;

  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
    TS_ERROR(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "import_module: expected 1 string arg");
    return TS_ZERO;
  }

  name = vstr_to_arena(args[0].data.string, &ctx->scratch_arena);
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

static exprtk_value_t ts_print(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
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
    char text[SALTS_UUID_STRING_SIZE];
    if (salts_uuid_format(&val->data.uuid, text, sizeof(text)) == SALTS_OK) printf("%s", text);
    else printf("[uuid]");
    break;
  }
  case EXPRTK_VAL_DATETIME: {
    char text[64];
    time_t ts = datetime_to_time(&val->data.datetime);
    if (ts != (time_t)-1 && datetime_format_rfc822(ts, text, sizeof(text)) >= 0)
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

turbo_script_ctx_t *turbo_script_init_with_plugin_authorizer(
    turbo_script_init_flags_t flags, turbo_script_plugin_authorizer_fn authorizer,
    void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)calloc(1, sizeof(turbo_script_ctx_t));
  if (!ctx) return NULL;

  atomic_init(&ctx->ref_count, 1U);
  atomic_init(&ctx->closing, 0);
  ctx->owner_thread_token = ts_context_current_thread_token();
  ctx->plugin_authorizer = authorizer;
  ctx->plugin_authorizer_data = user_data;
  if (turbo_script_memory_policy_init(TURBO_SCRIPT_MEMORY_SERVICE,
                                      &ctx->memory_policy) != 0) {
    free(ctx);
    return NULL;
  }
  if (ts_host_registry_init(ctx) != TURBO_SCRIPT_STATUS_OK) {
    free(ctx);
    return NULL;
  }
  exprtk_env_init(&ctx->env);
  ctx->env.max_external_value_bytes = ctx->memory_policy.max_external_value_bytes;
  mem_init(&ctx->scratch_arena, 4096);

  {
    salts_coro_executor_config_t executor_config = SALTS_CORO_EXECUTOR_CONFIG_DEFAULT;
    executor_config.worker_count = 1;
    executor_config.queue_capacity_per_worker = 256;
    executor_config.coroutine_pool.max_capacity = 128;
    ctx->executor = salts_coro_executor_create(&executor_config);
  }
  if (!ctx->executor) {
    turbo_script_free(ctx);
    return NULL;
  }

  if (ts_timer_scheduler_init(ctx, TS_TIMER_DEFAULT_CAPACITY) != 0) {
    turbo_script_free(ctx);
    return NULL;
  }
  if (ts_task_scheduler_init(ctx, TS_TASK_DEFAULT_CAPACITY) != 0) {
    turbo_script_free(ctx);
    return NULL;
  }

  exprtk_env_register_func(&ctx->env, "import", ts_import, ctx);
  exprtk_env_register_func(&ctx->env, "import_module", ts_import_module, ctx);
  exprtk_env_register_func(&ctx->env, "export", ts_export, ctx);
  ts_timer_register_functions(ctx);
  ts_task_register_functions(ctx);

  if (flags == TURBO_SCRIPT_INIT_DEFAULT) {
    exprtk_env_register_func(&ctx->env, "print", ts_print, ctx);
    (void)ts_load_plugin(ctx, "parser", NULL);
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

turbo_script_ctx_t *turbo_script_init(turbo_script_init_flags_t flags) {
  return turbo_script_init_with_plugin_authorizer(flags, NULL, NULL);
}

int turbo_script_load_plugin(turbo_script_ctx_t *ctx, const char *name) {
  int load_result;
  ts_plugin_error_t plugin_error = {0};
  if (!ctx) return -1;
  if (!name || !*name) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "plugin name is empty");
    return -1;
  }
  if (!ts_is_valid_plugin_name(name)) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "invalid plugin name");
    return -1;
  }
  clear_error(ctx);
  load_result = ts_load_plugin(ctx, name, &plugin_error);
  if (load_result != TS_PLUGIN_LOAD_OK) {
    ts_set_plugin_load_error(ctx, "load plugin", name, load_result, &plugin_error);
    return -1;
  }
  return 0;
}

int turbo_script_run(turbo_script_ctx_t *ctx, const char *script) {
  if (!ctx) return -1;
  if (atomic_load_explicit(&ctx->closing, memory_order_acquire)) return -1;
  if (ctx->memory_exhausted) {
    set_error(ctx, TURBO_SCRIPT_ERROR_STATE,
              "memory policy: context is exhausted and must be recreated");
    return -1;
  }
  if (!script) {
    set_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "script is NULL");
    return -1;
  }
  clear_error(ctx);

  if (ts_prepare_expr(ctx, script) != 0) {
    return -1;
  }
  return ts_memory_finish_run(ctx, turbo_script_run_mir_interp(ctx, script));
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
  int ret = turbo_script_compile_mir_interp_ast(ctx, ctx->expr, script);
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

  exprtk_env_sweep_closures(&ctx->env);
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

  if (ctx->memory_exhausted) {
    set_error(ctx, TURBO_SCRIPT_ERROR_STATE,
              "memory policy: context is exhausted and must be recreated");
    return -1;
  }
  return ts_memory_finish_run(ctx, turbo_script_run_mir_interp(ctx, compiled->source));
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
  salts_fs_buf_t buf;
  if (salts_fs_read_file(filename, &buf) != 0) {
    set_error(ctx, TURBO_SCRIPT_ERROR_IO, "run_file: failed to read file");
    return -1;
  }

  /* Ensure null-termination */
  char *script = (char *)malloc(buf.len + 1);
  if (!script) {
    salts_fs_buf_free(&buf);
    set_error(ctx, TURBO_SCRIPT_ERROR_OOM, "run_file: out of memory");
    return -1;
  }

  memcpy(script, buf.base, buf.len);
  script[buf.len] = '\0';
  salts_fs_buf_free(&buf);

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
  exprtk_value_t owned;
  if (exprtk_value_copy_to_env(exprtk_val_str(vstr_from_cstr(value)), &ctx->env,
                               &owned) != 0)
    return;
  exprtk_env_set(&ctx->env, name, owned);
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
  exprtk_value_t owned;
  if (exprtk_value_copy_to_env(exprtk_val_vec((double *)data, len), &ctx->env, &owned) != 0)
    return -1;
  exprtk_env_set(&ctx->env, name, owned);
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

exprtk_value_t turbo_script_value_map(void) { return exprtk_val_map(); }

void turbo_script_value_map_set(exprtk_value_t *map, const char *key, exprtk_value_t value) {
  if (!map || !key) return;
  exprtk_map_set(map, key, value);
}

turbo_script_value_map_iterator_t
turbo_script_value_map_iter_begin(const exprtk_value_t *map) {
  const exprtk_map_iter_t iterator = exprtk_map_iter_begin(map);
  turbo_script_value_map_iterator_t result;
  result.storage = iterator.htab;
  result.position = iterator.pos;
  result.bound = iterator.bound;
  return result;
}

int turbo_script_value_map_iter_next(turbo_script_value_map_iterator_t *iterator,
                                     const char **key, exprtk_value_t *value) {
  exprtk_map_iter_t internal;
  int has_entry;

  if (!iterator || !key || !value) return 0;

  internal.htab = iterator->storage;
  internal.pos = iterator->position;
  internal.bound = iterator->bound;
  has_entry = exprtk_map_iter_next(&internal, key, value);
  iterator->storage = internal.htab;
  iterator->position = internal.pos;
  iterator->bound = internal.bound;
  return has_entry;
}

exprtk_value_t turbo_script_value_list_borrowed(exprtk_value_t *items, size_t count) {
  return exprtk_val_list_ex(items, count, 0);
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
