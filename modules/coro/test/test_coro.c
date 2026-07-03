/**
 * @file test_coro.c
 * @brief Coroutine module unit tests
 */
#define CORO_TESTING
#include "coro_ctx.h"
#include "exprtk.h"
#include "tinytest.h"
#include "turbo_str.h"


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration for testing */
extern exprtk_value_t fn_coro_yield(size_t argc, exprtk_value_t *args, void *user_data);

/* Global registry for tests */
static coro_registry_t *g_registry = NULL;

/* Setup/teardown */
static void setup_registry(void) {
  if (!g_registry) {
    g_registry = coro_registry_create();
  }
}

static void teardown_registry(void) {
  if (g_registry) {
    coro_registry_destroy(g_registry);
    g_registry = NULL;
  }
}

spec("coro_module") {
  before_each() { setup_registry(); }

  after_each() { teardown_registry(); }

  describe("registry") {
    it("should create registry") {
      check_not_null(g_registry);
      check_int_eq(g_registry->count, 0);
      check(g_registry->capacity >= 16);
      check_int_eq(g_registry->next_id, 1);
    }

    it("should add coroutines to registry") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "test_func", 0);
      check_not_null(ctx);

      ctx->id = g_registry->next_id++;
      int result = coro_registry_add(g_registry, ctx);
      check_int_eq(result, 0);
      check_int_eq(g_registry->count, 1);

      /* Cleanup */
      coro_registry_remove(g_registry, ctx->id);
      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }

    it("should find coroutines by ID") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "test_func", 0);
      check_not_null(ctx);

      ctx->id = g_registry->next_id++;
      coro_registry_add(g_registry, ctx);

      coro_ctx_t *found = coro_registry_find(g_registry, ctx->id);
      check_not_null(found);
      check(found == ctx);
      check_int_eq(found->id, ctx->id);

      /* Cleanup */
      coro_registry_remove(g_registry, ctx->id);
      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }

    it("should remove coroutines from registry") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "test_func", 0);
      ctx->id = g_registry->next_id++;
      coro_registry_add(g_registry, ctx);

      check_int_eq(g_registry->count, 1);

      coro_registry_remove(g_registry, ctx->id);
      check_int_eq(g_registry->count, 0);

      coro_ctx_t *found = coro_registry_find(g_registry, ctx->id);
      check_null(found);

      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }
  }

  describe("coroutine_context") {
    it("should create coroutine context") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "my_generator", 0);
      check_not_null(ctx);
      check_not_null(ctx->coro);
      check_not_null(ctx->func_name);
      check_str_eq(ctx->func_name, "my_generator");
      check_int_eq(ctx->status, 0); /* suspended */

      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }

    it("should use custom stack size") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      size_t custom_size = 128 * 1024; /* 128KB */
      coro_ctx_t *ctx = coro_ctx_create(&env, "test_func", custom_size);
      check_not_null(ctx);
      check_not_null(ctx->coro);

      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }

    it("should destroy coroutine context") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "test_func", 0);
      check_not_null(ctx);

      coro_ctx_destroy(ctx);
      /* Should not crash */

      exprtk_env_free(&env);
    }
  }

  describe("coroutine_execution") {
    it("should resume coroutine and yield values") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "generator", 0);
      check_not_null(ctx);
      ctx->id = 1;

      /* Initial state */
      check_int_eq(ctx->status, 0); /* suspended */

      /* First resume - should yield 42 */
      mco_result res = mco_resume(ctx->coro);
      check_int_eq(res, MCO_SUCCESS);

      mco_state state = mco_status(ctx->coro);
      check_int_eq(state, MCO_SUSPENDED);
      check_int_eq(ctx->yield_value.type, EXPRTK_VAL_NUMBER);
      check(ctx->yield_value.data.number == 42.0);

      /* Second resume - should yield 100 */
      res = mco_resume(ctx->coro);
      check_int_eq(res, MCO_SUCCESS);

      state = mco_status(ctx->coro);
      check_int_eq(state, MCO_SUSPENDED);
      check_int_eq(ctx->yield_value.type, EXPRTK_VAL_NUMBER);
      check(ctx->yield_value.data.number == 100.0);

      /* Third resume - should complete with "done" */
      res = mco_resume(ctx->coro);
      check_int_eq(res, MCO_SUCCESS);

      state = mco_status(ctx->coro);
      check_int_eq(state, MCO_DEAD);
      check_int_eq(ctx->return_value.type, EXPRTK_VAL_STRING);

      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }

    it("should not resume dead coroutine") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      coro_ctx_t *ctx = coro_ctx_create(&env, "generator", 0);
      check_not_null(ctx);

      /* Resume until dead */
      mco_resume(ctx->coro);
      mco_resume(ctx->coro);
      mco_resume(ctx->coro);

      mco_state state = mco_status(ctx->coro);
      check_int_eq(state, MCO_DEAD);

      /* Try to resume again - should fail gracefully */
      mco_result res = mco_resume(ctx->coro);
      check(res != MCO_SUCCESS);

      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }
  }

  describe("integration") {
    it("should work with full registry lifecycle") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      /* Create coroutine */
      coro_ctx_t *ctx = coro_ctx_create(&env, "my_coro", 0);
      check_not_null(ctx);

      /* Add to registry */
      ctx->id = g_registry->next_id++;
      int add_result = coro_registry_add(g_registry, ctx);
      check_int_eq(add_result, 0);

      /* Find it */
      coro_ctx_t *found = coro_registry_find(g_registry, ctx->id);
      check_not_null(found);
      check(found == ctx);

      /* Resume it */
      mco_result res = mco_resume(ctx->coro);
      check_int_eq(res, MCO_SUCCESS);

      /* Remove from registry */
      coro_registry_remove(g_registry, ctx->id);
      check_int_eq(g_registry->count, 0);

      /* Destroy */
      coro_ctx_destroy(ctx);
      exprtk_env_free(&env);
    }

    it("should handle multiple coroutines") {
      exprtk_env_t env;
      exprtk_env_init(&env);

      /* Create 3 coroutines */
      coro_ctx_t *ctx1 = coro_ctx_create(&env, "coro1", 0);
      coro_ctx_t *ctx2 = coro_ctx_create(&env, "coro2", 0);
      coro_ctx_t *ctx3 = coro_ctx_create(&env, "coro3", 0);

      check_not_null(ctx1);
      check_not_null(ctx2);
      check_not_null(ctx3);

      /* Add all to registry */
      ctx1->id = g_registry->next_id++;
      ctx2->id = g_registry->next_id++;
      ctx3->id = g_registry->next_id++;

      coro_registry_add(g_registry, ctx1);
      coro_registry_add(g_registry, ctx2);
      coro_registry_add(g_registry, ctx3);

      check_int_eq(g_registry->count, 3);

      /* Find each one */
      check(coro_registry_find(g_registry, ctx1->id) == ctx1);
      check(coro_registry_find(g_registry, ctx2->id) == ctx2);
      check(coro_registry_find(g_registry, ctx3->id) == ctx3);

      /* Cleanup */
      coro_registry_remove(g_registry, ctx1->id);
      coro_registry_remove(g_registry, ctx2->id);
      coro_registry_remove(g_registry, ctx3->id);

      coro_ctx_destroy(ctx1);
      coro_ctx_destroy(ctx2);
      coro_ctx_destroy(ctx3);

      exprtk_env_free(&env);
    }
  }
}
