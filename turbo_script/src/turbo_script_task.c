#include "turbo_script_task.h"
#include "turbo_script_timer.h"

#include "exprtk.h"
#include "CoroNet/turbo_coro_cancel.h"
#include "turbo_coro.h"
#include "turbo_coro_context.h"
#include "turbo_thread.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TS_TASK_MAX_CAPACITY = 65536,
  TS_TASK_ERROR_SIZE = 256
};

#define TS_TASK_MAGIC UINT64_C(0x54535441534B4A4F)
#define TS_TASK_MAX_SLEEP_MS ((uint64_t)UINT_MAX - 1U)

typedef enum {
  TS_TASK_EMPTY = 0,
  TS_TASK_RESERVED,
  TS_TASK_SCHEDULED,
  TS_TASK_RUNNING,
  TS_TASK_WAITING,
  TS_TASK_CANCELLING,
  TS_TASK_COMPLETED,
  TS_TASK_FAILED,
  TS_TASK_CANCELLED
} ts_task_state_t;

typedef struct ts_task_job_s {
  uint64_t magic;
  struct ts_task_scheduler_s *scheduler;
  int64_t id;
  ts_task_state_t state;
  int active_counted;
  size_t waiters;
  coro_t *co;
  struct ts_task_job_s *waiter_head;
  struct ts_task_job_s *waiter_next;
  struct ts_task_job_s *waiting_on;
  coro_cancel_source_t *cancel_source;
  coro_cancel_registration_t *lifetime_registration;
  int cancel_ref_held;
  exprtk_value_t callback;
  exprtk_value_t result;
  exprtk_env_t env;
  int env_initialized;
  int native_failed;
  int auto_release;
  ts_task_completion_fn completion;
  void *completion_arg;
  char error[TS_TASK_ERROR_SIZE];
} ts_task_job_t;

struct ts_task_scheduler_s {
  turbo_script_ctx_t *ctx;
  turbo_mutex_t mutex;
  ts_task_job_t *jobs;
  size_t capacity;
  size_t active_count;
  int64_t next_id;
  int closing;
};

static void ts_task_entry(coro_t *co, void *arg);
static int ts_task_reset_job(ts_task_job_t *job);

static void ts_task_copy_error(char *dst, size_t dst_size, const char *message) {
  if (!dst || dst_size == 0) return;
  snprintf(dst, dst_size, "%s", message ? message : "");
}

static int ts_task_state_is_terminal(ts_task_state_t state) {
  return state == TS_TASK_COMPLETED || state == TS_TASK_FAILED || state == TS_TASK_CANCELLED;
}

static const char *ts_task_state_name(ts_task_state_t state) {
  switch (state) {
  case TS_TASK_RESERVED:
    return "reserved";
  case TS_TASK_SCHEDULED:
    return "scheduled";
  case TS_TASK_RUNNING:
    return "running";
  case TS_TASK_WAITING:
    return "waiting";
  case TS_TASK_CANCELLING:
    return "cancelling";
  case TS_TASK_COMPLETED:
    return "completed";
  case TS_TASK_FAILED:
    return "failed";
  case TS_TASK_CANCELLED:
    return "cancelled";
  case TS_TASK_EMPTY:
  default:
    return "invalid";
  }
}

static ts_task_job_t *ts_task_current(turbo_script_ctx_t *ctx) {
  coro_t *co;
  ts_task_job_t *job;
  if (!ctx || !ctx->task_scheduler) return NULL;
  co = coro_running();
  if (!co) return NULL;
  job = (ts_task_job_t *)coro_get_data(co);
  if (!job || job->magic != TS_TASK_MAGIC || job->scheduler != ctx->task_scheduler) return NULL;
  return job;
}

const coro_cancel_token_t *turbo_script_current_task_cancel_token(void) {
  coro_t *co = coro_running();
  ts_task_job_t *job = co ? (ts_task_job_t *)coro_get_data(co) : NULL;
  if (!job || job->magic != TS_TASK_MAGIC || !job->cancel_source) return NULL;
  return coro_cancel_source_token(job->cancel_source);
}

exprtk_env_t *ts_task_execution_env(turbo_script_ctx_t *ctx) {
  ts_task_job_t *job = ts_task_current(ctx);
  return job && job->env_initialized ? &job->env : &ctx->env;
}

const coro_cancel_token_t *ts_task_cancel_token(turbo_script_ctx_t *ctx) {
  ts_task_job_t *job = ts_task_current(ctx);
  return job ? turbo_script_current_task_cancel_token() : NULL;
}

static void ts_task_set_context_error(turbo_script_ctx_t *ctx, turbo_script_error_code_t code,
                                      const char *message) {
  if (!ctx) return;
  ctx->error_code = code;
  ts_task_copy_error(ctx->error_msg, sizeof(ctx->error_msg), message);
}

static exprtk_value_t ts_task_fail(turbo_script_ctx_t *ctx, turbo_script_error_code_t code,
                                   const char *message) {
  exprtk_env_t *env;
  ts_task_job_t *job;
  if (!ctx) return exprtk_val_num(0.0);
  job = ts_task_current(ctx);
  env = job && job->env_initialized ? &job->env : &ctx->env;
  if (job) {
    job->native_failed = 1;
    ts_task_copy_error(job->error, sizeof(job->error), message);
  } else {
    ts_task_set_context_error(ctx, code, message);
  }
  env->aborted = 1;
  ts_task_copy_error(env->error_msg, sizeof(env->error_msg), message);
  return exprtk_val_num(0.0);
}

static int ts_task_to_uint64(exprtk_value_t value, uint64_t *out) {
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

static int ts_task_to_id(exprtk_value_t value, int64_t *out) {
  uint64_t raw;
  if (ts_task_to_uint64(value, &raw) != 0 || raw == 0 || raw > INT64_MAX) return -1;
  *out = (int64_t)raw;
  return 0;
}

static ts_task_job_t *ts_task_find_locked(ts_task_scheduler_t *scheduler, int64_t id) {
  size_t i;
  if (!scheduler || id <= 0) return NULL;
  for (i = 0; i < scheduler->capacity; ++i) {
    ts_task_job_t *job = &scheduler->jobs[i];
    if (job->id == id && job->state != TS_TASK_EMPTY) return job;
  }
  return NULL;
}

static void ts_task_deactivate_locked(ts_task_scheduler_t *scheduler, ts_task_job_t *job) {
  if (!job->active_counted) return;
  job->active_counted = 0;
  if (scheduler->active_count > 0) scheduler->active_count--;
}

static void ts_task_wake_waiters_locked(ts_task_job_t *target) {
  ts_task_job_t *waiter = target->waiter_head;
  target->waiter_head = NULL;
  while (waiter) {
    ts_task_job_t *next = waiter->waiter_next;
    waiter->waiter_next = NULL;
    waiter->waiting_on = NULL;
    if (!ts_task_state_is_terminal(waiter->state) && waiter->state != TS_TASK_CANCELLING)
      waiter->state = TS_TASK_SCHEDULED;
    if (waiter->co) coro_set_waiting_for_io(waiter->co, 0);
    waiter = next;
  }
}

static void ts_task_remove_waiter_locked(ts_task_job_t *target, ts_task_job_t *waiter) {
  ts_task_job_t **link = &target->waiter_head;
  while (*link) {
    if (*link == waiter) {
      *link = waiter->waiter_next;
      waiter->waiter_next = NULL;
      waiter->waiting_on = NULL;
      if (target->waiters > 0) target->waiters--;
      return;
    }
    link = &(*link)->waiter_next;
  }
}

static int ts_task_reset_job(ts_task_job_t *job) {
  ts_task_scheduler_t *scheduler;
  int rc;
  if (!job) return TURBO_EINVAL;
  scheduler = job->scheduler;
  if (job->cancel_source) {
    rc = coro_cancel_source_destroy(job->cancel_source);
    if (rc != TURBO_OK) return rc;
    job->cancel_source = NULL;
  }
  if (job->env_initialized) {
    if (exprtk_value_is_object_like(&job->result)) exprtk_map_free(&job->result);
    exprtk_env_free(&job->env);
  }
  memset(job, 0, sizeof(*job));
  job->magic = TS_TASK_MAGIC;
  job->scheduler = scheduler;
  return TURBO_OK;
}

static void ts_task_cancel_lifetime_released(void *arg) {
  ts_task_job_t *job = (ts_task_job_t *)arg;
  ts_task_scheduler_t *scheduler;
  turbo_script_ctx_t *ctx;
  coro_cancel_registration_t *registration;
  int release_ref = 0;
  int reset_terminal = 0;
  if (!job || !job->scheduler) return;
  scheduler = job->scheduler;
  ctx = scheduler->ctx;

  turbo_mutex_lock(&scheduler->mutex);
  registration = job->lifetime_registration;
  job->lifetime_registration = NULL;
  if (job->cancel_ref_held) {
    job->cancel_ref_held = 0;
    release_ref = 1;
  }
  if (job->auto_release && ts_task_state_is_terminal(job->state) && !job->co)
    reset_terminal = 1;
  turbo_mutex_unlock(&scheduler->mutex);

  if (registration) (void)coro_cancel_unregister(registration);
  if (reset_terminal) (void)ts_task_reset_job(job);
  if (release_ref) ts_context_release(ctx);
}

static void ts_task_release_cancel_lifetime(ts_task_job_t *job) {
  turbo_script_ctx_t *ctx;
  coro_cancel_registration_t *registration;
  if (!job || !job->scheduler || !job->cancel_ref_held) return;
  ctx = job->scheduler->ctx;
  registration = job->lifetime_registration;
  job->lifetime_registration = NULL;
  job->cancel_ref_held = 0;
  if (registration) (void)coro_cancel_unregister(registration);
  ts_context_release(ctx);
}

static void ts_task_init_env(ts_task_job_t *job, turbo_script_ctx_t *ctx) {
  exprtk_env_init_child(&job->env, &ctx->env);
  job->env.eval_node = ctx->env.eval_node;
  job->env.exec_script_body = ctx->env.exec_script_body;
  job->env.max_recursion = ctx->env.max_recursion;
  job->env.max_loop_iterations = ctx->env.max_loop_iterations;
  job->env.max_nodes = ctx->env.max_nodes;
  job->env.max_external_value_bytes = ctx->memory_policy.max_external_value_bytes;
  job->env_initialized = 1;
}

static void ts_task_attach_closure(turbo_script_ctx_t *ctx, exprtk_value_t callback) {
  exprtk_env_t *closure_root = callback.data.function.closure_env;
  if (!closure_root) return;
  while (closure_root->parent && closure_root->parent != &ctx->env)
    closure_root = closure_root->parent;
  if (closure_root != &ctx->env && !closure_root->parent) closure_root->parent = &ctx->env;
}

static void ts_task_capture_error(exprtk_env_t *env, char *dst, size_t dst_size) {
  size_t length;
  if (env->error_msg[0]) {
    ts_task_copy_error(dst, dst_size, env->error_msg);
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
  ts_task_copy_error(dst, dst_size, "task callback failed");
}

static void ts_task_set_current_state(ts_task_job_t *job, ts_task_state_t state) {
  ts_task_scheduler_t *scheduler = job->scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  if (!ts_task_state_is_terminal(job->state) && job->state != TS_TASK_CANCELLING)
    job->state = state;
  turbo_mutex_unlock(&scheduler->mutex);
}

static int ts_task_is_cancelled(ts_task_job_t *job) {
  const coro_cancel_token_t *token;
  if (!job || !job->cancel_source) return 0;
  token = coro_cancel_source_token(job->cancel_source);
  return coro_cancel_token_is_requested(token);
}

static void ts_task_mark_cancelling(ts_task_job_t *job) {
  ts_task_scheduler_t *scheduler;
  if (!job || !job->scheduler) return;
  scheduler = job->scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  if (!ts_task_state_is_terminal(job->state)) job->state = TS_TASK_CANCELLING;
  turbo_mutex_unlock(&scheduler->mutex);
}

static exprtk_value_t ts_task_cancel_failure(turbo_script_ctx_t *ctx, ts_task_job_t *job) {
  ts_task_mark_cancelling(job);
  return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_CANCELLED, "task was cancelled");
}

static void ts_task_interrupt_wait(void *arg) {
  coro_wait_t *wait = (coro_wait_t *)arg;
  if (wait) (void)coro_wait_interrupt(wait, TURBO_ECANCELED);
}

static void ts_task_interrupt_join(void *arg) {
  ts_task_job_t *job = (ts_task_job_t *)arg;
  ts_task_scheduler_t *scheduler;
  if (!job || !job->scheduler) return;
  scheduler = job->scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  if (!ts_task_state_is_terminal(job->state)) job->state = TS_TASK_CANCELLING;
  if (job->waiting_on) ts_task_remove_waiter_locked(job->waiting_on, job);
  if (job->co) coro_set_waiting_for_io(job->co, 0);
  turbo_mutex_unlock(&scheduler->mutex);
}

static int ts_task_spawn_job(turbo_script_ctx_t *ctx, exprtk_value_t callback,
                             ts_task_completion_fn completion, void *completion_arg,
                             int auto_release, int64_t *id_out) {
  ts_task_scheduler_t *scheduler = ctx->task_scheduler;
  ts_task_job_t *job = NULL;
  size_t i;
  int rc;

  if (!scheduler || !ctx->coro_ctx) {
    ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                 "task.spawn: configure a CoroNet context first");
    return -1;
  }

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire)) {
    turbo_mutex_unlock(&scheduler->mutex);
    ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.spawn: context is closing");
    return -1;
  }
  for (i = 0; i < scheduler->capacity; ++i) {
    if (scheduler->jobs[i].state == TS_TASK_EMPTY) {
      job = &scheduler->jobs[i];
      job->state = TS_TASK_RESERVED;
      break;
    }
  }
  turbo_mutex_unlock(&scheduler->mutex);

  if (!job) {
    ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                 "task.spawn: registry capacity exhausted; release completed tasks");
    return -1;
  }

  ts_task_attach_closure(ctx, callback);
  job->cancel_source = coro_cancel_source_create(ctx->coro_ctx);
  if (!job->cancel_source) {
    turbo_mutex_lock(&scheduler->mutex);
    job->state = TS_TASK_EMPTY;
    turbo_mutex_unlock(&scheduler->mutex);
    ts_task_reset_job(job);
    ts_task_fail(ctx, TURBO_SCRIPT_ERROR_OOM, "task.spawn: failed to allocate cancellation source");
    return -1;
  }
  ts_context_retain(ctx);
  job->cancel_ref_held = 1;
  rc = coro_cancel_register(coro_cancel_source_token(job->cancel_source),
                            ts_task_cancel_lifetime_released, job,
                            &job->lifetime_registration);
  if (rc != TURBO_OK) {
    ts_task_release_cancel_lifetime(job);
    turbo_mutex_lock(&scheduler->mutex);
    job->state = TS_TASK_EMPTY;
    turbo_mutex_unlock(&scheduler->mutex);
    (void)ts_task_reset_job(job);
    ts_task_fail(ctx, rc == TURBO_ENOMEM ? TURBO_SCRIPT_ERROR_OOM
                                        : TURBO_SCRIPT_ERROR_STATE,
                 "task.spawn: failed to register cancellation lifetime");
    return -1;
  }
  ts_task_init_env(job, ctx);

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire) ||
      job->state != TS_TASK_RESERVED) {
    job->state = TS_TASK_EMPTY;
    turbo_mutex_unlock(&scheduler->mutex);
    ts_task_release_cancel_lifetime(job);
    (void)ts_task_reset_job(job);
    ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.spawn: context is closing");
    return -1;
  }
  scheduler->next_id++;
  if (scheduler->next_id <= 0) scheduler->next_id = 1;
  job->id = scheduler->next_id;
  job->callback = callback;
  job->completion = completion;
  job->completion_arg = completion_arg;
  job->auto_release = auto_release;
  job->result = exprtk_val_num(0.0);
  job->active_counted = 1;
  job->state = TS_TASK_SCHEDULED;
  scheduler->active_count++;
  *id_out = job->id;
  ts_context_retain(ctx);
  turbo_mutex_unlock(&scheduler->mutex);

  rc = coro_context_spawn(ctx->coro_ctx, ts_task_entry, job);
  if (rc != 0) {
    turbo_mutex_lock(&scheduler->mutex);
    ts_task_deactivate_locked(scheduler, job);
    job->state = TS_TASK_EMPTY;
    turbo_mutex_unlock(&scheduler->mutex);
    ts_task_release_cancel_lifetime(job);
    (void)ts_task_reset_job(job);
    ts_context_release(ctx);
    ts_task_fail(ctx, rc == TURBO_ENOMEM ? TURBO_SCRIPT_ERROR_OOM : TURBO_SCRIPT_ERROR_STATE,
                 "task.spawn: CoroNet rejected the task");
    return -1;
  }
  return 0;
}

static void ts_task_entry(coro_t *co, void *arg) {
  ts_task_job_t *job = (ts_task_job_t *)arg;
  ts_task_scheduler_t *scheduler;
  turbo_script_ctx_t *ctx;
  ts_task_completion_fn completion;
  void *completion_arg;
  char completion_error[TS_TASK_ERROR_SIZE];
  int auto_release;
  int cancelled;
  int failed;

  if (!job || job->magic != TS_TASK_MAGIC || !job->scheduler) return;
  scheduler = job->scheduler;
  ctx = scheduler->ctx;
  job->co = co;
  coro_set_data(co, job);

  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing || atomic_load_explicit(&ctx->closing, memory_order_acquire) ||
      job->state != TS_TASK_SCHEDULED) {
    job->state = TS_TASK_CANCELLED;
    ts_task_copy_error(job->error, sizeof(job->error), "task was cancelled");
    ts_task_deactivate_locked(scheduler, job);
    ts_task_wake_waiters_locked(job);
    turbo_mutex_unlock(&scheduler->mutex);
    completion = job->completion;
    completion_arg = job->completion_arg;
    auto_release = job->auto_release;
    ts_task_copy_error(completion_error, sizeof(completion_error), job->error);
    job->co = NULL;
    coro_set_data(co, NULL);
    if (!ts_task_is_cancelled(job)) ts_task_release_cancel_lifetime(job);
    if (auto_release) (void)ts_task_reset_job(job);
    if (completion) completion(ctx, 1, completion_error, completion_arg);
    ts_context_release(ctx);
    return;
  }
  job->state = TS_TASK_RUNNING;
  turbo_mutex_unlock(&scheduler->mutex);

  job->env.aborted = 0;
  job->env.flow = exprtk_FLOW_NORMAL;
  job->env.curr_nodes = 0;
  job->env.curr_loop_iterations = 0;
  job->env.curr_recursion = 0;
  job->env.error_msg[0] = '\0';
  job->result = exprtk_call_function_value(job->callback, 0, NULL, &job->env);
  if (mem_pool_total_used(&job->env.arena) > ctx->memory_policy.max_task_bytes) {
    job->native_failed = 1;
    job->env.aborted = 1;
    ts_task_copy_error(job->error, sizeof(job->error),
                       "task result exceeded the configured memory quota");
  }
  cancelled = ts_task_is_cancelled(job);
  failed = job->native_failed || job->env.aborted || job->env.flow != exprtk_FLOW_NORMAL;
  if (cancelled)
    ts_task_copy_error(job->error, sizeof(job->error), "task was cancelled");
  else if (failed && !job->error[0])
    ts_task_capture_error(&job->env, job->error, sizeof(job->error));
  memset(&job->callback, 0, sizeof(job->callback));

  turbo_mutex_lock(&scheduler->mutex);
  job->state = cancelled ? TS_TASK_CANCELLED : (failed ? TS_TASK_FAILED : TS_TASK_COMPLETED);
  ts_task_deactivate_locked(scheduler, job);
  ts_task_wake_waiters_locked(job);
  turbo_mutex_unlock(&scheduler->mutex);
  completion = job->completion;
  completion_arg = job->completion_arg;
  auto_release = job->auto_release;
  ts_task_copy_error(completion_error, sizeof(completion_error), job->error);
  job->co = NULL;
  coro_set_data(co, NULL);
  if (!cancelled) ts_task_release_cancel_lifetime(job);
  if (auto_release) (void)ts_task_reset_job(job);
  if (completion) completion(ctx, cancelled, completion_error, completion_arg);
  ts_context_release(ctx);
}

size_t ts_task_memory_used(turbo_script_ctx_t *ctx) {
  ts_task_scheduler_t *scheduler;
  size_t total = 0;
  size_t i;
  if (!ctx || !ctx->task_scheduler) return 0;
  scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  for (i = 0; i < scheduler->capacity; ++i) {
    ts_task_job_t *job = &scheduler->jobs[i];
    size_t used;
    if (!job->env_initialized) continue;
    used = mem_pool_total_used(&job->env.arena);
    if (total > SIZE_MAX - used) {
      total = SIZE_MAX;
      break;
    }
    total += used;
  }
  turbo_mutex_unlock(&scheduler->mutex);
  return total;
}

size_t ts_task_memory_max_used(turbo_script_ctx_t *ctx) {
  ts_task_scheduler_t *scheduler;
  size_t maximum = 0;
  size_t i;
  if (!ctx || !ctx->task_scheduler) return 0;
  scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  for (i = 0; i < scheduler->capacity; ++i) {
    ts_task_job_t *job = &scheduler->jobs[i];
    size_t used;
    if (!job->env_initialized) continue;
    used = mem_pool_total_used(&job->env.arena);
    if (used > maximum) maximum = used;
  }
  turbo_mutex_unlock(&scheduler->mutex);
  return maximum;
}

static exprtk_value_t ts_task_spawn_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  int64_t id;
  if (argc != 1 || args[0].type != EXPRTK_VAL_FUNCTION)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.spawn: expected one function argument");
  if (ts_task_spawn_job(ctx, args[0], NULL, NULL, 0, &id) != 0)
    return exprtk_val_num(0.0);
  return exprtk_val_int(id);
}

static exprtk_value_t ts_task_yield_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_task_job_t *job = ts_task_current(ctx);
  (void)args;
  if (argc != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "task.yield: expected no arguments");
  if (!job)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.yield: may only be called from task.spawn callback");
  if (ts_task_is_cancelled(job)) return ts_task_cancel_failure(ctx, job);
  ts_task_set_current_state(job, TS_TASK_SCHEDULED);
  if (coro_yield() != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.yield: scheduler yield failed");
  if (ts_task_is_cancelled(job)) return ts_task_cancel_failure(ctx, job);
  ts_task_set_current_state(job, TS_TASK_RUNNING);
  return exprtk_val_num(0.0);
}

static exprtk_value_t ts_task_sleep_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_task_job_t *job = ts_task_current(ctx);
  const coro_cancel_token_t *token;
  coro_cancel_registration_t *registration = NULL;
  coro_wait_t *wait;
  uint64_t delay;
  int rc;
  if (argc != 1 || ts_task_to_uint64(args[0], &delay) != 0 || delay > TS_TASK_MAX_SLEEP_MS)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.sleep: expected milliseconds between 0 and 4294967294");
  if (!job)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.sleep: may only be called from task.spawn callback");
  token = coro_cancel_source_token(job->cancel_source);
  if (coro_cancel_token_is_requested(token)) return ts_task_cancel_failure(ctx, job);
  wait = coro_wait_create(ctx->coro_ctx);
  if (!wait)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_OOM, "task.sleep: failed to allocate wait");
  rc = coro_cancel_register(token, ts_task_interrupt_wait, wait, &registration);
  if (rc != TURBO_OK) {
    (void)coro_wait_destroy(wait);
    if (rc == TURBO_ECANCELED) return ts_task_cancel_failure(ctx, job);
    return ts_task_fail(ctx, rc == TURBO_ENOMEM ? TURBO_SCRIPT_ERROR_OOM
                                                : TURBO_SCRIPT_ERROR_STATE,
                        "task.sleep: failed to register cancellation");
  }
  ts_task_set_current_state(job, TS_TASK_WAITING);
  rc = coro_wait_for(wait, delay);
  (void)coro_cancel_unregister(registration);
  (void)coro_wait_destroy(wait);
  if (rc == TURBO_ECANCELED || coro_cancel_token_is_requested(token))
    return ts_task_cancel_failure(ctx, job);
  if (rc != TURBO_OK)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.sleep: wait failed");
  ts_task_set_current_state(job, TS_TASK_RUNNING);
  return exprtk_val_num(0.0);
}

static exprtk_value_t ts_task_status_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_task_scheduler_t *scheduler = ctx->task_scheduler;
  ts_task_job_t *job;
  const char *name = "invalid";
  int64_t id;
  if (argc != 1 || ts_task_to_id(args[0], &id) != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.status: expected a positive task id");
  turbo_mutex_lock(&scheduler->mutex);
  job = ts_task_find_locked(scheduler, id);
  if (job) name = ts_task_state_name(job->state);
  turbo_mutex_unlock(&scheduler->mutex);
  return exprtk_val_str(tstr_v_from_cstr(name));
}

static exprtk_value_t ts_task_error_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_task_scheduler_t *scheduler = ctx->task_scheduler;
  exprtk_env_t *dst = ts_task_execution_env(ctx);
  ts_task_job_t *job;
  char error[TS_TASK_ERROR_SIZE] = {0};
  int64_t id;
  if (argc != 1 || ts_task_to_id(args[0], &id) != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.error: expected a positive task id");
  turbo_mutex_lock(&scheduler->mutex);
  job = ts_task_find_locked(scheduler, id);
  if (job) ts_task_copy_error(error, sizeof(error), job->error);
  turbo_mutex_unlock(&scheduler->mutex);
  return exprtk_value_clone_to_env(exprtk_val_str(tstr_v_from_cstr(error)), dst);
}

static exprtk_value_t ts_task_result_value(turbo_script_ctx_t *ctx, int64_t id, int await) {
  ts_task_scheduler_t *scheduler = ctx->task_scheduler;
  ts_task_job_t *self = ts_task_current(ctx);
  ts_task_job_t *target;
  exprtk_value_t result;
  char error[TS_TASK_ERROR_SIZE];
  coro_cancel_registration_t *cancel_registration = NULL;
  int waiter_counted = 0;
  int rc;

  if (await && !self)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.join: may only be called from task.spawn callback");
  if (await && ts_task_is_cancelled(self)) return ts_task_cancel_failure(ctx, self);

  turbo_mutex_lock(&scheduler->mutex);
  target = ts_task_find_locked(scheduler, id);
  if (!target) {
    turbo_mutex_unlock(&scheduler->mutex);
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT, "task: unknown or released task id");
  }
  if (await && target == self) {
    turbo_mutex_unlock(&scheduler->mutex);
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.join: a task cannot join itself");
  }
  if (!ts_task_state_is_terminal(target->state)) {
    if (!await) {
      turbo_mutex_unlock(&scheduler->mutex);
      return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.result: task is not complete");
    }
    target->waiters++;
    waiter_counted = 1;
    self->waiting_on = target;
    self->waiter_next = target->waiter_head;
    target->waiter_head = self;
    self->state = TS_TASK_WAITING;
    coro_set_waiting_for_io(self->co, 1);
    turbo_mutex_unlock(&scheduler->mutex);
    rc = coro_cancel_register(coro_cancel_source_token(self->cancel_source),
                              ts_task_interrupt_join, self, &cancel_registration);
    if (rc != TURBO_OK) {
      turbo_mutex_lock(&scheduler->mutex);
      if (self->waiting_on == target) {
        ts_task_remove_waiter_locked(target, self);
        waiter_counted = 0;
      }
      coro_set_waiting_for_io(self->co, 0);
      turbo_mutex_unlock(&scheduler->mutex);
      if (rc == TURBO_ECANCELED) return ts_task_cancel_failure(ctx, self);
      return ts_task_fail(ctx, rc == TURBO_ENOMEM ? TURBO_SCRIPT_ERROR_OOM
                                                  : TURBO_SCRIPT_ERROR_STATE,
                          "task.join: failed to register cancellation");
    }
    rc = coro_yield();
    (void)coro_cancel_unregister(cancel_registration);
    if (rc != 0) {
      turbo_mutex_lock(&scheduler->mutex);
      if (self->waiting_on == target) {
        ts_task_remove_waiter_locked(target, self);
        waiter_counted = 0;
      }
      coro_set_waiting_for_io(self->co, 0);
      turbo_mutex_unlock(&scheduler->mutex);
      return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE, "task.join: scheduler yield failed");
    }
    turbo_mutex_lock(&scheduler->mutex);
    if (self->state == TS_TASK_CANCELLING || ts_task_is_cancelled(self)) {
      if (self->waiting_on == target) {
        ts_task_remove_waiter_locked(target, self);
        waiter_counted = 0;
      } else if (waiter_counted && target->waiters > 0) {
        target->waiters--;
        waiter_counted = 0;
      }
      coro_set_waiting_for_io(self->co, 0);
      turbo_mutex_unlock(&scheduler->mutex);
      return ts_task_cancel_failure(ctx, self);
    }
    self->state = TS_TASK_RUNNING;
    if (!ts_task_state_is_terminal(target->state)) {
      if (self->waiting_on == target) {
        ts_task_remove_waiter_locked(target, self);
        waiter_counted = 0;
      }
      turbo_mutex_unlock(&scheduler->mutex);
      return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                          "task.join: resumed before target completion");
    }
  }

  if (target->state != TS_TASK_COMPLETED) {
    ts_task_copy_error(error, sizeof(error), target->error[0] ? target->error : "task was cancelled");
    if (waiter_counted && target->waiters > 0) target->waiters--;
    turbo_mutex_unlock(&scheduler->mutex);
    return ts_task_fail(ctx, target->state == TS_TASK_CANCELLED
                                 ? TURBO_SCRIPT_ERROR_CANCELLED
                                 : TURBO_SCRIPT_ERROR_RUNTIME,
                        error);
  }

  result = exprtk_value_clone_to_env(target->result, ts_task_execution_env(ctx));
  if (waiter_counted && target->waiters > 0) target->waiters--;
  turbo_mutex_unlock(&scheduler->mutex);
  return result;
}

static exprtk_value_t ts_task_result_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  int64_t id;
  if (argc != 1 || ts_task_to_id(args[0], &id) != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.result: expected a positive task id");
  return ts_task_result_value(ctx, id, 0);
}

static exprtk_value_t ts_task_join_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  int64_t id;
  if (argc != 1 || ts_task_to_id(args[0], &id) != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.join: expected a positive task id");
  return ts_task_result_value(ctx, id, 1);
}

static exprtk_value_t ts_task_release_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_task_scheduler_t *scheduler = ctx->task_scheduler;
  ts_task_job_t *job;
  int64_t id;
  if (argc != 1 || ts_task_to_id(args[0], &id) != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.release: expected a positive task id");
  turbo_mutex_lock(&scheduler->mutex);
  job = ts_task_find_locked(scheduler, id);
  if (!job) {
    turbo_mutex_unlock(&scheduler->mutex);
    return exprtk_val_bool(0);
  }
  if (!ts_task_state_is_terminal(job->state) || job->waiters != 0) {
    turbo_mutex_unlock(&scheduler->mutex);
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.release: task is active or still awaited");
  }
  turbo_mutex_unlock(&scheduler->mutex);
  if (ts_task_reset_job(job) != TURBO_OK)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.release: cancellation dispatch is still active");
  return exprtk_val_bool(1);
}

static exprtk_value_t ts_task_cancel_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  ts_task_scheduler_t *scheduler = ctx->task_scheduler;
  coro_cancel_source_t *source;
  ts_task_job_t *job;
  int64_t id;
  int rc;
  if (argc != 1 || ts_task_to_id(args[0], &id) != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.cancel: expected a positive task id");

  turbo_mutex_lock(&scheduler->mutex);
  job = ts_task_find_locked(scheduler, id);
  if (!job || ts_task_state_is_terminal(job->state) || job->state == TS_TASK_CANCELLING) {
    turbo_mutex_unlock(&scheduler->mutex);
    return exprtk_val_bool(0);
  }
  source = job->cancel_source;
  if (!source) {
    turbo_mutex_unlock(&scheduler->mutex);
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.cancel: task has no cancellation source");
  }
  job->state = TS_TASK_CANCELLING;
  turbo_mutex_unlock(&scheduler->mutex);

  rc = coro_cancel_source_request(source);
  if (rc != TURBO_OK && rc != TURBO_EALREADY)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.cancel: failed to dispatch cancellation");
  return exprtk_val_bool(1);
}

int ts_task_cancel_managed(turbo_script_ctx_t *ctx, int64_t id) {
  ts_task_scheduler_t *scheduler;
  coro_cancel_source_t *source;
  ts_task_job_t *job;
  int rc;
  if (!ctx || !ctx->task_scheduler || id <= 0) return TURBO_EINVAL;
  scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  job = ts_task_find_locked(scheduler, id);
  if (!job || ts_task_state_is_terminal(job->state) || job->state == TS_TASK_CANCELLING) {
    turbo_mutex_unlock(&scheduler->mutex);
    return TURBO_EALREADY;
  }
  source = job->cancel_source;
  job->state = TS_TASK_CANCELLING;
  turbo_mutex_unlock(&scheduler->mutex);
  if (!source) return TURBO_EINVAL;
  rc = coro_cancel_source_request(source);
  return rc == TURBO_EALREADY ? TURBO_OK : rc;
}

int ts_task_spawn_managed(turbo_script_ctx_t *ctx, exprtk_value_t callback,
                          ts_task_completion_fn completion, void *completion_arg,
                          int64_t *id_out) {
  if (!ctx || !completion || !id_out) return TURBO_EINVAL;
  return ts_task_spawn_job(ctx, callback, completion, completion_arg, 1, id_out) == 0
             ? TURBO_OK
             : TURBO_EINVAL;
}

static size_t ts_task_shutdown_jobs(ts_task_scheduler_t *scheduler, int *request_error) {
  size_t cancelled = 0;
  size_t i;
  if (request_error) *request_error = TURBO_OK;

  turbo_mutex_lock(&scheduler->mutex);
  scheduler->closing = 1;
  turbo_mutex_unlock(&scheduler->mutex);

  for (i = 0; i < scheduler->capacity; ++i) {
    coro_cancel_source_t *source = NULL;
    int rc;
    turbo_mutex_lock(&scheduler->mutex);
    if (scheduler->jobs[i].state == TS_TASK_RESERVED) {
      scheduler->jobs[i].state = TS_TASK_CANCELLED;
    } else if (!ts_task_state_is_terminal(scheduler->jobs[i].state) &&
               scheduler->jobs[i].state != TS_TASK_EMPTY &&
               scheduler->jobs[i].state != TS_TASK_CANCELLING) {
      scheduler->jobs[i].state = TS_TASK_CANCELLING;
      source = scheduler->jobs[i].cancel_source;
      cancelled++;
    }
    turbo_mutex_unlock(&scheduler->mutex);

    if (!source) continue;
    rc = coro_cancel_source_request(source);
    if (rc != TURBO_OK && rc != TURBO_EALREADY && request_error && *request_error == TURBO_OK)
      *request_error = rc;
  }
  return cancelled;
}

static exprtk_value_t ts_task_shutdown_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  int request_error;
  size_t cancelled;
  (void)args;
  if (argc != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.shutdown: expected no arguments");
  ts_timer_scheduler_shutdown(ctx);
  cancelled = ts_task_shutdown_jobs(ctx->task_scheduler, &request_error);
  if (request_error != TURBO_OK)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_STATE,
                        "task.shutdown: failed to dispatch cancellation");
  return exprtk_val_int((int64_t)cancelled);
}

static exprtk_value_t ts_task_active_count_fn(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  turbo_script_ctx_t *ctx = (turbo_script_ctx_t *)user_data;
  (void)args;
  if (argc != 0)
    return ts_task_fail(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                        "task.active_count: expected no arguments");
  return exprtk_val_int((int64_t)turbo_script_task_active_count(ctx));
}

int ts_task_scheduler_init(turbo_script_ctx_t *ctx, size_t capacity) {
  ts_task_scheduler_t *scheduler;
  size_t i;
  if (!ctx || capacity == 0 || capacity > TS_TASK_MAX_CAPACITY) return -1;
  scheduler = (ts_task_scheduler_t *)calloc(1, sizeof(*scheduler));
  if (!scheduler) return -1;
  scheduler->jobs = (ts_task_job_t *)calloc(capacity, sizeof(*scheduler->jobs));
  if (!scheduler->jobs) {
    free(scheduler);
    return -1;
  }
  scheduler->ctx = ctx;
  scheduler->capacity = capacity;
  turbo_mutex_init(&scheduler->mutex);
  for (i = 0; i < capacity; ++i) {
    scheduler->jobs[i].magic = TS_TASK_MAGIC;
    scheduler->jobs[i].scheduler = scheduler;
  }
  ctx->task_scheduler = scheduler;
  return 0;
}

void ts_task_scheduler_shutdown(turbo_script_ctx_t *ctx) {
  ts_task_scheduler_t *scheduler;
  if (!ctx || !ctx->task_scheduler) return;
  scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  if (scheduler->closing) {
    turbo_mutex_unlock(&scheduler->mutex);
    return;
  }
  turbo_mutex_unlock(&scheduler->mutex);
  (void)ts_task_shutdown_jobs(scheduler, NULL);
}

void ts_task_scheduler_destroy(turbo_script_ctx_t *ctx) {
  ts_task_scheduler_t *scheduler;
  size_t i;
  if (!ctx || !ctx->task_scheduler) return;
  scheduler = ctx->task_scheduler;
  for (i = 0; i < scheduler->capacity; ++i) ts_task_reset_job(&scheduler->jobs[i]);
  turbo_mutex_destroy(&scheduler->mutex);
  free(scheduler->jobs);
  free(scheduler);
  ctx->task_scheduler = NULL;
}

void ts_task_register_functions(turbo_script_ctx_t *ctx) {
  if (!ctx) return;
  exprtk_env_register_func(&ctx->env, "task.spawn", ts_task_spawn_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.yield", ts_task_yield_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.sleep", ts_task_sleep_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.join", ts_task_join_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.status", ts_task_status_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.result", ts_task_result_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.error", ts_task_error_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.cancel", ts_task_cancel_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.shutdown", ts_task_shutdown_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.release", ts_task_release_fn, ctx);
  exprtk_env_register_func(&ctx->env, "task.active_count", ts_task_active_count_fn, ctx);
}

int turbo_script_set_task_capacity(turbo_script_ctx_t *ctx, size_t capacity) {
  ts_task_scheduler_t *old_scheduler;
  ts_task_scheduler_t *new_scheduler;
  if (!ctx || !ctx->task_scheduler || capacity == 0 || capacity > TS_TASK_MAX_CAPACITY) {
    if (ctx)
      ts_task_set_context_error(ctx, TURBO_SCRIPT_ERROR_ARGUMENT,
                                "task capacity must be between 1 and 65536");
    return -1;
  }
  old_scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&old_scheduler->mutex);
  if (old_scheduler->closing || old_scheduler->active_count != 0) {
    turbo_mutex_unlock(&old_scheduler->mutex);
    ts_task_set_context_error(ctx, TURBO_SCRIPT_ERROR_STATE,
                              "cannot resize task registry while tasks are active");
    return -1;
  }
  turbo_mutex_unlock(&old_scheduler->mutex);

  ctx->task_scheduler = NULL;
  if (ts_task_scheduler_init(ctx, capacity) != 0) {
    ctx->task_scheduler = old_scheduler;
    ts_task_set_context_error(ctx, TURBO_SCRIPT_ERROR_OOM, "failed to allocate task registry");
    return -1;
  }
  new_scheduler = ctx->task_scheduler;
  ctx->task_scheduler = old_scheduler;
  ts_task_scheduler_destroy(ctx);
  ctx->task_scheduler = new_scheduler;
  return 0;
}

size_t turbo_script_task_active_count(turbo_script_ctx_t *ctx) {
  ts_task_scheduler_t *scheduler;
  size_t count;
  if (!ctx || !ctx->task_scheduler) return 0;
  scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  count = scheduler->active_count;
  turbo_mutex_unlock(&scheduler->mutex);
  return count;
}

size_t turbo_script_task_failed_count(turbo_script_ctx_t *ctx) {
  ts_task_scheduler_t *scheduler;
  size_t count = 0;
  size_t i;
  if (!ctx || !ctx->task_scheduler) return 0;
  scheduler = ctx->task_scheduler;
  turbo_mutex_lock(&scheduler->mutex);
  for (i = 0; i < scheduler->capacity; ++i) {
    if (scheduler->jobs[i].state == TS_TASK_FAILED) count++;
  }
  turbo_mutex_unlock(&scheduler->mutex);
  return count;
}
