/**
 * @file test_net_ctx.c
 * @brief Tests for net_ctx — context lifecycle and helpers.
 */
#include "net_ctx.h"
#include "tinytest.h"
#include <string.h>
#include <turbo_buffer.h>
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

            turbo_http_t *c = net_ctx_ensure_client(ctx);
            check_not_null(c);
            check(ctx->client == c);
            check_not_null(turbo_http_get_context(c));

            /* Second call returns same instance */
            turbo_http_t *c2 = net_ctx_ensure_client(ctx);
            check(c == c2);

            net_ctx_destroy(ctx);
        }

        it("should return NULL for NULL context") {
            check_null(net_ctx_ensure_client(NULL));
        }
    }

    describe("net_arena_cstr") {

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
}
