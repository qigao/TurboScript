#include "tinytest.h"
#include "exprtk.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_script.h"

#include <stdio.h>

static turbo_script_ctx_t *task_test_context(coro_context_t **coro_out) {
  turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  coro_context_t *coro_ctx = coro_context_create(NULL);
  if (!ctx || !coro_ctx) {
    if (ctx) turbo_script_free(ctx);
    if (coro_ctx) coro_context_destroy(coro_ctx);
    return NULL;
  }
  turbo_script_set_coro_context(ctx, coro_ctx);
  *coro_out = coro_ctx;
  return ctx;
}

static void task_test_destroy(turbo_script_ctx_t *ctx, coro_context_t *coro_ctx) {
  turbo_script_free(ctx);
  coro_context_destroy(coro_ctx);
}

static int task_test_run(turbo_script_ctx_t *ctx, const char *script) {
  int result = turbo_script_run(ctx, script);
  if (result < 0)
    fprintf(stderr, "task test script failed: %s (code=%d active=%zu)\n",
            turbo_script_get_error(ctx), (int)turbo_script_get_error_code(ctx),
            turbo_script_task_active_count(ctx));
  return result;
}

spec("turbo_script_task") {
  describe("managed task scheduler") {
    it("fails fast when no CoroNet context is configured") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);
      check_int_lt(task_test_run(ctx, "id = task.spawn(() => 1);"), 0);
      check_int_eq(turbo_script_get_error_code(ctx), TURBO_SCRIPT_ERROR_STATE);
      check_size_eq(turbo_script_task_active_count(ctx), 0);
      turbo_script_free(ctx);
    }

    it("runs yielding callbacks as independently scheduled coroutines") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "first = task.spawn(() => {"
                                     "  task.sleep(10); return 1;"
                                     "});"
                                     "second = task.spawn(() => {"
                                     "  return task.status(first) == \"waiting\";"
                                     "});"),
                   0);
      check_size_eq(turbo_script_task_active_count(ctx), 2);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_size_eq(turbo_script_task_active_count(ctx), 0);
      check_size_eq(turbo_script_task_failed_count(ctx), 0);
      check_int_eq(task_test_run(ctx, "overlapped = task.result(second);"), 0);
      check_float_eq(ts_get_num(ctx, "overlapped"), 1.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }

    it("awaits another task and preserves its result") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "first = task.spawn(() => {"
                                     "  task.sleep(5); return 21;"
                                     "});"
                                     "second = task.spawn(() => {"
                                     "  task.yield(); return task.join(first) * 2;"
                                     "});"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_int_eq(task_test_run(ctx, "first_state = task.status(first);"
                                     "second_state = task.status(second);"
                                     "answer = task.result(second);"),
                   0);
      check_str_eq(ts_get_str(ctx, "first_state"), "completed");
      check_str_eq(ts_get_str(ctx, "second_state"), "completed");
      check_float_eq(ts_get_num(ctx, "answer"), 42.0, 0.001);

      check_int_eq(task_test_run(ctx, "third = task.spawn(() => { return task.join(first) + 1; });"), 0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_int_eq(task_test_run(ctx, "joined_completed = task.result(third);"), 0);
      check_float_eq(ts_get_num(ctx, "joined_completed"), 22.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }

    it("dispatches dotted native calls from concise task callbacks") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "first = task.spawn(() => {"
                                     "  task.sleep(5); return 41;"
                                     "});"
                                     "second = task.spawn(() => task.join(first) + 1);"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_int_eq(task_test_run(ctx, "concise_result = task.result(second);"), 0);
      check_float_eq(ts_get_num(ctx, "concise_result"), 42.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }

    it("retains task errors without poisoning the root environment") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "failed = task.spawn(() => { throw \"boom\"; });"), 0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_size_eq(turbo_script_task_failed_count(ctx), 1);
      check_int_eq(task_test_run(ctx, "failed_state = task.status(failed);"
                                     "failure = task.error(failed);"),
                   0);
      check_str_eq(ts_get_str(ctx, "failed_state"), "failed");
      check_str_eq(ts_get_str(ctx, "failure"), "boom");
      task_test_destroy(ctx, coro_ctx);
    }

    it("propagates a joined task failure to its waiter") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "failed = task.spawn(() => {"
                                     "  task.yield(); throw \"joined boom\";"
                                     "});"
                                     "waiter = task.spawn(() => { return task.join(failed); });"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_size_eq(turbo_script_task_failed_count(ctx), 2);
      check_int_eq(task_test_run(ctx, "waiter_state = task.status(waiter);"
                                     "waiter_error = task.error(waiter);"),
                   0);
      check_str_eq(ts_get_str(ctx, "waiter_state"), "failed");
      check_str_eq(ts_get_str(ctx, "waiter_error"), "joined boom");
      task_test_destroy(ctx, coro_ctx);
    }

    it("cancels a sleeping task and reaches a terminal state") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "sleeper = task.spawn(() => {"
                                     "  task.sleep(5000); return 1;"
                                     "});"
                                     "canceller = task.spawn(() => {"
                                     "  task.yield(); return task.cancel(sleeper);"
                                     "});"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_size_eq(turbo_script_task_active_count(ctx), 0);
      check_int_eq(task_test_run(ctx, "sleeper_state = task.status(sleeper);"
                                     "sleeper_error = task.error(sleeper);"
                                     "cancel_accepted = task.result(canceller);"),
                   0);
      check_str_eq(ts_get_str(ctx, "sleeper_state"), "cancelled");
      check_str_eq(ts_get_str(ctx, "sleeper_error"), "task was cancelled");
      check_float_eq(ts_get_num(ctx, "cancel_accepted"), 1.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }

    it("removes a cancelled join waiter without retaining the target slot") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "target = task.spawn(() => {"
                                     "  task.sleep(5000); return 7;"
                                     "});"
                                     "waiter = task.spawn(() => task.join(target));"
                                     "canceller = task.spawn(() => {"
                                     "  task.yield();"
                                     "  waiter_cancelled = task.cancel(waiter);"
                                     "  target_cancelled = task.cancel(target);"
                                     "  return waiter_cancelled && target_cancelled;"
                                     "});"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_size_eq(turbo_script_task_active_count(ctx), 0);
      check_int_eq(task_test_run(ctx, "waiter_state = task.status(waiter);"
                                     "target_state = task.status(target);"
                                     "both_cancelled = task.result(canceller);"
                                     "waiter_released = task.release(waiter);"
                                     "target_released = task.release(target);"),
                   0);
      check_str_eq(ts_get_str(ctx, "waiter_state"), "cancelled");
      check_str_eq(ts_get_str(ctx, "target_state"), "cancelled");
      check_float_eq(ts_get_num(ctx, "both_cancelled"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "waiter_released"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "target_released"), 1.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }

    it("shuts down all scheduled tasks and rejects new work") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "first = task.spawn(() => { task.sleep(5000); });"
                                     "second = task.spawn(() => { task.sleep(5000); });"
                                     "timer_job = timer.after(5000, () => 1);"
                                     "shutdown_count = task.shutdown();"),
                   0);
      check_float_eq(ts_get_num(ctx, "shutdown_count"), 2.0, 0.001);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_size_eq(turbo_script_task_active_count(ctx), 0);
      check_int_eq(task_test_run(ctx, "first_state = task.status(first);"
                                     "second_state = task.status(second);"
                                     "timer_state = timer.status(timer_job);"),
                   0);
      check_str_eq(ts_get_str(ctx, "first_state"), "cancelled");
      check_str_eq(ts_get_str(ctx, "second_state"), "cancelled");
      check_str_eq(ts_get_str(ctx, "timer_state"), "cancelled");
      check_int_lt(task_test_run(ctx, "rejected = task.spawn(() => 1);"), 0);
      check_int_eq(turbo_script_get_error_code(ctx), TURBO_SCRIPT_ERROR_STATE);
      task_test_destroy(ctx, coro_ctx);
    }

    it("requires explicit release before a bounded result slot is reused") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);
      check_int_eq(turbo_script_set_task_capacity(ctx, 1), 0);

      check_int_eq(task_test_run(ctx, "first = task.spawn(() => 7);"), 0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_int_lt(task_test_run(ctx, "blocked = task.spawn(() => 8);"), 0);
      check_int_eq(turbo_script_get_error_code(ctx), TURBO_SCRIPT_ERROR_STATE);
      check_int_eq(task_test_run(ctx, "released = task.release(first);"
                                     "second = task.spawn(() => 8);"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_int_eq(task_test_run(ctx, "value = task.result(second);"), 0);
      check_float_eq(ts_get_num(ctx, "released"), 1.0, 0.001);
      check_float_eq(ts_get_num(ctx, "value"), 8.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }

    it("keeps a scheduled task alive through deferred context shutdown") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);
      check_int_eq(task_test_run(ctx, "pending = task.spawn(() => { task.sleep(5); });"), 0);
      check_size_eq(turbo_script_task_active_count(ctx), 1);

      turbo_script_free(ctx);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      coro_context_destroy(coro_ctx);
    }

    it("does not confuse manual coro.yield with a managed task context") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = task_test_context(&coro_ctx);
      check_not_null(ctx);

      check_int_eq(task_test_run(ctx, "import(\"coro\");"
                                     "managed = task.spawn(() => {"
                                     "  coro.yield(7); return 9;"
                                     "});"),
                   0);
      check_int_eq(coro_context_run(coro_ctx, TURBO_RUN_DEFAULT), 0);
      check_int_eq(task_test_run(ctx, "managed_result = task.result(managed);"), 0);
      check_float_eq(ts_get_num(ctx, "managed_result"), 9.0, 0.001);
      task_test_destroy(ctx, coro_ctx);
    }
  }
}
