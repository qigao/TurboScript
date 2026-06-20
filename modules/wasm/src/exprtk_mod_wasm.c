/**
 * @file exprtk_mod_wasm.c
 * @brief Wasm module for TurboScript — wasm.* functions + handle management.
 */
#include "wasm_ctx.h"

/* == Lifecycle ============================================================ */

void *wasm_ctx_create(void) { return calloc(1, sizeof(wasm_ctx_t)); }

void wasm_ctx_destroy(void *p) {
  wasm_ctx_t *ctx = (wasm_ctx_t *)p;
  int i;
  if (!ctx)
    return;
  for (i = 0; i < WASM_MAX_HANDLES; i++) {
    if (ctx->handles[i]) {
      if (ctx->handles[i]->vm)
        turbo_wasm_vm_destroy(ctx->handles[i]->vm);
      free(ctx->handles[i]);
      ctx->handles[i] = NULL;
    }
  }
  free(ctx);
}

/* == Handle management ==================================================== */

static int wasm_handle_alloc(wasm_ctx_t *ctx, turbo_wasm_vm_t *vm) {
  int i;
  for (i = 0; i < WASM_MAX_HANDLES; i++) {
    if (!ctx->handles[i]) {
      wasm_handle_t *h = (wasm_handle_t *)calloc(1, sizeof(*h));
      if (!h)
        return -1;
      h->vm = vm;
      ctx->handles[i] = h;
      return i;
    }
  }
  return -1;
}

static void wasm_handle_free(wasm_ctx_t *ctx, int handle) {
  if (handle < 0 || handle >= WASM_MAX_HANDLES)
    return;
  if (ctx->handles[handle]) {
    if (ctx->handles[handle]->vm)
      turbo_wasm_vm_destroy(ctx->handles[handle]->vm);
    free(ctx->handles[handle]);
    ctx->handles[handle] = NULL;
  }
}

static wasm_handle_t *wasm_handle_get(wasm_ctx_t *ctx, int handle) {
  if (handle < 0 || handle >= WASM_MAX_HANDLES)
    return NULL;
  return ctx->handles[handle];
}

/* == Error helpers ======================================================== */

static void wasm_set_ctx_error(wasm_ud_t *ud, const char *msg) {
  if (!ud || !ud->ctx)
    return;
  snprintf(ud->ctx->last_error, sizeof(ud->ctx->last_error), "%s",
           msg ? msg : "wasm error");
  WASM_CTX_ABORT(ud);
}

static void wasm_set_handle_error(wasm_ud_t *ud, wasm_handle_t *h, const char *msg) {
  if (h) {
    snprintf(h->error_msg, sizeof(h->error_msg), "%s", msg ? msg : "wasm error");
  }
  wasm_set_ctx_error(ud, msg);
}

/* == API functions ======================================================== */

static exprtk_value_t fn_wasm_open(size_t argc, exprtk_value_t *args, void *user_data) {
  wasm_ud_t *ud = (wasm_ud_t *)user_data;
  turbo_wasm_config_t cfg;
  turbo_wasm_vm_t *vm;
  char *path;
  int handle;

  if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING) {
    wasm_set_ctx_error(ud, "wasm.open: expected (string [, number])");
    return WASM_ZERO;
  }

  cfg = turbo_wasm_config_default();
  if (argc == 2) {
    if (args[1].type != EXPRTK_VAL_NUMBER || args[1].data.number <= 0) {
      wasm_set_ctx_error(ud, "wasm.open: stack size must be positive number");
      return WASM_ZERO;
    }
    cfg.stack_size_bytes = (uint32_t)args[1].data.number;
  }

  path = wasm_arena_cstr(wasm_tmp_arena(ud), args[0].data.string);
  if (!path) {
    wasm_set_ctx_error(ud, "wasm.open: OOM");
    return WASM_ZERO;
  }

  vm = turbo_wasm_vm_create(&cfg);
  if (!vm) {
    wasm_set_ctx_error(ud, "wasm.open: create VM failed");
    return WASM_ZERO;
  }

  if (turbo_wasm_vm_load_file(vm, path) != 0) {
    wasm_set_ctx_error(ud, turbo_wasm_vm_last_error(vm));
    turbo_wasm_vm_destroy(vm);
    return WASM_ZERO;
  }

  handle = wasm_handle_alloc(ud->ctx, vm);
  if (handle < 0) {
    turbo_wasm_vm_destroy(vm);
    wasm_set_ctx_error(ud, "wasm.open: too many open handles");
    return WASM_ZERO;
  }

  ud->ctx->last_error[0] = '\0';
  return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = (double)handle};
}

static exprtk_value_t fn_wasm_close(size_t argc, exprtk_value_t *args, void *user_data) {
  wasm_ud_t *ud = (wasm_ud_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
    wasm_set_ctx_error(ud, "wasm.close: expected number handle");
    return WASM_ZERO;
  }
  wasm_handle_free(ud->ctx, (int)args[0].data.number);
  return WASM_ZERO;
}

static exprtk_value_t fn_wasm_call(size_t argc, exprtk_value_t *args, void *user_data) {
  wasm_ud_t *ud = (wasm_ud_t *)user_data;
  wasm_handle_t *h;
  int handle_id;
  char *func_name;
  uint32_t wasm_argc;
  const char **wasm_argv = NULL;
  size_t i;
  int rc;
  turbo_wasm_value_type_t rt;

  if (argc < 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
    wasm_set_ctx_error(ud, "wasm.call: expected (number, string, ...)");
    return WASM_ZERO;
  }

  handle_id = (int)args[0].data.number;
  h = wasm_handle_get(ud->ctx, handle_id);
  if (!h || !h->vm) {
    wasm_set_ctx_error(ud, "wasm.call: invalid handle");
    return WASM_ZERO;
  }

  func_name = wasm_arena_cstr(wasm_tmp_arena(ud), args[1].data.string);
  if (!func_name) {
    wasm_set_handle_error(ud, h, "wasm.call: OOM");
    return WASM_ZERO;
  }

  wasm_argc = (uint32_t)(argc - 2);
  if (wasm_argc > 0) {
    wasm_argv = (const char **)mem_alloc(wasm_tmp_arena(ud), sizeof(char *) * wasm_argc);
    if (!wasm_argv) {
      wasm_set_handle_error(ud, h, "wasm.call: OOM");
      return WASM_ZERO;
    }
  }

  for (i = 0; i < wasm_argc; ++i) {
    exprtk_value_t *v = &args[i + 2];
    if (v->type == EXPRTK_VAL_STRING) {
      wasm_argv[i] = wasm_arena_cstr(wasm_tmp_arena(ud), v->data.string);
      if (!wasm_argv[i]) {
        wasm_set_handle_error(ud, h, "wasm.call: OOM");
        return WASM_ZERO;
      }
    } else if (v->type == EXPRTK_VAL_NUMBER) {
      char *num = (char *)mem_alloc(wasm_tmp_arena(ud), 64);
      if (!num) {
        wasm_set_handle_error(ud, h, "wasm.call: OOM");
        return WASM_ZERO;
      }
      snprintf(num, 64, "%.17g", v->data.number);
      wasm_argv[i] = num;
    } else {
      wasm_set_handle_error(ud, h, "wasm.call: args must be number/string");
      return WASM_ZERO;
    }
  }

  rc = turbo_wasm_vm_call_argv(h->vm, func_name, wasm_argc, wasm_argv);
  if (rc != 0) {
    wasm_set_handle_error(ud, h, turbo_wasm_vm_last_error(h->vm));
    return WASM_ZERO;
  }

  h->error_msg[0] = '\0';
  ud->ctx->last_error[0] = '\0';

  rt = turbo_wasm_vm_last_result_type(h->vm);
  if (rt == TURBO_WASM_VAL_NONE) {
    return WASM_ZERO;
  }
  if (rt == TURBO_WASM_VAL_I32 || rt == TURBO_WASM_VAL_I64) {
    int64_t out_i64 = 0;
    if (turbo_wasm_vm_last_result_i64(h->vm, &out_i64) != 0) {
      wasm_set_handle_error(ud, h, "wasm.call: failed to read integer result");
      return WASM_ZERO;
    }
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = (double)out_i64};
  }
  if (rt == TURBO_WASM_VAL_F32 || rt == TURBO_WASM_VAL_F64) {
    double out_f64 = 0.0;
    if (turbo_wasm_vm_last_result_f64(h->vm, &out_f64) != 0) {
      wasm_set_handle_error(ud, h, "wasm.call: failed to read float result");
      return WASM_ZERO;
    }
    return (exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = out_f64};
  }

  wasm_set_handle_error(ud, h, "wasm.call: unsupported result type");
  return WASM_ZERO;
}

static exprtk_value_t fn_wasm_last_error(size_t argc, exprtk_value_t *args, void *user_data) {
  wasm_ud_t *ud = (wasm_ud_t *)user_data;
  wasm_handle_t *h = NULL;
  const char *msg;
  size_t len;
  char *out;

  if (argc == 1 && args[0].type == EXPRTK_VAL_NUMBER) {
    h = wasm_handle_get(ud->ctx, (int)args[0].data.number);
  } else if (argc != 0) {
    wasm_set_ctx_error(ud, "wasm.last_error: expected ([number])");
    return WASM_ZERO;
  }

  msg = h ? h->error_msg : ud->ctx->last_error;
  if (!msg || msg[0] == '\0')
    return WASM_ZERO;

  len = strlen(msg);
  out = (char *)mem_alloc(&ud->env->arena, len + 1);
  if (!out)
    return WASM_ZERO;
  memcpy(out, msg, len + 1);
  return (exprtk_value_t){EXPRTK_VAL_STRING, .data.string = {out, len}};
}

/* == Loader =============================================================== */

void wasm_load(void *p, void *e, void *s) {
  wasm_ctx_t *ctx = (wasm_ctx_t *)p;
  exprtk_env_t *env = (exprtk_env_t *)e;
  mem_pool_t *scratch = (mem_pool_t *)s;
  wasm_ud_t *ud;

  if (!ctx || !env)
    return;

  ud = (wasm_ud_t *)mem_alloc(&env->arena, sizeof(*ud));
  if (!ud)
    return;
  ud->ctx = ctx;
  ud->env = env;
  ud->scratch = scratch;

  exprtk_env_register_func(env, "wasm.open", fn_wasm_open, ud);
  exprtk_env_register_func(env, "wasm.call", fn_wasm_call, ud);
  exprtk_env_register_func(env, "wasm.close", fn_wasm_close, ud);
  exprtk_env_register_func(env, "wasm.last_error", fn_wasm_last_error, ud);
}

