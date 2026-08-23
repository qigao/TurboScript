#include "tinytest.h"
#include "exprtk.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_script.h"
#include "turbo_thread.h"
#include <stdatomic.h>
#include <stdio.h>

typedef struct {
  turbo_script_executor_task_fn task;
  void *arg1;
  void *arg2;
  atomic_int ready;
} queued_executor_t;

typedef struct {
  coro_context_t *coro_ctx;
  atomic_int active;
  atomic_int calls;
  atomic_int max_active;
  atomic_int inside_coro;
} timer_coro_probe_t;

static exprtk_value_t timer_coro_probe(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  timer_coro_probe_t *probe = (timer_coro_probe_t *)user_data;
  int current;
  int maximum;
  (void)argc;
  (void)args;

  current = atomic_fetch_add_explicit(&probe->active, 1, memory_order_acq_rel) + 1;
  maximum = atomic_load_explicit(&probe->max_active, memory_order_acquire);
  while (maximum < current &&
         !atomic_compare_exchange_weak_explicit(&probe->max_active, &maximum, current,
                                                memory_order_acq_rel, memory_order_acquire)) {
  }

  if (coro_running()) {
    atomic_fetch_add_explicit(&probe->inside_coro, 1, memory_order_acq_rel);
    coro_sleep(probe->coro_ctx, 20);
  }

  atomic_fetch_sub_explicit(&probe->active, 1, memory_order_acq_rel);
  atomic_fetch_add_explicit(&probe->calls, 1, memory_order_acq_rel);
  return exprtk_val_num(1.0);
}

static int queued_executor_post(void *executor_data, turbo_script_executor_task_fn task, void *arg1,
                                void *arg2) {
  queued_executor_t *executor = (queued_executor_t *)executor_data;
  executor->task = task;
  executor->arg1 = arg1;
  executor->arg2 = arg2;
  atomic_store_explicit(&executor->ready, 1, memory_order_release);
  return 0;
}

static turbo_script_ctx_t *timer_test_context(coro_context_t **coro_out) {
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

static void timer_test_destroy(turbo_script_ctx_t *ctx, coro_context_t *coro_ctx) {
  turbo_script_free(ctx);
  coro_context_destroy(coro_ctx);
}

static int timer_test_run(turbo_script_ctx_t *ctx, const char *script) {
  int result = turbo_script_run(ctx, script);
  if (result < 0)
    fprintf(stderr, "timer test script failed: %s (code=%d active=%zu)\n",
            turbo_script_get_error(ctx), (int)turbo_script_get_error_code(ctx),
            turbo_script_timer_active_count(ctx));
  return result;
}

spec("turbo_script_timer") {
  describe("timer executor boundary") {
    it("fails fast when no serialized executor is configured") {
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      check_not_null(ctx);
      check((turbo_script_run(ctx, "import(\"timer\");"
                                         "job = timer.after(1, () => 1);")) < (0));
      check((turbo_script_get_error_code(ctx)) == (TURBO_SCRIPT_ERROR_STATE));
      check((turbo_script_timer_active_count(ctx)) == (0));
      turbo_script_free(ctx);
    }

    it("executes after callbacks on the CoroNet owner thread") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      check_not_null(ctx);

      check((timer_test_run(ctx, "import(\"timer\");"
                                       "job = timer.after(5, () => { timer.status(1); });")) == (0));
      check((turbo_script_timer_active_count(ctx)) == (1));
      check((coro_context_run(coro_ctx, TURBO_RUN_DEFAULT)) == (0));
      check((turbo_script_timer_active_count(ctx)) == (0));
      check((turbo_script_timer_failed_count(ctx)) == (0));

      check((timer_test_run(ctx, "state = timer.status(job);")) == (0));
      check(strcmp((ts_get_str(ctx, "state")), ("completed")) == 0);
      timer_test_destroy(ctx, coro_ctx);
    }

    it("runs yielding callbacks in one serialized managed coroutine") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      timer_coro_probe_t probe;
      check_not_null(ctx);

      probe.coro_ctx = coro_ctx;
      atomic_init(&probe.active, 0);
      atomic_init(&probe.calls, 0);
      atomic_init(&probe.max_active, 0);
      atomic_init(&probe.inside_coro, 0);
      ts_bind_func(ctx, "timer_probe", timer_coro_probe, &probe);

      check((timer_test_run(ctx, "import(\"timer\");"
                                       "first = timer.after(1, () => { timer_probe(); });"
                                       "second = timer.after(1, () => { timer_probe(); });")) == (0));
      check((coro_context_run(coro_ctx, TURBO_RUN_DEFAULT)) == (0));
      check((atomic_load_explicit(&probe.calls, memory_order_acquire)) == (2));
      check((atomic_load_explicit(&probe.inside_coro, memory_order_acquire)) == (2));
      check((atomic_load_explicit(&probe.max_active, memory_order_acquire)) == (1));
      check((turbo_script_timer_active_count(ctx)) == (0));
      check((turbo_script_timer_failed_count(ctx)) == (0));
      timer_test_destroy(ctx, coro_ctx);
    }

    it("cancels the managed task executing a timer callback") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      check_not_null(ctx);

      check((timer_test_run(ctx, "job = timer.after(1, () => {"
                                       "  task.sleep(5000); return 1;"
                                       "});"
                                       "canceller = task.spawn(() => {"
                                       "  task.sleep(20); return timer.cancel(job);"
                                       "});")) == (0));
      check((coro_context_run(coro_ctx, TURBO_RUN_DEFAULT)) == (0));
      check((turbo_script_timer_active_count(ctx)) == (0));
      check((turbo_script_task_active_count(ctx)) == (0));
      check((timer_test_run(ctx, "timer_state = timer.status(job);"
                                       "cancel_accepted = task.result(canceller);")) == (0));
      check(strcmp((ts_get_str(ctx, "timer_state")), ("cancelled")) == 0);
      check(fabs((double)(ts_get_num(ctx, "cancel_accepted")) - (double)(1.0)) <= (double)(0.001));
      timer_test_destroy(ctx, coro_ctx);
    }

    it("keeps driving the owner loop after http.get yields") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      check_not_null(ctx);

      check((timer_test_run(ctx, "import(\"net\");"
                                       "import(\"timer\");"
                                       "request = timer.after(1, () => {"
                                       "  http.get(\"http://127.0.0.1:1/\","
                                       "           {timeout: 100});"
                                       "});"
                                       "later = timer.after(25, () => 1);")) == (0));
      check((coro_context_run(coro_ctx, TURBO_RUN_DEFAULT)) == (0));
      check((turbo_script_timer_active_count(ctx)) == (0));
      check((turbo_script_timer_failed_count(ctx)) == (0));
      check((timer_test_run(ctx, "request_state = timer.status(request);"
                                       "later_state = timer.status(later);")) == (0));
      check(strcmp((ts_get_str(ctx, "request_state")), ("completed")) == 0);
      check(strcmp((ts_get_str(ctx, "later_state")), ("completed")) == 0);
      timer_test_destroy(ctx, coro_ctx);
    }

    it("stops an interval after an uncaught callback error") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      check_not_null(ctx);

      check((timer_test_run(ctx, "job = timer.every(5, () => { throw \"stop\"; });")) == (0));
      check((coro_context_run(coro_ctx, TURBO_RUN_DEFAULT)) == (0));
      check((turbo_script_timer_active_count(ctx)) == (0));
      check((turbo_script_timer_failed_count(ctx)) == (1));
      check((timer_test_run(ctx, "state = timer.status(job);")) == (0));
      check(strcmp((ts_get_str(ctx, "state")), ("failed")) == 0);
      check((timer_test_run(ctx, "callback_error = timer.error(job);")) == (0));
      check(strcmp((ts_get_str(ctx, "callback_error")), ("stop")) == 0);
      timer_test_destroy(ctx, coro_ctx);
    }

    it("validates cron expressions and keeps cancelled status queryable") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      check_not_null(ctx);

      check((turbo_script_run(ctx, "job = timer.cron(\"invalid cron\", () => 1);")) < (0));
      check((turbo_script_get_error_code(ctx)) == (TURBO_SCRIPT_ERROR_ARGUMENT));

      check((timer_test_run(ctx, "job = timer.cron(\"* * * * *\", () => 1);"
                                       "cancelled = timer.cancel(job);"
                                       "state = timer.status(job);")) == (0));
      check(fabs((double)(ts_get_num(ctx, "cancelled")) - (double)(1.0)) <= (double)(0.001));
      check(strcmp((ts_get_str(ctx, "state")), ("cancelled")) == 0);
      check((turbo_script_timer_active_count(ctx)) == (0));
      timer_test_destroy(ctx, coro_ctx);
    }

    it("enforces the configured bounded registry capacity") {
      coro_context_t *coro_ctx = NULL;
      turbo_script_ctx_t *ctx = timer_test_context(&coro_ctx);
      check_not_null(ctx);
      check((turbo_script_set_timer_capacity(ctx, 1)) == (0));
      check((turbo_script_run(ctx, "first = timer.after(1000, () => 1);"
                                         "second = timer.after(1000, () => 2);")) < (0));
      check((turbo_script_get_error_code(ctx)) == (TURBO_SCRIPT_ERROR_STATE));
      check((turbo_script_timer_active_count(ctx)) == (1));
      timer_test_destroy(ctx, coro_ctx);
    }

    it("keeps queued callback state alive through context shutdown") {
      queued_executor_t queued = {0};
      turbo_script_executor_t executor = {queued_executor_post, &queued};
      turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
      int attempts = 0;
      atomic_init(&queued.ready, 0);
      check_not_null(ctx);
      check((turbo_script_set_executor(ctx, &executor)) == (0));
      check((timer_test_run(ctx, "job = timer.after(1, () => 1);")) == (0));

      while (!atomic_load_explicit(&queued.ready, memory_order_acquire) && attempts < 100) {
        turbo_sleep_ms(1);
        attempts++;
      }
      check((atomic_load_explicit(&queued.ready, memory_order_acquire)) == (1));
      check_not_null(queued.task);

      turbo_script_free(ctx);
      queued.task(queued.arg1, queued.arg2);
    }
  }
}
