/**
 * @file mir_mod.c
 * @brief Script-level `mir.load` / `mir.call` built-in functions for turbo_script.
 *
 * These are exprtk_native_fn (user_data-based) functions registered via
 * exprtk_env_register_func. The user_data is a mir_instance_t*.
 */
#include "turbo_script_internal.h"
#include "turbo_buffer.h"
#include "exprtk.h"
#include "exprtk_module.h"
#include <mir.h>
#include <stdlib.h>
#include <string.h>

static MIR_item_t mir_get_global_item(MIR_context_t ctx, const char *name) {
  if (!ctx)
    return NULL;
  DLIST(MIR_module_t) *modules = MIR_get_module_list(ctx);
  for (MIR_module_t m = DLIST_HEAD(MIR_module_t, *modules); m != NULL;
       m = DLIST_NEXT(MIR_module_t, m)) {
    for (MIR_item_t it = DLIST_HEAD(MIR_item_t, m->items); it != NULL;
         it = DLIST_NEXT(MIR_item_t, it)) {
      const char *it_name = MIR_item_name(ctx, it);
      if (it_name && strcmp(it_name, name) == 0)
        return it;
    }
  }
  return NULL;
}

static exprtk_value_t ts_mir_load(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
    return exprtk_val_num(0);

  if (!ctx->mir_ctx)
    ctx->mir_ctx = MIR_init();

  /* Null-terminate the script string in a stack buffer or malloc */
  size_t len = args[0].data.string.len;
  char *script = (char *)malloc(len + 1);
  if (!script)
    return exprtk_val_num(0);
  memcpy(script, args[0].data.string.data, len);
  script[len] = '\0';

  DLIST(MIR_module_t) *modules = MIR_get_module_list(ctx->mir_ctx);
  MIR_module_t last_before = DLIST_TAIL(MIR_module_t, *modules);

  MIR_scan_string(ctx->mir_ctx, script);
  free(script);

  MIR_module_t starting =
      last_before ? DLIST_NEXT(MIR_module_t, last_before) : DLIST_HEAD(MIR_module_t, *modules);
  for (MIR_module_t m = starting; m != NULL; m = DLIST_NEXT(MIR_module_t, m))
    MIR_load_module(ctx->mir_ctx, m);

  MIR_link(ctx->mir_ctx, MIR_set_interp_interface, NULL);
  return exprtk_val_num(1.0);
}

static exprtk_value_t ts_mir_call(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  if (argc < 1 || args[0].type != EXPRTK_VAL_STRING)
    return exprtk_val_num(0);
  if (!ctx->mir_ctx)
    return exprtk_val_num(0);

  size_t name_len = args[0].data.string.len;
  char *func_name = (char *)malloc(name_len + 1);
  if (!func_name)
    return exprtk_val_num(0);
  memcpy(func_name, args[0].data.string.data, name_len);
  func_name[name_len] = '\0';

  MIR_item_t func_item = mir_get_global_item(ctx->mir_ctx, func_name);
  free(func_name);

  if (!func_item || func_item->item_type != MIR_func_item)
    return exprtk_val_num(0);

  size_t mir_argc = argc - 1;
  MIR_val_t *mir_args = NULL;
  if (mir_argc > 0) {
    mir_args = (MIR_val_t *)malloc(mir_argc * sizeof(MIR_val_t));
    if (!mir_args)
      return exprtk_val_num(0);
  }

  for (size_t i = 0; i < mir_argc; i++) {
    if (args[i + 1].type == EXPRTK_VAL_NUMBER)
      mir_args[i].d = args[i + 1].data.number;
    else if (args[i + 1].type == EXPRTK_VAL_STRING)
      mir_args[i].a = (void *)args[i + 1].data.string.data;
    else
      mir_args[i].i = 0;
  }

  MIR_val_t result;
  MIR_interp_arr(ctx->mir_ctx, func_item, &result, mir_argc, mir_args);
  free(mir_args);

  if (func_item->item_type == MIR_func_item && func_item->u.func->nres > 0) {
    MIR_type_t type = func_item->u.func->res_types[0];
    if (type == MIR_T_F)
      return exprtk_val_num(result.f);
    if (type == MIR_T_D)
      return exprtk_val_num(result.d);
    if (type == MIR_T_I64 || type == MIR_T_I32 || type == MIR_T_I16 || type == MIR_T_I8)
      return exprtk_val_num((double)result.i);
    if (type == MIR_T_U64 || type == MIR_T_U32 || type == MIR_T_U16 || type == MIR_T_U8)
      return exprtk_val_num((double)result.u);
  }

  return exprtk_val_num(result.d);
}

void turbo_script_register_mir(turbo_script_ctx_t *ctx) {
  exprtk_env_register_func(&ctx->env, "mir.load", ts_mir_load, ctx);
  exprtk_env_register_func(&ctx->env, "mir.call", ts_mir_call, ctx);
  ctx->env.eval_node = turbo_script_mir_eval_node;
  ctx->env.exec_script_body = turbo_script_mir_exec_script_body;
}
