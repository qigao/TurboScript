#include "turbo_script_host_internal.h"

#include "../turbo_script_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
  TS_HOST_HARD_STEP_LIMIT = 100000,
  TS_HOST_HARD_LOOP_LIMIT = 10000,
  TS_HOST_HARD_CALLBACK_LIMIT = 1024,
  TS_HOST_HARD_RECURSION_LIMIT = 100,
  TS_HOST_INTERRUPT_STEP_INTERVAL = 256,
};

static uint32_t ts_host_min_u32(uint32_t lhs, uint32_t rhs) { return lhs < rhs ? lhs : rhs; }

static size_t ts_host_min_size(size_t lhs, size_t rhs) { return lhs < rhs ? lhs : rhs; }

static int ts_host_budget_fail(ts_host_call_budget_t *budget, turbo_script_status_t status,
                               turbo_script_error_phase_t phase, const char *message) {
  if (!budget) return -1;
  if (budget->failure_status == TURBO_SCRIPT_STATUS_OK) {
    budget->failure_status = status;
    budget->failure_phase = phase;
    budget->failure_message = message;
  }
  return -1;
}

static int ts_host_budget_poll_interrupt(ts_host_call_budget_t *budget) {
  if (!budget || !budget->interrupt) return 0;
  if (!budget->interrupt(budget->interrupt_user_data)) return 0;
  return ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_INTERRUPTED,
                             TURBO_SCRIPT_ERROR_PHASE_INTERRUPT, "call interrupted");
}

static int ts_host_budget_charge_u32(ts_host_call_budget_t *budget, uint32_t *remaining,
                                     size_t cost, const char *message) {
  if (!budget || !remaining || cost > UINT32_MAX || *remaining < (uint32_t)cost)
    return ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
                               TURBO_SCRIPT_ERROR_PHASE_CALL, message);
  *remaining -= (uint32_t)cost;
  return 0;
}

turbo_script_status_t ts_host_call_budget_init(ts_host_call_budget_t *budget,
                                               const turbo_script_instance_t *instance,
                                               const turbo_script_call_options_t *options) {
  if (!budget || !instance || !options) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  memset(budget, 0, sizeof(*budget));
  budget->steps_left = ts_host_min_u32(options->max_steps, TS_HOST_HARD_STEP_LIMIT);
  budget->loops_left = ts_host_min_u32(options->max_loop_iterations, TS_HOST_HARD_LOOP_LIMIT);
  budget->callbacks_left =
      ts_host_min_u32(options->max_host_callbacks, TS_HOST_HARD_CALLBACK_LIMIT);
  budget->recursion_left =
      ts_host_min_u32(ts_host_min_u32(options->max_recursion, instance->limits.max_recursion),
                      TS_HOST_HARD_RECURSION_LIMIT);
  budget->recursion_limit = budget->recursion_left;
  budget->stack_bytes_left = instance->limits.max_stack_bytes;
  budget->stack_bytes_limit = budget->stack_bytes_left;
  budget->result_bytes_left =
      ts_host_min_size(options->max_result_bytes, instance->limits.max_result_bytes);
  budget->retained_bytes_limit = instance->limits.max_retained_bytes;
  budget->interrupt = options->interrupt;
  budget->interrupt_user_data = options->interrupt_user_data;
  budget->failure_status = TURBO_SCRIPT_STATUS_OK;
  budget->failure_phase = TURBO_SCRIPT_ERROR_PHASE_NONE;
  return TURBO_SCRIPT_STATUS_OK;
}

static int ts_host_exprtk_safe_point(void *user_data, exprtk_safe_point_kind_t kind, size_t cost) {
  return ts_host_safe_point((turbo_script_ctx_t *)user_data, kind, cost);
}

turbo_script_status_t ts_host_call_budget_attach(turbo_script_ctx_t *runtime_ctx,
                                                 ts_host_call_budget_t *budget) {
  if (!runtime_ctx || !budget) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (runtime_ctx->active_host_budget || runtime_ctx->env.safe_point ||
      runtime_ctx->env.safe_point_user_data)
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  runtime_ctx->active_host_budget = budget;
  runtime_ctx->env.safe_point = ts_host_exprtk_safe_point;
  runtime_ctx->env.safe_point_user_data = runtime_ctx;
  return TURBO_SCRIPT_STATUS_OK;
}

void ts_host_call_budget_detach(turbo_script_ctx_t *runtime_ctx, ts_host_call_budget_t *budget) {
  if (!runtime_ctx || runtime_ctx->active_host_budget != budget) return;
  runtime_ctx->env.safe_point = NULL;
  runtime_ctx->env.safe_point_user_data = NULL;
  runtime_ctx->active_host_budget = NULL;
}

int ts_host_safe_point(turbo_script_ctx_t *runtime_ctx, exprtk_safe_point_kind_t kind,
                       size_t cost) {
  ts_host_call_budget_t *budget;
  int result = 0;
  if (!runtime_ctx || !runtime_ctx->active_host_budget) return 0;
  budget = runtime_ctx->active_host_budget;
  if (budget->failure_status != TURBO_SCRIPT_STATUS_OK) return -1;

  switch (kind) {
  case EXPRTK_SAFE_POINT_STEP:
    result =
        ts_host_budget_charge_u32(budget, &budget->steps_left, cost, "call step quota exceeded");
    if (result == 0) {
      uint32_t increment = cost > UINT32_MAX ? UINT32_MAX : (uint32_t)cost;
      if (budget->interrupt_step_accumulator > UINT32_MAX - increment)
        budget->interrupt_step_accumulator = TS_HOST_INTERRUPT_STEP_INTERVAL;
      else budget->interrupt_step_accumulator += increment;
      if (budget->interrupt_step_accumulator >= TS_HOST_INTERRUPT_STEP_INTERVAL) {
        budget->interrupt_step_accumulator = 0;
        result = ts_host_budget_poll_interrupt(budget);
      }
    }
    break;
  case EXPRTK_SAFE_POINT_LOOP:
    result =
        ts_host_budget_charge_u32(budget, &budget->loops_left, cost, "call loop quota exceeded");
    if (result == 0) result = ts_host_budget_poll_interrupt(budget);
    break;
  case EXPRTK_SAFE_POINT_CALLBACK_BEFORE:
    result = ts_host_budget_charge_u32(budget, &budget->callbacks_left, cost,
                                       "call callback quota exceeded");
    if (result == 0) result = ts_host_budget_poll_interrupt(budget);
    break;
  case EXPRTK_SAFE_POINT_CALLBACK_AFTER:
    result = ts_host_budget_poll_interrupt(budget);
    break;
  case EXPRTK_SAFE_POINT_FUNCTION_ENTER:
    if (budget->recursion_left == 0)
      result = ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
                                   TURBO_SCRIPT_ERROR_PHASE_CALL, "call recursion quota exceeded");
    else if (budget->stack_bytes_left < cost)
      result = ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
                                   TURBO_SCRIPT_ERROR_PHASE_CALL, "call stack quota exceeded");
    else {
      budget->recursion_left--;
      budget->stack_bytes_left -= cost;
      result = ts_host_budget_poll_interrupt(budget);
    }
    break;
  case EXPRTK_SAFE_POINT_FUNCTION_LEAVE:
    if (cost > budget->stack_bytes_limit || budget->recursion_left >= budget->recursion_limit ||
        budget->stack_bytes_left > budget->stack_bytes_limit - cost)
      result = ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_INVALID_STATE,
                                   TURBO_SCRIPT_ERROR_PHASE_CALL,
                                   "call frame accounting is inconsistent");
    else {
      budget->recursion_left++;
      budget->stack_bytes_left += cost;
    }
    break;
  case EXPRTK_SAFE_POINT_RETAINED_BYTES:
    if (cost > budget->retained_bytes_limit)
      result =
          ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
                              TURBO_SCRIPT_ERROR_PHASE_CALL, "call retained memory quota exceeded");
    break;
  default:
    result = ts_host_budget_fail(budget, TURBO_SCRIPT_STATUS_INVALID_STATE,
                                 TURBO_SCRIPT_ERROR_PHASE_CALL, "invalid call safe point");
    break;
  }

  if (result != 0) {
    runtime_ctx->env.aborted = 1;
    runtime_ctx->error_code = budget->failure_status == TURBO_SCRIPT_STATUS_INTERRUPTED
                                  ? TURBO_SCRIPT_ERROR_CANCELLED
                                  : TURBO_SCRIPT_ERROR_RUNTIME;
    snprintf(runtime_ctx->env.error_msg, sizeof(runtime_ctx->env.error_msg), "%s",
             budget->failure_message ? budget->failure_message : "call budget failed");
    snprintf(runtime_ctx->error_msg, sizeof(runtime_ctx->error_msg), "%s",
             runtime_ctx->env.error_msg);
  }
  return result;
}

turbo_script_status_t ts_host_call_budget_status(const ts_host_call_budget_t *budget) {
  return budget ? budget->failure_status : TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
}

turbo_script_error_phase_t ts_host_call_budget_phase(const ts_host_call_budget_t *budget) {
  return budget ? budget->failure_phase : TURBO_SCRIPT_ERROR_PHASE_NONE;
}

const char *ts_host_call_budget_message(const ts_host_call_budget_t *budget) {
  return budget && budget->failure_message ? budget->failure_message : "call budget failed";
}
