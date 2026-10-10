/**
 * @file test_net_ctx.c
 * @brief Tests for net_ctx — context lifecycle and helpers.
 */
#include "net_ctx.h"
#include "exprtk_module.h"
#include "tinytest.h"
#include <string.h>
#include <cmeta_buffer.h>

/* Exercise the registered native boundary without the script dispatcher's
 * compatibility conversion from integer to number. */
static exprtk_value_t call_ws_consume(
    exprtk_env_t *env, exprtk_value_t args[2]) {
    for (exprtk_func_t *function = env->funcs; function; function = function->next) {
        if (strcmp(function->name, "ws.consume") == 0) {
            return function->data.native.fn(
                2u, args, env, function->data.native.user_data);
        }
    }
    check(false);
    return NET_ZERO;
}

spec("net_ctx") {

    describe("Lifecycle") {

        it("should create and destroy context") {
            net_ctx_t *ctx = net_ctx_create();
            check_not_null(ctx);
            check_null(ctx->client);
            check_null(ctx->ws_client);
            check((ctx->ws_task_connection_count) == (0));
            check(strcmp((ctx->error_msg), ("")) == 0);
            net_ctx_destroy(ctx);
        }

        it("should handle destroy of NULL gracefully") {
            net_ctx_destroy(NULL);
        }
    }

    describe("ensure_client") {

        it("should lazily create client on first call") {
            net_ctx_t *ctx = net_ctx_create();
            check_null(ctx->client);

            chttp_client *c = net_ctx_ensure_client(ctx);
            check_not_null(c);
            check(ctx->client == c);
            check_not_null(c->impl);

            /* Second call returns same instance */
            chttp_client *c2 = net_ctx_ensure_client(ctx);
            check(c == c2);

            net_ctx_destroy(ctx);
        }

        it("should return NULL for NULL context") {
            check_null(net_ctx_ensure_client(NULL));
        }
    }

    describe("net_arena_cstr") {

        it("rejects invalid views and overflowing terminator allocation") {
            mem_pool_t arena = {0};
            mem_init(&arena, 256);
            check_null(net_arena_cstr(NULL, vstr_from_cstr("value")));
            check_null(net_arena_cstr(&arena, ((vstr){NULL, 1u})));
            check_null(net_arena_cstr(&arena, ((vstr){"x", SIZE_MAX})));
            check_equal(net_arena_cstr(&arena, ((vstr){NULL, 0u})), "");
            mem_destroy(&arena);
        }

        it("should copy string view to arena as null-terminated C string") {
            mem_pool_t arena = {0};
            mem_init(&arena, 256);

            vstr sv = vstr_from_buf("hello", 5);
            char *cstr = net_arena_cstr(&arena, sv);

            check_not_null(cstr);
            check(strcmp((cstr), ("hello")) == 0);
            check((strlen(cstr)) == (5));

            mem_destroy(&arena);
        }

        it("should allocate multiple strings from same arena") {
            mem_pool_t arena = {0};
            mem_init(&arena, 256);

            vstr sv1 = vstr_from_buf("hello", 5);
            vstr sv2 = vstr_from_buf("world", 5);

            char *cstr1 = net_arena_cstr(&arena, sv1);
            char *cstr2 = net_arena_cstr(&arena, sv2);

            check_not_null(cstr1);
            check_not_null(cstr2);
            check(cstr1 != cstr2);
            check(strcmp((cstr1), ("hello")) == 0);
            check(strcmp((cstr2), ("world")) == 0);

            mem_destroy(&arena);
        }

        it("should handle empty string view") {
            mem_pool_t arena = {0};
            mem_init(&arena, 256);

            vstr sv = vstr_from_buf("", 0);
            char *cstr = net_arena_cstr(&arena, sv);

            check_not_null(cstr);
            check(strcmp((cstr), ("")) == 0);

            mem_destroy(&arena);
        }
    }

    describe("WebSocket script boundary") {
        static net_ctx_t *ctx;
        static exprtk_env_t env;
        static mem_pool_t scratch;

        before_each() {
            ctx = net_ctx_create();
            exprtk_env_init(&env);
            mem_init(&scratch, 256);
            net_load(ctx, &env, &scratch);
        }

        after_each() {
            exprtk_env_free(&env);
            net_ctx_destroy(ctx);
            mem_destroy(&scratch);
        }

        it("accepts integer timeouts before checking the connection") {
            exprtk_value_t args[2] = {0};
            exprtk_value_t result;
            args[0] = exprtk_val_int(1);
            args[1].type = EXPRTK_VAL_FUNCTION;
            result = call_ws_consume(&env, args);
            check_equal(result.data.number, 0.0);
            check_equal(ctx->error_msg, "ws client not connected");
            exprtk_value_destroy(&result);
        }

        it("rejects fractional and out of range timeouts") {
            exprtk_value_t args[2] = {0};
            exprtk_value_t result;
            args[1].type = EXPRTK_VAL_FUNCTION;
            args[0] = exprtk_val_num(1.5);
            result = call_ws_consume(&env, args);
            check_equal(result.data.number, 0.0);
            check_equal(ctx->error_msg, "");
            exprtk_value_destroy(&result);
            args[0] = exprtk_val_int((int64_t)UINT32_MAX + 1);
            result = call_ws_consume(&env, args);
            check_equal(result.data.number, 0.0);
            check_equal(ctx->error_msg, "");
            exprtk_value_destroy(&result);
        }

        it("releases a disconnected client and permits repeated close") {
            chttp_websocket_client_config config = {0};
            exprtk_value_t result;
            config.size = sizeof(config);
#if defined(_WIN32)
            config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
            config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
            config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
            config.network.connection_capacity = 1u;
            config.network.command_capacity = 16u;
            config.network.request_capacity = 8u;
            config.network.completion_batch_capacity = 8u;
            config.network.event_capacity = 16u;
            config.network.max_send_bytes = 1024u * 1024u;
            config.network.receive_buffer_bytes = 64u * 1024u;
            ctx->ws_client = calloc(1u, sizeof(*ctx->ws_client));
            check_not_null(ctx->ws_client);
            check_equal(chttp_websocket_client_init(ctx->ws_client, &config), SALTS_OK);
            result = exprtk_call_internal("ws.close", 0u, NULL, &env);
            check_equal(result.data.number, 1.0);
            check_null(ctx->ws_client);
            exprtk_value_destroy(&result);
            result = exprtk_call_internal("ws.close", 0u, NULL, &env);
            check_equal(result.data.number, 1.0);
            check_equal(ctx->error_msg, "");
            exprtk_value_destroy(&result);
        }
    }
}
