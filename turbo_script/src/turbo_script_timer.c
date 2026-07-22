#include "turbo_script_timer.h"
#include "turbo_script_task.h"

#include "exprtk.h"
#include "turbo_coro_context.h"
#include "turbo_cron.h"
#include "turbo_thread.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TS_TIMER_MAX_CAPACITY = 65536, TS_TIMER_ERROR_SIZE = 256 };

#define TS_TIMER_MAX_DELAY_MS ((uint64_t)UINT_MAX - 1U)

typedef enum {
  TS_TIMER_JOB_EMPTY = 0,
  TS_TIMER_JOB_RESERVED,
  TS_TIMER_JOB_SCHEDULED,
  TS_TIMER_JOB_PENDING,
  TS_TIMER_JOB_RUNNING,
  TS_TIMER_JOB_COMPLETED,
  TS_TIMER_JOB_CANCELLED,
  TS_TIMER_JOB_FAILED
} ts_timer_job_state_t;

typedef enum { TS_TIMER_KIND_AFTER = 0, TS_TIMER_KIND_EVERY, TS_TIMER_KIND_CRON } ts_timer_kind_t;

typedef struct ts_timer_job_s {
  struct ts_timer_scheduler_s *scheduler;
  int64_t id;
  ts_timer_kind_t kind;
  ts_timer_job_state_t state;
  int active_counted;
  turbo_timer_t *timer;
  uint64_t interval_ms;
  turbo_cron_expr_t cron;
  time_t cron_due;
  exprtk_value_t callback;
  int64_t task_id;
  struct ts_timer_job_s *executor_next;
  char error[TS_TIMER_ERROR_SIZE];
} ts_timer_job_t;

struct ts_timer_scheduler_s {
  turbo_script_ctx_t *ctx;
  turbo_mutex_t mutex;
  ts_timer_job_t *jobs;
  size_t capacity;
  size_t active_count;
  int64_t next_id;
  int closing;
  ts_timer_job_t *executor_head;
  ts_timer_job_t *executor_tail;
  int executor_worker_running;
};

static void ts_timer_execute_posted(void *arg1, void *arg2);
static void ts_coro_executor_dispatch_posted(void *arg1, void *arg2);
static void ts_timer_dispatch_next(turbo_script_ctx_t *ctx);

static void ts_timer_copy_error(char *dst, size_t dst_size, const char *message) {
  if (!dst || dst_size == 0) return;
  if (!message) message = "";
  snprintf(dst, dst_size, "%s", message);
}

static void ts_timer_capture_callback_error(exprtk_env_t *env, char *dst, size_t dst_size) {
  size_t length;
  if (!env || !dst || dst_size == 0) return;
  if (env->error_msg[0]) {
    ts_timer_copy_error(dst, dst_size, env->error_msg);
    return;
  }
  if (env->flow == exprtk_FLOW_THROW && env->error_value.type == EXPRTK_VAL_STRING &&
      env->error_value.data.string.data) {
    length = env->error_value.data.string.len;
    if (length >= dst_size) length = dst_size - 1;
    memcpy(dst, env->error_value.data.string.data, length);
    dst[length] = '\0';
    return;
  }
  ts_timer_copy_error(dst, dst_size, "timer callback failed");
}

static void ts_timer_set_context_error(turbo_script_ctx_t *ctx, turbo_script_error_code_t code,
                                       const char *message, int abort_env) {
  if (!ctx) return;
  ctx->error_code = code;
  ts_timer_copy_error(ctx->error_msg, sizeof(ctx->error_msg), message);
  if (abort_env) {
    ctx->env.aborted = 1;
    ts_timer_copy_error(ctx->env.error_msg, sizeof(ctx->env.error_msg), message);
  }
}

static exprtk_value_t ts_timer_fail(turbo_script_ctx_t *ctx, turbo_script_error_code_t code,
                                    const char *message) {
  ts_timer_set_context_error(ctx, code, message, 1);
  return exprtk_val_num(0.0);
}

static int ts_timer_state_is_terminal(ts_timer_job_state_t state) {
  return state == TS_TIMER_JOB_EMPTY || state == TS_TIMER_JOB_COMPLETED ||
         state == TS_TIMER_JOB_CANCELLED || state == TS_TIMER_JOB_FAILED;
}

static const char *ts_timer_state_name(ts_timer_job_state_t state) {
  switch (state) {
  case TS_TIMER_JOB_RESERVED:
    return "reserved";
  case TS_TIMER_JOB_SCHEDULED:
    return "scheduled";
  case TS_TIMER_JOB_PENDING:
    return "pending";
  case TS_TIMER_JOB_RUNNING:
    return "running";
  case TS_TIMER_JOB_COMPLETED:
    return "completed";
  case TS_TIMER_JOB_CANCELLED:
    return "cancelled";
  case TS_TIMER_JOB_FAILED:
    return "failed";
  case TS_TIMER_JOB_EMPTY:
  default:
    return "invalid";
  }
}

static void ts_timer_set_coro_persistent(turbo_script_ctx_t *ctx, int active) {
  if (ctx && ctx->coro_ctx) coro_context_set_persistent(ctx->coro_ctx, active ? 1 : 0);
}

static void ts_timer_deactivate_locked(ts_timer_scheduler_t *scheduler, ts_timer_job_t *job) {
  if (!job->active_counted) return;
  job->active_counted = 0;
  if (scheduler->active_count > 0) scheduler->active_count--;
}

static ts_timer_job_t *ts_timer_find_locked(ts_timer_scheduler_t *scheduler, int64_t id) {
  size_t i;
  if (id <= 0) return NULL;
  for (i = 0; i < scheduler->capacity; ++i) {
    if (scheduler->jobs[i].id == id && scheduler->jobs[i].state != TS_TIMER_JOB_EMPTY)
      return &scheduler->jobs[i];
  }
  return NULL;
}

static uint64_t ts_timer_cron_delay_ms(time_t due) {
  time_t now = time(NULL);
  uint64_t seconds;
  if (due <= now) return 0;
  seconds = (uint64_t)(due - now);
  if (seconds > TS_TIMER_MAX_DELAY_MS / 1000U) return TS_TIMER_MAX_DELAY_MS;
  return seconds * 1000U;
}

static int ts_timer_to_uint64(exprtk_value_t value, uint64_t *out) {
  if (!out) return -1;
  if (value.type == EXPRTK_VAL_INTEGER) {
    if (value.data.integer < 0) return -1;
    *out = (uint64_t)value.data.integer;
    return 0;
  }
  if (value.type == EXPRTK_VAL_NUMBER && isfinite(value.data.number) && value.data.number >= 0.0 &&
      floor(value.data.number) == value.data.number && value.data.number <= (double)UINT64_MAX) {
    *out = (uint64_t)value.data.number;
    return 0;
  }
  return -1;
}

static int ts_timer_to_id(exprtk_value_t value, int64_t *out) {
  uint64_t raw;
  if (ts_timer_to_uint64(value, &raw) != 0 || raw == 0 || raw > INT64_MAX) return -1;
  *out = (int64_t)raw;
  return 0;
}

static void ts_timer_native_fired(turbo_timer_t *timer) {
  ts_timer_job_t *job = (ts_timer_job_t *)turbo_timer_get_data(timer);
  ts_timer_scheduler_t *scheduler;
  turbo_script_ctx_t *ctx;
  turbo_script_executor_t executor;
  int became_idle = 0;

  if (!job || !job->scheduler) return;
  scheduler = job->scheduler;
  ctx = scheduler->ctx;

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire) ||
      job->state != TS_TIMER_JOB_SCHEDULED) {
    turbo_mutex_unlock(&scheduler->mutex);
    return;
  }

  executor = ctx->executor;
  if (!executor.post) {
    job->state = TS_TIMER_JOB_FAILED;
    ts_timer_copy_error(job->error, sizeof(job->error), "timer executor is not configured");
    ts_timer_deactivate_locked(scheduler, job);
    became_idle = scheduler->active_count == 0;
    turbo_mutex_unlock(&scheduler->mutex);
    if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
    return;
  }

  job->state = TS_TIMER_JOB_PENDING;
  ts_context_retain(ctx);
  turbo_mutex_unlock(&scheduler->mutex);

  if (executor.post(executor.data, ts_timer_execute_posted, ctx, job) != 0) {
    turbo_mutex_lock(&scheduler->mutex);
    if (job->state == TS_TIMER_JOB_PENDING) {
      job->state = TS_TIMER_JOB_FAILED;
      ts_timer_copy_error(job->error, sizeof(job->error), "timer executor rejected callback");
      ts_timer_deactivate_locked(scheduler, job);
      became_idle = scheduler->active_count == 0;
    }
    turbo_mutex_unlock(&scheduler->mutex);
    if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
    ts_context_release(ctx);
  }
}

static int ts_timer_start_job(turbo_script_ctx_t *ctx, ts_timer_kind_t kind, uint64_t interval_ms,
                              const turbo_cron_expr_t *cron, time_t cron_due,
                              exprtk_value_t callback, int64_t *id_out) {
  ts_timer_scheduler_t *scheduler = ctx->timer_scheduler;
  ts_timer_job_t *job = NULL;
  turbo_timer_t *new_timer = NULL;
  turbo_timer_t *old_timer = NULL;
  size_t i;
  int became_active = 0;

  if (!scheduler || !ctx->executor.post) {
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE,
                               "timer: configure a serialized executor first", 1);
    return -1;
  }

  /* ExprTk closure snapshots own captured variables but intentionally omit the
   * native function list. Reattach the snapshot root to this context so a
   * delayed callback can still resolve module calls such as http.get(). */
  if (callback.data.function.closure_env) {
    exprtk_env_t *closure_root = callback.data.function.closure_env;
    while (closure_root->parent && closure_root->parent != &ctx->env)
      closure_root = closure_root->parent;
    if (closure_root != &ctx->env && !closure_root->parent) closure_root->parent = &ctx->env;
  }

  new_timer = turbo_timer_create(NULL);
  if (!new_timer) {
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_OOM, "timer: native timer allocation failed",
                               1);
    return -1;
  }

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire)) {
    turbo_mutex_unlock(&scheduler->mutex);
    turbo_timer_destroy(new_timer);
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE, "timer: context is closing", 1);
    return -1;
  }
  for (i = 0; i < scheduler->capacity; ++i) {
    if (ts_timer_state_is_terminal(scheduler->jobs[i].state)) {
      job = &scheduler->jobs[i];
      job->state = TS_TIMER_JOB_RESERVED;
      old_timer = job->timer;
      job->timer = NULL;
      break;
    }
  }
  turbo_mutex_unlock(&scheduler->mutex);

  if (!job) {
    turbo_timer_destroy(new_timer);
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE, "timer: job capacity exhausted", 1);
    return -1;
  }
  if (old_timer) turbo_timer_destroy(old_timer);

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire)) {
    job->state = TS_TIMER_JOB_CANCELLED;
    turbo_mutex_unlock(&scheduler->mutex);
    turbo_timer_destroy(new_timer);
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE, "timer: context is closing", 1);
    return -1;
  }

  memset(job->error, 0, sizeof(job->error));
  scheduler->next_id++;
  if (scheduler->next_id <= 0) scheduler->next_id = 1;
  job->scheduler = scheduler;
  job->id = scheduler->next_id;
  job->kind = kind;
  job->timer = new_timer;
  job->interval_ms = interval_ms;
  job->cron_due = cron_due;
  job->callback = callback;
  job->task_id = 0;
  job->active_counted = 1;
  if (cron) job->cron = *cron;
  turbo_timer_set_data(new_timer, job);
  job->state = TS_TIMER_JOB_SCHEDULED;
  scheduler->active_count++;
  became_active = scheduler->active_count == 1;

  {
    uint64_t delay = kind == TS_TIMER_KIND_CRON ? ts_timer_cron_delay_ms(cron_due) : interval_ms;
    if (turbo_timer_start(new_timer, ts_timer_native_fired, delay, 0) != 0) {
      job->state = TS_TIMER_JOB_FAILED;
      ts_timer_copy_error(job->error, sizeof(job->error), "timer: native timer start failed");
      ts_timer_deactivate_locked(scheduler, job);
      turbo_mutex_unlock(&scheduler->mutex);
      ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE, "timer: native timer start failed",
                                 1);
      return -1;
    }
  }
  *id_out = job->id;
  turbo_mutex_unlock(&scheduler->mutex);

  if (became_active) ts_timer_set_coro_persistent(ctx, 1);
  return 0;
}

static void ts_timer_finish_callback(turbo_script_ctx_t *ctx, ts_timer_job_t *job,
                                     int cancelled, const char *callback_error) {
  ts_timer_scheduler_t *scheduler = job->scheduler;
  int callback_failed = callback_error && callback_error[0] != '\0';
  int became_idle;

  turbo_mutex_lock(&scheduler->mutex);
  job->task_id = 0;
  if (job->state == TS_TIMER_JOB_CANCELLED || scheduler->closing ||
      atomic_load_explicit(&ctx->closing, memory_order_acquire)) {
    ts_timer_deactivate_locked(scheduler, job);
  } else if (cancelled) {
    job->state = TS_TIMER_JOB_CANCELLED;
    ts_timer_copy_error(job->error, sizeof(job->error),
                        callback_failed ? callback_error : "timer callback was cancelled");
    ts_timer_deactivate_locked(scheduler, job);
  } else if (callback_failed) {
    job->state = TS_TIMER_JOB_FAILED;
    ts_timer_copy_error(job->error, sizeof(job->error), callback_error);
    ts_timer_deactivate_locked(scheduler, job);
  } else if (job->kind == TS_TIMER_KIND_AFTER) {
    job->state = TS_TIMER_JOB_COMPLETED;
    ts_timer_deactivate_locked(scheduler, job);
  } else {
    uint64_t delay = job->interval_ms;
    int next_ok = 1;
    if (job->kind == TS_TIMER_KIND_CRON) {
      if (turbo_cron_next(&job->cron, time(NULL), &job->cron_due) != TURBO_CRON_OK) {
        next_ok = 0;
      } else {
        delay = ts_timer_cron_delay_ms(job->cron_due);
      }
    }
    if (!next_ok || turbo_timer_start(job->timer, ts_timer_native_fired, delay, 0) != 0) {
      job->state = TS_TIMER_JOB_FAILED;
      ts_timer_copy_error(job->error, sizeof(job->error),
                          next_ok ? "timer: rearm failed" : "timer: no next cron occurrence");
      ts_timer_deactivate_locked(scheduler, job);
    } else {
      job->state = TS_TIMER_JOB_SCHEDULED;
    }
  }
  became_idle = scheduler->active_count == 0;
  turbo_mutex_unlock(&scheduler->mutex);

  if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
}

static void ts_timer_execute_posted(void *arg1, void *arg2) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)arg1;
  ts_timer_job_t *job = (ts_timer_job_t *)arg2;
  ts_timer_scheduler_t *scheduler = job ? job->scheduler : NULL;
  int callback_failed = 0;
  int became_idle = 0;
  exprtk_value_t callback_result;
  char callback_error[TS_TIMER_ERROR_SIZE] = {0};

  if (!ctx || !scheduler) {
    if (ctx) ts_context_release(ctx);
    return;
  }

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire) ||
      job->state == TS_TIMER_JOB_CANCELLED) {
    turbo_mutex_unlock(&scheduler->mutex);
    ts_context_release(ctx);
    return;
  }
  if (job->state != TS_TIMER_JOB_PENDING) {
    turbo_mutex_unlock(&scheduler->mutex);
    ts_context_release(ctx);
    return;
  }

  if (job->kind == TS_TIMER_KIND_CRON && time(NULL) < job->cron_due) {
    uint64_t delay = ts_timer_cron_delay_ms(job->cron_due);
    job->state = TS_TIMER_JOB_SCHEDULED;
    if (turbo_timer_start(job->timer, ts_timer_native_fired, delay, 0) != 0) {
      job->state = TS_TIMER_JOB_FAILED;
      ts_timer_copy_error(job->error, sizeof(job->error), "timer: cron rearm failed");
      ts_timer_deactivate_locked(scheduler, job);
      became_idle = scheduler->active_count == 0;
    }
    turbo_mutex_unlock(&scheduler->mutex);
    if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
    ts_context_release(ctx);
    return;
  }

  job->state = TS_TIMER_JOB_RUNNING;
  turbo_mutex_unlock(&scheduler->mutex);

  ctx->env.aborted = 0;
  ctx->env.flow = exprtk_FLOW_NORMAL;
  ctx->env.curr_nodes = 0;
  ctx->env.curr_loop_iterations = 0;
  ctx->env.curr_recursion = 0;
  ctx->env.error_msg[0] = '\0';
  callback_result = exprtk_call_function_value(job->callback, 0, NULL, &ctx->env);
  callback_failed = ctx->env.aborted || ctx->env.flow != exprtk_FLOW_NORMAL;
  if (callback_failed)
    ts_timer_capture_callback_error(&ctx->env, callback_error, sizeof(callback_error));
  if (exprtk_value_is_object_like(&callback_result)) exprtk_map_free(&callback_result);
  ctx->env.aborted = 0;
  ctx->env.flow = exprtk_FLOW_NORMAL;

  ts_timer_finish_callback(ctx, job, 0, callback_failed ? callback_error : NULL);
  ts_context_release(ctx);
}

static exprtk_value_t ts_timer_after_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  uint64_t delay;
  int64_t id;
  if (argc != 2 || ts_timer_to_uint64(args[0], &delay) != 0 || delay > TS_TIMER_MAX_DELAY_MS ||
      args[1].type != EXPRTK_VAL_FUNCTION) {
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                         "timer.after: expected (integer milliseconds, function)");
  }
  if (ts_timer_start_job(ctx, TS_TIMER_KIND_AFTER, delay, NULL, 0, args[1], &id) != 0)
    return exprtk_val_num(0.0);
  return exprtk_val_int(id);
}

static exprtk_value_t ts_timer_every_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  uint64_t interval;
  int64_t id;
  if (argc != 2 || ts_timer_to_uint64(args[0], &interval) != 0 || interval == 0 ||
      interval > TS_TIMER_MAX_DELAY_MS || args[1].type != EXPRTK_VAL_FUNCTION) {
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                         "timer.every: expected (positive integer milliseconds, function)");
  }
  if (ts_timer_start_job(ctx, TS_TIMER_KIND_EVERY, interval, NULL, 0, args[1], &id) != 0)
    return exprtk_val_num(0.0);
  return exprtk_val_int(id);
}

static exprtk_value_t ts_timer_cron_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  turbo_cron_expr_t cron;
  time_t next;
  int64_t id;
  char parse_error[TS_TIMER_ERROR_SIZE];
  char *expression;
  if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_FUNCTION) {
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                         "timer.cron: expected (cron expression, function)");
  }
  expression = tstr_v_to_arena(args[0].data.string, &ctx->scratch_arena);
  if (!expression) return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_OOM, "timer.cron: out of memory");
  if (turbo_cron_parse_ex(expression, &cron, parse_error, sizeof(parse_error)) != TURBO_CRON_OK) {
    char message[TS_TIMER_ERROR_SIZE];
    snprintf(message, sizeof(message), "timer.cron: %.220s", parse_error);
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, message);
  }
  if (turbo_cron_next(&cron, time(NULL), &next) != TURBO_CRON_OK)
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "timer.cron: no next occurrence");
  if (ts_timer_start_job(ctx, TS_TIMER_KIND_CRON, 0, &cron, next, args[1], &id) != 0)
    return exprtk_val_num(0.0);
  return exprtk_val_int(id);
}

static exprtk_value_t ts_timer_cancel_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_timer_scheduler_t *scheduler = ctx->timer_scheduler;
  ts_timer_job_t *job;
  turbo_timer_t *timer = NULL;
  int64_t id;
  int64_t task_id = 0;
  int became_idle = 0;
  if (argc != 1 || ts_timer_to_id(args[0], &id) != 0)
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                         "timer.cancel: expected a positive timer id");

  turbo_mutex_lock(&scheduler->mutex);
  job = ts_timer_find_locked(scheduler, id);
  if (!job || ts_timer_state_is_terminal(job->state)) {
    turbo_mutex_unlock(&scheduler->mutex);
    return exprtk_val_bool(0);
  }
  job->state = TS_TIMER_JOB_CANCELLED;
  timer = job->timer;
  task_id = job->task_id;
  ts_timer_deactivate_locked(scheduler, job);
  became_idle = scheduler->active_count == 0;
  turbo_mutex_unlock(&scheduler->mutex);

  if (timer) (void)turbo_timer_stop(timer);
  if (task_id > 0) (void)ts_task_cancel_managed(ctx, task_id);
  if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
  return exprtk_val_bool(1);
}

static exprtk_value_t ts_timer_status_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_timer_scheduler_t *scheduler = ctx->timer_scheduler;
  ts_timer_job_t *job;
  const char *name = "invalid";
  int64_t id;
  if (argc != 1 || ts_timer_to_id(args[0], &id) != 0)
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                         "timer.status: expected a positive timer id");
  turbo_mutex_lock(&scheduler->mutex);
  job = ts_timer_find_locked(scheduler, id);
  if (job) name = ts_timer_state_name(job->state);
  turbo_mutex_unlock(&scheduler->mutex);
  return exprtk_val_str(tstr_v_from_cstr(name));
}

static exprtk_value_t ts_timer_error_fn(size_t argc, exprtk_value_t *args, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_timer_scheduler_t *scheduler = ctx->timer_scheduler;
  ts_timer_job_t *job;
  char error[TS_TIMER_ERROR_SIZE] = {0};
  int64_t id;
  exprtk_value_t value;
  if (argc != 1 || ts_timer_to_id(args[0], &id) != 0)
    return ts_timer_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                         "timer.error: expected a positive timer id");
  turbo_mutex_lock(&scheduler->mutex);
  job = ts_timer_find_locked(scheduler, id);
  if (job) ts_timer_copy_error(error, sizeof(error), job->error);
  turbo_mutex_unlock(&scheduler->mutex);
  value = exprtk_val_str(tstr_v_from_cstr(error));
  return exprtk_value_clone_to_env(value, &ctx->env);
}

static void ts_timer_managed_completed(turbo_script_ctx_t *ctx, int cancelled,
                                       const char *error, void *arg) {
  ts_timer_job_t *job = (ts_timer_job_t *)arg;
  if (!ctx || !job || !job->scheduler) {
    if (ctx) ts_context_release(ctx);
    return;
  }
  ts_timer_finish_callback(ctx, job, cancelled, error);
  ts_timer_dispatch_next(ctx);
  /* Release the reference retained when the native timer fired. */
  ts_context_release(ctx);
}

static int ts_timer_submit_managed(turbo_script_ctx_t *ctx, ts_timer_job_t *job) {
  ts_timer_scheduler_t *scheduler = job->scheduler;
  int64_t task_id;
  int became_idle = 0;

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire) ||
      job->state == TS_TIMER_JOB_CANCELLED) {
    turbo_mutex_unlock(&scheduler->mutex);
    ts_context_release(ctx);
    return 0;
  }
  if (job->state != TS_TIMER_JOB_PENDING) {
    turbo_mutex_unlock(&scheduler->mutex);
    ts_context_release(ctx);
    return 0;
  }
  if (job->kind == TS_TIMER_KIND_CRON && time(NULL) < job->cron_due) {
    uint64_t delay = ts_timer_cron_delay_ms(job->cron_due);
    job->state = TS_TIMER_JOB_SCHEDULED;
    if (turbo_timer_start(job->timer, ts_timer_native_fired, delay, 0) != 0) {
      job->state = TS_TIMER_JOB_FAILED;
      ts_timer_copy_error(job->error, sizeof(job->error), "timer: cron rearm failed");
      ts_timer_deactivate_locked(scheduler, job);
      became_idle = scheduler->active_count == 0;
    }
    turbo_mutex_unlock(&scheduler->mutex);
    if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
    ts_context_release(ctx);
    return 0;
  }
  job->state = TS_TIMER_JOB_RUNNING;
  turbo_mutex_unlock(&scheduler->mutex);

  if (ts_task_spawn_managed(ctx, job->callback, ts_timer_managed_completed, job, &task_id) !=
      TURBO_OK) {
    turbo_mutex_lock(&scheduler->mutex);
    if (job->state == TS_TIMER_JOB_RUNNING) {
      job->state = TS_TIMER_JOB_FAILED;
      ts_timer_copy_error(job->error, sizeof(job->error),
                          "timer: managed task rejected callback");
      ts_timer_deactivate_locked(scheduler, job);
      became_idle = scheduler->active_count == 0;
    }
    turbo_mutex_unlock(&scheduler->mutex);
    ctx->env.aborted = 0;
    ctx->env.flow = exprtk_FLOW_NORMAL;
    if (became_idle) ts_timer_set_coro_persistent(ctx, 0);
    ts_context_release(ctx);
    return 0;
  }

  turbo_mutex_lock(&scheduler->mutex);
  job->task_id = task_id;
  turbo_mutex_unlock(&scheduler->mutex);
  return 1;
}

static void ts_timer_dispatch_next(turbo_script_ctx_t *ctx) {
  ts_timer_scheduler_t *scheduler = ctx ? ctx->timer_scheduler : NULL;
  if (!scheduler) return;

  for (;;) {
    ts_timer_job_t *job;
    turbo_mutex_lock(&scheduler->mutex);
    job = scheduler->executor_head;
    if (!job) {
      scheduler->executor_worker_running = 0;
      turbo_mutex_unlock(&scheduler->mutex);
      return;
    }
    scheduler->executor_head = job->executor_next;
    if (!scheduler->executor_head) scheduler->executor_tail = NULL;
    job->executor_next = NULL;
    turbo_mutex_unlock(&scheduler->mutex);

    if (ts_timer_submit_managed(ctx, job)) return;
  }
}

static void ts_coro_executor_dispatch_posted(void *arg1, void *arg2) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)arg1;
  ts_timer_job_t *job = (ts_timer_job_t *)arg2;
  ts_timer_scheduler_t *scheduler = job ? job->scheduler : NULL;
  int start_lane = 0;

  if (!ctx || !scheduler) {
    if (ctx) ts_context_release(ctx);
    return;
  }

  turbo_mutex_lock(&scheduler->mutex);
  job->executor_next = NULL;
  if (scheduler->executor_tail)
    scheduler->executor_tail->executor_next = job;
  else
    scheduler->executor_head = job;
  scheduler->executor_tail = job;
  if (!scheduler->executor_worker_running) {
    scheduler->executor_worker_running = 1;
    start_lane = 1;
  }
  turbo_mutex_unlock(&scheduler->mutex);

  if (start_lane) ts_timer_dispatch_next(ctx);
}

static int ts_coro_executor_post(void *executor_data, turbo_script_executor_task_fn task,
                                 void *arg1, void *arg2) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)arg1;
  if (!executor_data || !ctx || task != ts_timer_execute_posted ||
      executor_data != (void *)ctx->coro_ctx)
    return TURBO_EINVAL;
  return coro_post((coro_context_t *)executor_data, ts_coro_executor_dispatch_posted, arg1, arg2);
}

int ts_timer_scheduler_init(turbo_script_ctx_t *ctx, size_t capacity) {
  ts_timer_scheduler_t *scheduler;
  size_t i;
  if (!ctx || capacity == 0 || capacity > TS_TIMER_MAX_CAPACITY) return -1;
  scheduler = (ts_timer_scheduler_t *)calloc(1, sizeof(*scheduler));
  if (!scheduler) return -1;
  scheduler->jobs = (ts_timer_job_t *)calloc(capacity, sizeof(*scheduler->jobs));
  if (!scheduler->jobs) {
    free(scheduler);
    return -1;
  }
  scheduler->ctx = ctx;
  scheduler->capacity = capacity;
  turbo_mutex_init(&scheduler->mutex);
  for (i = 0; i < capacity; ++i)
    scheduler->jobs[i].scheduler = scheduler;
  ctx->timer_scheduler = scheduler;
  return 0;
}

void ts_timer_scheduler_shutdown(turbo_script_ctx_t *ctx) {
  ts_timer_scheduler_t *scheduler;
  size_t i;
  if (!ctx || !ctx->timer_scheduler) return;
  scheduler = ctx->timer_scheduler;

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing) {
    turbo_mutex_unlock(&scheduler->mutex);
    return;
  }
  scheduler->closing = 1;
  for (i = 0; i < scheduler->capacity; ++i) {
    ts_timer_job_t *job = &scheduler->jobs[i];
    if (!ts_timer_state_is_terminal(job->state)) job->state = TS_TIMER_JOB_CANCELLED;
    ts_timer_deactivate_locked(scheduler, job);
  }
  turbo_mutex_unlock(&scheduler->mutex);

  ts_timer_set_coro_persistent(ctx, 0);
  for (i = 0; i < scheduler->capacity; ++i) {
    turbo_timer_t *timer = scheduler->jobs[i].timer;
    if (timer) {
      scheduler->jobs[i].timer = NULL;
      turbo_timer_destroy(timer);
    }
  }
}

void ts_timer_scheduler_destroy(turbo_script_ctx_t *ctx) {
  ts_timer_scheduler_t *scheduler;
  if (!ctx || !ctx->timer_scheduler) return;
  scheduler = ctx->timer_scheduler;
  turbo_mutex_destroy(&scheduler->mutex);
  free(scheduler->jobs);
  free(scheduler);
  ctx->timer_scheduler = NULL;
}

void ts_timer_register_functions(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  exprtk_env_register_func(&ctx->env, "timer.after", ts_timer_after_fn, ctx);
  exprtk_env_register_func(&ctx->env, "timer.every", ts_timer_every_fn, ctx);
  exprtk_env_register_func(&ctx->env, "timer.cron", ts_timer_cron_fn, ctx);
  exprtk_env_register_func(&ctx->env, "timer.cancel", ts_timer_cancel_fn, ctx);
  exprtk_env_register_func(&ctx->env, "timer.status", ts_timer_status_fn, ctx);
  exprtk_env_register_func(&ctx->env, "timer.error", ts_timer_error_fn, ctx);
}

int turbo_script_set_executor(turbo_script_ctx_t *ctx, const turbo_script_executor_t *executor) {
  ts_timer_scheduler_t *scheduler;
  if (!ctx || !ctx->timer_scheduler) return -1;
  if (executor && !executor->post) {
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "executor post callback is NULL",
                               0);
    return -1;
  }
  if (turbo_script_task_active_count(ctx) != 0) {
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE,
                               "cannot replace CoroNet context while managed tasks are active", 0);
    return -1;
  }
  scheduler = ctx->timer_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || scheduler->active_count != 0 || scheduler->executor_worker_running) {
    turbo_mutex_unlock(&scheduler->mutex);
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE,
                               "cannot replace executor while timer jobs are active", 0);
    return -1;
  }
  if (executor) ctx->executor = *executor;
  else memset(&ctx->executor, 0, sizeof(ctx->executor));
  ctx->coro_ctx = NULL;
  turbo_mutex_unlock(&scheduler->mutex);
  return 0;
}

void turbo_script_set_coro_context(turbo_script_ctx_t *ctx, coro_context_t *coro_ctx) {
  turbo_script_executor_t executor;
  if (!ctx) return;
  if (!coro_ctx) {
    (void)turbo_script_set_executor(ctx, NULL);
    return;
  }
  executor.post = ts_coro_executor_post;
  executor.data = coro_ctx;
  if (turbo_script_set_executor(ctx, &executor) == 0) ctx->coro_ctx = coro_ctx;
}

int turbo_script_set_timer_capacity(turbo_script_ctx_t *ctx, size_t capacity) {
  ts_timer_scheduler_t *old_scheduler;
  ts_timer_scheduler_t *new_scheduler;
  size_t i;
  if (!ctx || !ctx->timer_scheduler || capacity == 0 || capacity > TS_TIMER_MAX_CAPACITY) {
    if (ctx)
      ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                                 "timer capacity must be between 1 and 65536", 0);
    return -1;
  }
  old_scheduler = ctx->timer_scheduler;
  turbo_mutex_lock(&old_scheduler->mutex);
  if (old_scheduler->closing || old_scheduler->active_count != 0 ||
      old_scheduler->executor_worker_running) {
    turbo_mutex_unlock(&old_scheduler->mutex);
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE,
                               "cannot resize timer registry while jobs are active", 0);
    return -1;
  }
  turbo_mutex_unlock(&old_scheduler->mutex);

  new_scheduler = (ts_timer_scheduler_t *)calloc(1, sizeof(*new_scheduler));
  if (!new_scheduler) {
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_OOM, "failed to allocate timer registry", 0);
    return -1;
  }
  new_scheduler->jobs = (ts_timer_job_t *)calloc(capacity, sizeof(*new_scheduler->jobs));
  if (!new_scheduler->jobs) {
    free(new_scheduler);
    ts_timer_set_context_error(ctx, TURBO_SCRIPT_ERROR_OOM, "failed to allocate timer registry", 0);
    return -1;
  }
  new_scheduler->ctx = ctx;
  new_scheduler->capacity = capacity;
  turbo_mutex_init(&new_scheduler->mutex);
  for (i = 0; i < capacity; ++i)
    new_scheduler->jobs[i].scheduler = new_scheduler;

  ts_timer_scheduler_shutdown(ctx);
  ts_timer_scheduler_destroy(ctx);
  ctx->timer_scheduler = new_scheduler;
  return 0;
}

size_t turbo_script_timer_active_count(turbo_script_ctx_t *ctx) {
  ts_timer_scheduler_t *scheduler;
  size_t count;
  if (!ctx || !ctx->timer_scheduler) return 0;
  scheduler = ctx->timer_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  count = scheduler->active_count;
  turbo_mutex_unlock(&scheduler->mutex);
  return count;
}

size_t turbo_script_timer_failed_count(turbo_script_ctx_t *ctx) {
  ts_timer_scheduler_t *scheduler;
  size_t count = 0;
  size_t i;
  if (!ctx || !ctx->timer_scheduler) return 0;
  scheduler = ctx->timer_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  for (i = 0; i < scheduler->capacity; ++i) {
    if (scheduler->jobs[i].state == TS_TIMER_JOB_FAILED) count++;
  }
  turbo_mutex_unlock(&scheduler->mutex);
  return count;
}
