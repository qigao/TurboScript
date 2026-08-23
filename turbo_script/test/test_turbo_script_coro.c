#include "tinytest.h"
#include "turbo_script.h"

#include <stdio.h>

static int run_coro_script(turbo_script_ctx_t *ctx, const char *script) {
    int result = turbo_script_run(ctx, script);
    if (result < 0) {
        fprintf(stderr, "coro script failed: %s\n", turbo_script_get_error(ctx));
    }
    return result;
}

spec("turbo_script_coro") {
    describe("manual generator API") {
        it("loads the plugin and advances only when explicitly resumed") {
            turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
            check_not_null(ctx);
            check((run_coro_script(ctx, "import(\"coro\");")) == (0));

            check((run_coro_script(
                    ctx,
                    "func generator(seed) {"
                    "  coro.yield(seed);"
                    "  coro.yield(seed + 1);"
                    "  return seed + 2;"
                    "}"
                    "co = coro.create(\"generator\");"
                    "initial_status = coro.status(co);"
                    "first = coro.resume(co, 40);"
                    "first_status = first.status;"
                    "first_value = first.value;"
                    "second = coro.resume(co);"
                    "second_status = second.status;"
                    "second_value = second.value;"
                    "completion = coro.resume(co);"
                    "completion_status = completion.status;"
                    "completion_value = completion.value;"
                    "destroyed = coro.destroy(co);")) == (0));

            check(strcmp((ts_get_str(ctx, "initial_status")), ("suspended")) == 0);
            check(strcmp((ts_get_str(ctx, "first_status")), ("suspended")) == 0);
            check(fabs((double)(ts_get_num(ctx, "first_value")) - (double)(40)) <= (double)(0.001));
            check(strcmp((ts_get_str(ctx, "second_status")), ("suspended")) == 0);
            check(fabs((double)(ts_get_num(ctx, "second_value")) - (double)(41)) <= (double)(0.001));
            check(strcmp((ts_get_str(ctx, "completion_status")), ("dead")) == 0);
            check(fabs((double)(ts_get_num(ctx, "completion_value")) - (double)(42)) <= (double)(0.001));
            check(fabs((double)(ts_get_num(ctx, "destroyed")) - (double)(1)) <= (double)(0.001));

            turbo_script_free(ctx);
        }

        it("passes a resume value back through coro.yield") {
            turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
            check_not_null(ctx);
            check((run_coro_script(ctx, "import(\"coro\");")) == (0));

            check((run_coro_script(
                    ctx,
                    "func echo_resume() {"
                    "  received = coro.yield(7);"
                    "  return received;"
                    "}"
                    "co = coro.create(\"echo_resume\");"
                    "first = coro.resume(co);"
                    "second = coro.resume(co, 99);"
                    "first_status = first.status;"
                    "first_value = first.value;"
                    "second_status = second.status;"
                    "second_value = second.value;"
                    "destroyed = coro.destroy(co);")) == (0));

            check(strcmp((ts_get_str(ctx, "first_status")), ("suspended")) == 0);
            check(fabs((double)(ts_get_num(ctx, "first_value")) - (double)(7)) <= (double)(0.001));
            check(strcmp((ts_get_str(ctx, "second_status")), ("dead")) == 0);
            check(fabs((double)(ts_get_num(ctx, "second_value")) - (double)(99)) <= (double)(0.001));
            check(fabs((double)(ts_get_num(ctx, "destroyed")) - (double)(1)) <= (double)(0.001));

            turbo_script_free(ctx);
        }

        it("reports a missing function as a dead coroutine") {
            turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
            check_not_null(ctx);
            check((run_coro_script(ctx, "import(\"coro\");")) == (0));

            check((run_coro_script(ctx,
                                "co = coro.create(\"missing_function\");"
                                "result = coro.resume(co);"
                                "result_status = result.status;"
                                "result_value = result.value;"
                                "destroyed = coro.destroy(co);")) == (0));

            check(strcmp((ts_get_str(ctx, "result_status")), ("dead")) == 0);
            check(strcmp((ts_get_str(ctx, "result_value")), ("coroutine function not found")) == 0);
            check(fabs((double)(ts_get_num(ctx, "destroyed")) - (double)(1)) <= (double)(0.001));

            turbo_script_free(ctx);
        }
    }
}
