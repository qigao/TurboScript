/**
 * @file test_coro.c
 * @brief Coroutine module unit tests.
 */
#include "coro_ctx.h"
#include "exprtk.h"
#include "tinytest.h"
#include "turbo_str.h"

static coro_registry_t *g_registry;

static exprtk_value_t test_generator(size_t argc,
                                     exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    coro_ctx_t *ctx = coro_get_current();

    (void)argc;
    (void)args;
    (void)user_data;
    if (!ctx) return exprtk_val_num(-1);

    ctx->yield_value = exprtk_val_num(42);
    if (coro_yield() != 0) return exprtk_val_num(-1);

    ctx->yield_value = exprtk_val_num(100);
    if (coro_yield() != 0) return exprtk_val_num(-1);

    return exprtk_val_str(vstr_from_cstr("done"));
}

static void setup_registry(void) {
    g_registry = coro_registry_create();
}

static void teardown_registry(void) {
    coro_registry_destroy(g_registry);
    g_registry = NULL;
}

static void register_generator(exprtk_env_t *env) {
    exprtk_env_register_func(env, "generator", test_generator, NULL);
}

spec("coro_module") {
    before_each() { setup_registry(); }
    after_each() { teardown_registry(); }

    describe("registry") {
        it("creates an empty registry and pool") {
            check_not_null(g_registry);
            check_not_null(g_registry->pool);
            check(((int)g_registry->count) == (0));
            check(g_registry->capacity >= 16);
            check((g_registry->next_id) == (1));
        }

        it("adds, finds, and removes a context") {
            exprtk_env_t env;
            coro_ctx_t *ctx;

            exprtk_env_init(&env);
            ctx = coro_ctx_create(&env, "generator", 0);
            check_not_null(ctx);

            ctx->id = g_registry->next_id++;
            check((coro_registry_add(g_registry, ctx)) == (0));
            check(coro_registry_find(g_registry, ctx->id) == ctx);
            check(((int)g_registry->count) == (1));

            coro_registry_remove(g_registry, ctx->id);
            check_null(coro_registry_find(g_registry, ctx->id));
            check(((int)g_registry->count) == (0));
            coro_ctx_destroy(ctx);
            exprtk_env_free(&env);
        }
    }

    describe("coroutine_context") {
        it("creates a direct context with the requested function") {
            exprtk_env_t env;
            coro_ctx_t *ctx;

            exprtk_env_init(&env);
            ctx = coro_ctx_create(&env, "generator", 0);
            check_not_null(ctx);
            check_not_null(ctx->coro);
            check(strcmp((ctx->func_name), ("generator")) == 0);
            check((coro_ctx_status(ctx)) == (0));
            check((ctx->pooled) == (0));

            coro_ctx_destroy(ctx);
            exprtk_env_free(&env);
        }

        it("keeps a custom-stack coroutine outside the default pool") {
            exprtk_env_t env;
            coro_ctx_t *ctx;

            exprtk_env_init(&env);
            ctx = coro_ctx_create_pooled(&env, "generator", 128 * 1024,
                                         g_registry->pool);
            check_not_null(ctx);
            check((ctx->pooled) == (0));
            check(((int)turbo_coro_pool_active_count(g_registry->pool)) == (0));

            coro_ctx_destroy(ctx);
            exprtk_env_free(&env);
        }

        it("discards a suspended pooled coroutine safely") {
            exprtk_env_t env;
            coro_ctx_t *ctx;

            exprtk_env_init(&env);
            ctx = coro_ctx_create_pooled(&env, "generator", 0, g_registry->pool);
            check_not_null(ctx);
            check((ctx->pooled) == (1));
            check(((int)turbo_coro_pool_active_count(g_registry->pool)) == (1));

            coro_ctx_destroy(ctx);
            check(((int)turbo_coro_pool_active_count(g_registry->pool)) == (0));
            exprtk_env_free(&env);
        }
    }

    describe("coroutine_execution") {
        it("executes a registered function and preserves yielded values") {
            exprtk_env_t env;
            coro_ctx_t *ctx;
            coro_t *released_coro;

            exprtk_env_init(&env);
            register_generator(&env);
            ctx = coro_ctx_create_pooled(&env, "generator", 0, g_registry->pool);
            check_not_null(ctx);

            check((coro_resume(ctx->coro)) == (0));
            check((coro_state(ctx->coro)) == (coro_SUSPENDED));
            check((ctx->yield_value.type) == (EXPRTK_VAL_NUMBER));
            check(ctx->yield_value.data.number == 42);

            check((coro_resume(ctx->coro)) == (0));
            check((coro_state(ctx->coro)) == (coro_SUSPENDED));
            check(ctx->yield_value.data.number == 100);

            check((coro_resume(ctx->coro)) == (0));
            check((coro_state(ctx->coro)) == (coro_DEAD));
            check((ctx->return_value.type) == (EXPRTK_VAL_STRING));
            check(((int)turbo_coro_pool_active_count(g_registry->pool)) == (1));

            released_coro = ctx->coro;
            coro_ctx_destroy(ctx);
            check(((int)turbo_coro_pool_active_count(g_registry->pool)) == (0));

            ctx = coro_ctx_create_pooled(&env, "generator", 0, g_registry->pool);
            check_not_null(ctx);
            check((ctx->coro) == (released_coro));
            check((coro_resume(ctx->coro)) == (0));
            check((coro_resume(ctx->coro)) == (0));
            check((coro_resume(ctx->coro)) == (0));
            coro_ctx_destroy(ctx);
            exprtk_env_free(&env);
        }

        it("reports a missing script function without synthetic yields") {
            exprtk_env_t env;
            coro_ctx_t *ctx;

            exprtk_env_init(&env);
            ctx = coro_ctx_create(&env, "missing", 0);
            check_not_null(ctx);
            check((coro_resume(ctx->coro)) == (0));
            check((coro_state(ctx->coro)) == (coro_DEAD));
            check((ctx->return_value.type) == (EXPRTK_VAL_STRING));
            check(strcmp((ctx->error_msg), ("coroutine function not found")) == 0);

            coro_ctx_destroy(ctx);
            exprtk_env_free(&env);
        }

        it("rejects resuming a dead coroutine") {
            exprtk_env_t env;
            coro_ctx_t *ctx;

            exprtk_env_init(&env);
            register_generator(&env);
            ctx = coro_ctx_create(&env, "generator", 0);
            check_not_null(ctx);

            check((coro_resume(ctx->coro)) == (0));
            check((coro_resume(ctx->coro)) == (0));
            check((coro_resume(ctx->coro)) == (0));
            check(coro_resume(ctx->coro) != 0);

            coro_ctx_destroy(ctx);
            exprtk_env_free(&env);
        }
    }
}
