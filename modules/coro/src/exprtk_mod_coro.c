/**
 * @file exprtk_mod_coro.c
 * @brief Coroutine module: coro.* functions for TurboScript.
 */
#include "coro_ctx.h"
#include "exprtk_module.h"
#include "turbo_str.h"
#include "../src/minicoro.h"
#include <string.h>

/* Module user data */
typedef struct {
    coro_registry_t *registry;
    exprtk_env_t *env;
    mem_pool_t *scratch;
} coro_mod_t;

/* Helper: create result map */
static exprtk_value_t make_result_map(mem_pool_t *scratch, const char *status, exprtk_value_t value) {
    exprtk_value_t map = exprtk_val_map();

    (void)scratch;
    exprtk_map_set(&map, "status", exprtk_val_str(tstr_v_from_cstr(status)));

    /* Value */
    exprtk_map_set(&map, "value", value);
    
    return map;
}

static exprtk_value_t coro_str(const char *text) {
    return exprtk_val_str(tstr_v_from_cstr(text));
}

static exprtk_value_t coro_tstr_to_arena(mem_pool_t *arena, tstr_t text) {
    size_t len;
    char *buf;
    exprtk_value_t value;
    if (!text) return exprtk_val_num(0);
    len = tstr_len(text);
    buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) {
        tstr_free(text);
        return exprtk_val_num(0);
    }
    if (len > 0) memcpy(buf, text, len);
    buf[len] = '\0';
    value = exprtk_val_str(tstr_v_from_buf(buf, len));
    tstr_free(text);
    return value;
}

/* ========================================================================
 * coro.create(func_name, [stack_size])
 * ======================================================================== */
static exprtk_value_t fn_coro_create(size_t argc, exprtk_value_t *args, void *user_data) {
    coro_mod_t *mod = (coro_mod_t *)user_data;
    
    /* Validate arguments */
    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
        return exprtk_val_num(0); /* Invalid */
    }
    
    /* Extract function name */
    tstr_v func_name_sv = args[0].data.string;
    char *func_name = (char *)mem_alloc(mod->scratch, func_name_sv.len + 1);
    if (!func_name) return exprtk_val_num(0);
    
    memcpy(func_name, func_name_sv.data, func_name_sv.len);
    func_name[func_name_sv.len] = '\0';
    
    /* Optional stack size */
    size_t stack_size = 0;
    if (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) {
        stack_size = (size_t)args[1].data.number;
    }
    
    /* Create coroutine context */
    coro_ctx_t *ctx = coro_ctx_create(mod->env, func_name, stack_size);
    if (!ctx) {
        return exprtk_val_num(0);
    }
    
    /* Assign ID and add to registry */
    ctx->id = mod->registry->next_id++;
    if (coro_registry_add(mod->registry, ctx) != 0) {
        coro_ctx_destroy(ctx);
        return exprtk_val_num(0);
    }
    
    /* Return coroutine ID */
    return exprtk_val_int(ctx->id);
}

/* ========================================================================
 * coro.resume(co_id, [...args])
 * ======================================================================== */
static exprtk_value_t fn_coro_resume(size_t argc, exprtk_value_t *args, void *user_data) {
    coro_mod_t *mod = (coro_mod_t *)user_data;
    
    /* Validate arguments - accept both INTEGER and NUMBER */
    if (argc < 1) {
        return make_result_map(mod->scratch, "error", coro_str("missing coroutine ID argument"));
    }
    
    int co_id = 0;
    if (args[0].type == EXPRTK_VAL_INTEGER) {
        co_id = (int)args[0].data.integer;
    } else if (args[0].type == EXPRTK_VAL_NUMBER) {
        co_id = (int)args[0].data.number;
    } else {
        return make_result_map(mod->scratch, "error", coro_str("invalid coroutine ID type"));
    }
    
    coro_ctx_t *ctx = coro_registry_find(mod->registry, co_id);
    
    if (!ctx) {
        return make_result_map(mod->scratch, "error", coro_str("coroutine not found"));
    }
    
    if (ctx->status == 2) { /* dead */
        return make_result_map(mod->scratch, "dead", ctx->return_value);
    }
    
    if (ctx->status == 1) { /* running */
        return make_result_map(mod->scratch, "error", coro_str("coroutine is already running"));
    }
    
    /* Save resume arguments (for Phase 2) */
    ctx->resume_args = (argc > 1) ? &args[1] : NULL;
    ctx->resume_argc = (argc > 1) ? argc - 1 : 0;
    
    /* Resume coroutine */
    ctx->status = 1; /* running */
    mco_result res = mco_resume(ctx->coro);
    
    if (res != MCO_SUCCESS) {
        ctx->status = 2; /* dead on error */
        const char *err_desc = mco_result_description(res);
        tstr_t error = tstr_new();
        if (!error) return exprtk_val_map();
        error = tstr_cat_fmt(error, "resume failed: %s", err_desc ? err_desc : "");
        return make_result_map(mod->scratch, "error", coro_tstr_to_arena(mod->scratch, error));
    }
    
    /* Check status after resume */
    mco_state state = mco_status(ctx->coro);
    
    if (state == MCO_SUSPENDED) {
        ctx->status = 0; /* suspended */
        return make_result_map(mod->scratch, "suspended", ctx->yield_value);
    } else if (state == MCO_DEAD) {
        ctx->status = 2; /* dead */
        return make_result_map(mod->scratch, "dead", ctx->return_value);
    } else {
        ctx->status = 2; /* dead on unexpected state */
        return make_result_map(mod->scratch, "error", coro_str("unexpected coroutine state"));
    }
}

/* ========================================================================
 * coro.yield(value)
 * ======================================================================== */
exprtk_value_t fn_coro_yield(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    
    /* Get current coroutine using mco_running() */
    mco_coro *running_coro = mco_running();
    if (!running_coro) {
        return exprtk_val_num(0); /* Not in coroutine context */
    }
    
    /* Get coroutine context from user_data */
    coro_ctx_t *ctx = (coro_ctx_t *)mco_get_user_data(running_coro);
    if (!ctx || ctx->status != 1) { /* not running */
        return exprtk_val_num(0);
    }
    
    /* Save yield value */
    if (argc > 0) {
        ctx->yield_value = args[0];
    } else {
        /* Create null value */
        exprtk_value_t null_val;
        memset(&null_val, 0, sizeof(null_val));
        null_val.type = EXPRTK_VAL_NULL;
        ctx->yield_value = null_val;
    }
    
    /* Yield back to caller
     * This will save the entire C call stack (including exprtk execution state)
     * and switch back to the main stack
     */
    ctx->status = 0; /* suspended */
    mco_yield(running_coro);
    ctx->status = 1; /* running again after resume */
    
    /* When we get here, mco_resume() has been called again
     * Return resume arguments if any
     */
    if (ctx->resume_argc > 0) {
        return ctx->resume_args[0];
    }
    
    /* Return null */
    exprtk_value_t null_val;
    memset(&null_val, 0, sizeof(null_val));
    null_val.type = EXPRTK_VAL_NULL;
    return null_val;
}

/* ========================================================================
 * coro.status(co_id)
 * ======================================================================== */
static exprtk_value_t fn_coro_status(size_t argc, exprtk_value_t *args, void *user_data) {
    coro_mod_t *mod = (coro_mod_t *)user_data;
    
    if (argc < 1) {
        return coro_str("invalid");
    }
    
    int co_id = 0;
    if (args[0].type == EXPRTK_VAL_INTEGER) {
        co_id = (int)args[0].data.integer;
    } else if (args[0].type == EXPRTK_VAL_NUMBER) {
        co_id = (int)args[0].data.number;
    } else {
        return coro_str("invalid");
    }
    
    coro_ctx_t *ctx = coro_registry_find(mod->registry, co_id);
    
    if (!ctx) {
        return coro_str("invalid");
    }
    
    const char *status_str;
    switch (ctx->status) {
        case 0: status_str = "suspended"; break;
        case 1: status_str = "running"; break;
        case 2: status_str = "dead"; break;
        default: status_str = "unknown"; break;
    }
    
    return coro_str(status_str);
}

/* ========================================================================
 * coro.destroy(co_id)
 * ======================================================================== */
static exprtk_value_t fn_coro_destroy(size_t argc, exprtk_value_t *args, void *user_data) {
    coro_mod_t *mod = (coro_mod_t *)user_data;
    
    if (argc < 1) {
        return exprtk_val_num(0);
    }
    
    int co_id = 0;
    if (args[0].type == EXPRTK_VAL_INTEGER) {
        co_id = (int)args[0].data.integer;
    } else if (args[0].type == EXPRTK_VAL_NUMBER) {
        co_id = (int)args[0].data.number;
    } else {
        return exprtk_val_num(0);
    }
    
    coro_ctx_t *ctx = coro_registry_find(mod->registry, co_id);
    
    if (!ctx) {
        return exprtk_val_num(0);
    }
    
    /* Cannot destroy running coroutine */
    if (ctx->status == 1) {
        return exprtk_val_num(0);
    }
    
    /* Remove from registry and destroy */
    coro_registry_remove(mod->registry, co_id);
    coro_ctx_destroy(ctx);
    
    return exprtk_val_num(1);
}

/* ========================================================================
 * Module Loader
 * ======================================================================== */

void *coro_ctx_create_module(void) {
    return coro_registry_create();
}

void coro_ctx_destroy_module(void *p) {
    coro_registry_destroy((coro_registry_t *)p);
}

void coro_load(void *p, void *e, void *s) {
    coro_registry_t *registry = (coro_registry_t *)p;
    exprtk_env_t *env = (exprtk_env_t *)e;
    mem_pool_t *scratch = (mem_pool_t *)s;
    
    if (!registry || !env) return;
    
    /* Allocate module data */
    coro_mod_t *mod = (coro_mod_t *)mem_alloc(&env->arena, sizeof(coro_mod_t));
    if (!mod) return;
    
    mod->registry = registry;
    mod->env = env;
    mod->scratch = scratch;
    
    /* Register functions */
    exprtk_env_register_func(env, "coro.create", fn_coro_create, mod);
    exprtk_env_register_func(env, "coro.resume", fn_coro_resume, mod);
    exprtk_env_register_func(env, "coro.yield", fn_coro_yield, mod);
    exprtk_env_register_func(env, "coro.status", fn_coro_status, mod);
    exprtk_env_register_func(env, "coro.destroy", fn_coro_destroy, mod);
}

/* ========================================================================
 * Module Descriptor
 * ======================================================================== */

static const exprtk_func_entry_t coro_functions[] = {
    {"coro_create", (exprtk_builtin_fn)fn_coro_create},
    {"coro_resume", (exprtk_builtin_fn)fn_coro_resume},
    {"coro_yield", (exprtk_builtin_fn)fn_coro_yield},
    {"coro_status", (exprtk_builtin_fn)fn_coro_status},
    {"coro_destroy", (exprtk_builtin_fn)fn_coro_destroy},
};

static const exprtk_module_t coro_module = {
    .module_name = "coro",
    .entries = coro_functions,
    .count = sizeof(coro_functions) / sizeof(coro_functions[0]),
};

const exprtk_module_t *exprtk_module_coro(void) { 
    return &coro_module; 
}

