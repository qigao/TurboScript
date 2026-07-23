/**
 * @file coro_core.c
 * @brief TurboScript coroutine contexts backed by TurboUtils::Core.
 */
#include "coro_ctx.h"
#include "turbo_str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CORO_REGISTRY_INITIAL_CAPACITY = 16,
    CORO_REGISTRY_MAX_CAPACITY = 1024
};

static int coro_legacy_status(coro_state_t state) {
    switch (state) {
        case coro_RUNNING:
            return 1;
        case coro_DEAD:
            return 2;
        case coro_READY:
        case coro_SUSPENDED:
        default:
            return 0;
    }
}

static exprtk_value_t coro_null_value(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static exprtk_value_t coro_error_value(coro_ctx_t *ctx, const char *message) {
    size_t len;
    char *copy;

    if (!ctx || !ctx->env || !message) return coro_null_value();
    len = strlen(message);
    copy = (char *)mem_alloc(&ctx->env->arena, len + 1);
    if (!copy) return coro_null_value();
    memcpy(copy, message, len + 1);
    return exprtk_val_str(tstr_v_from_buf(copy, len));
}

static void coro_set_error(coro_ctx_t *ctx, const char *message) {
    if (!ctx || !message) return;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", message);
    ctx->return_value = coro_error_value(ctx, ctx->error_msg);
}

static void coro_entry_point(coro_t *co, void *arg) {
    coro_ctx_t *ctx = (coro_ctx_t *)arg;
    exprtk_value_t result;

    (void)co;
    if (!ctx) return;

    ctx->status = 1;
    if (!ctx->env || !ctx->func_name ||
        !exprtk_env_has_func(ctx->env, ctx->func_name)) {
        coro_set_error(ctx, "coroutine function not found");
        ctx->status = 2;
        return;
    }

    result = exprtk_call_internal(ctx->func_name,
                                  ctx->resume_argc,
                                  ctx->resume_args,
                                  ctx->env);

    if (ctx->env->flow == exprtk_FLOW_THROW) {
        const char *message = ctx->env->error_msg[0] != '\0'
                                  ? ctx->env->error_msg
                                  : "coroutine function error";
        coro_set_error(ctx, message);
        ctx->env->flow = exprtk_FLOW_NORMAL;
    } else {
        ctx->return_value = result;
    }
    ctx->status = 2;
}

coro_registry_t *coro_registry_create(void) {
    turbo_coro_pool_config_t config;
    coro_registry_t *registry = (coro_registry_t *)calloc(1, sizeof(*registry));

    if (!registry) return NULL;

    registry->capacity = CORO_REGISTRY_INITIAL_CAPACITY;
    registry->coroutines =
        (coro_ctx_t **)calloc(registry->capacity, sizeof(*registry->coroutines));
    if (!registry->coroutines) {
        free(registry);
        return NULL;
    }

    config.initial_capacity = CORO_REGISTRY_INITIAL_CAPACITY;
    config.max_capacity = CORO_REGISTRY_MAX_CAPACITY;
    config.stack_size = 0;
    config.storage_size = 0;
    config.alloc_fn = NULL;
    config.free_fn = NULL;
    config.allocator_data = NULL;
    registry->pool = turbo_coro_pool_create(&config);
    if (!registry->pool) {
        free(registry->coroutines);
        free(registry);
        return NULL;
    }

    registry->next_id = 1;
    return registry;
}

void coro_registry_destroy(coro_registry_t *registry) {
    size_t i;

    if (!registry) return;
    for (i = 0; i < registry->capacity; ++i) {
        coro_ctx_destroy(registry->coroutines[i]);
    }
    turbo_coro_pool_destroy(registry->pool);
    free(registry->coroutines);
    free(registry);
}

int coro_registry_add(coro_registry_t *registry, coro_ctx_t *ctx) {
    size_t slot = SIZE_MAX;
    size_t i;

    if (!registry || !ctx || registry->count >= CORO_REGISTRY_MAX_CAPACITY) return -1;

    for (i = 0; i < registry->capacity; ++i) {
        if (!registry->coroutines[i]) {
            slot = i;
            break;
        }
    }

    if (slot == SIZE_MAX) {
        size_t old_capacity = registry->capacity;
        size_t new_capacity = old_capacity * 2;
        coro_ctx_t **new_array;

        if (new_capacity > CORO_REGISTRY_MAX_CAPACITY) {
            new_capacity = CORO_REGISTRY_MAX_CAPACITY;
        }
        new_array = (coro_ctx_t **)realloc(
            registry->coroutines, new_capacity * sizeof(*registry->coroutines));
        if (!new_array) return -1;
        memset(new_array + old_capacity,
               0,
               (new_capacity - old_capacity) * sizeof(*new_array));
        registry->coroutines = new_array;
        registry->capacity = new_capacity;
        slot = old_capacity;
    }

    registry->coroutines[slot] = ctx;
    ++registry->count;
    return 0;
}

coro_ctx_t *coro_registry_find(coro_registry_t *registry, int id) {
    size_t i;

    if (!registry || id <= 0) return NULL;
    for (i = 0; i < registry->capacity; ++i) {
        coro_ctx_t *ctx = registry->coroutines[i];
        if (ctx && ctx->id == id) return ctx;
    }
    return NULL;
}

void coro_registry_remove(coro_registry_t *registry, int id) {
    size_t i;

    if (!registry || id <= 0) return;
    for (i = 0; i < registry->capacity; ++i) {
        coro_ctx_t *ctx = registry->coroutines[i];
        if (ctx && ctx->id == id) {
            registry->coroutines[i] = NULL;
            --registry->count;
            return;
        }
    }
}

static coro_ctx_t *coro_ctx_create_impl(exprtk_env_t *env,
                                        const char *func_name,
                                        size_t stack_size,
                                        turbo_coro_pool_t *pool) {
    coro_ctx_t *ctx;

    if (!env || !func_name) return NULL;

    ctx = (coro_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->env = env;
    ctx->func_name = strdup(func_name);
    if (!ctx->func_name) {
        free(ctx);
        return NULL;
    }

    if (pool && stack_size == 0) {
        ctx->coro = turbo_coro_pool_acquire(pool, coro_entry_point, ctx);
        ctx->pool = pool;
        ctx->pooled = 1;
    } else {
        coro_opts_t opts = coro_OPTS_DEFAULT;
        opts.stack_size = stack_size;
        opts.user_data = ctx;
        ctx->coro = coro_create(coro_entry_point, ctx, &opts);
    }

    if (!ctx->coro) {
        free(ctx->func_name);
        free(ctx);
        return NULL;
    }

    coro_set_data(ctx->coro, ctx);
    ctx->magic = CORO_CTX_MAGIC;
    ctx->status = coro_legacy_status(coro_state(ctx->coro));
    return ctx;
}

coro_ctx_t *coro_ctx_create(exprtk_env_t *env,
                            const char *func_name,
                            size_t stack_size) {
    return coro_ctx_create_impl(env, func_name, stack_size, NULL);
}

coro_ctx_t *coro_ctx_create_pooled(exprtk_env_t *env,
                                   const char *func_name,
                                   size_t stack_size,
                                   turbo_coro_pool_t *pool) {
    return coro_ctx_create_impl(env, func_name, stack_size, pool);
}

int coro_ctx_status(coro_ctx_t *ctx) {
    if (!ctx || !ctx->coro) return 2;
    ctx->status = coro_legacy_status(coro_state(ctx->coro));
    return ctx->status;
}

void coro_ctx_destroy(coro_ctx_t *ctx) {
    if (!ctx) return;

    ctx->magic = 0;

    if (ctx->coro) {
        coro_t *co = ctx->coro;
        coro_state_t state = coro_state(co);

        coro_set_data(co, NULL);
        ctx->coro = NULL;
        if (ctx->pooled) {
            if (state == coro_DEAD) {
                turbo_coro_pool_release(ctx->pool, co);
            } else {
                turbo_coro_pool_discard_coro(co);
                coro_destroy(co);
            }
        } else {
            coro_destroy(co);
        }
    }

    free(ctx->func_name);
    free(ctx);
}

coro_ctx_t *coro_get_current(void) {
    coro_t *co = coro_running();
    coro_ctx_t *ctx = co ? (coro_ctx_t *)coro_get_data(co) : NULL;
    return ctx && ctx->magic == CORO_CTX_MAGIC ? ctx : NULL;
}
