#include "turbo_script_host_internal.h"

#include "../turbo_script_internal.h"
#include "exprtk.h"
#include "turbo_vstr.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  TS_HOST_INSTANCE_RETAINED_LIMIT = 16 * 1024 * 1024,
  TS_HOST_INSTANCE_STACK_LIMIT = 1024 * 1024,
  TS_HOST_INSTANCE_RECURSION_LIMIT = 64,
  TS_HOST_INSTANCE_GLOBAL_LIMIT = 4096,
  TS_HOST_INSTANCE_VALUE_DEPTH_LIMIT = 64,
  TS_HOST_INSTANCE_VALUE_NODE_LIMIT = 65536,
  TS_HOST_INSTANCE_RESULT_LIMIT = 4 * 1024 * 1024,
  TS_HOST_INSTANCE_NAME_LIMIT = 4 * 1024 * 1024,
};

static turbo_script_status_t ts_host_instance_fail(
    turbo_script_result_t *result, turbo_script_status_t status,
    turbo_script_error_phase_t phase, turbo_script_string_view_t module_name,
    turbo_script_string_view_t function_name, const char *message,
    size_t max_bytes) {
  turbo_script_error_info_t error = {
      .struct_size = sizeof(error),
      .status = status,
      .phase = phase,
      .module_name = module_name,
      .function_name = function_name,
      .message = {message, strlen(message)},
  };
  (void)ts_host_result_set_error(result, &error, max_bytes);
  return status;
}

static turbo_script_string_view_t ts_host_instance_module_name(
    const turbo_script_module_t *module) {
  turbo_script_string_view_t view = {0};
  if (module && module->module_name) {
    view.data = module->module_name;
    view.size = tstr_len(module->module_name);
  }
  return view;
}

static int ts_host_instance_options_valid(
    const turbo_script_instance_options_t *options) {
  if (!options || options->struct_size != sizeof(*options) ||
      (options->mode != TURBO_SCRIPT_EXEC_INTERPRETER &&
       options->mode != TURBO_SCRIPT_EXEC_JIT) ||
      options->max_retained_bytes == 0 || options->max_stack_bytes == 0 ||
      options->max_recursion == 0 || options->max_globals == 0 ||
      options->max_value_depth == 0 || options->reserved0 != 0 ||
      options->max_value_nodes == 0 || options->max_result_bytes == 0)
    return 0;
  for (size_t i = 0; i < 4; ++i)
    if (options->reserved[i] != 0) return 0;
  return 1;
}

static turbo_script_status_t ts_host_instance_validate_name(
    turbo_script_string_view_t name) {
  if (!name.data && name.size != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (name.size == 0 || name.size > TS_HOST_INSTANCE_NAME_LIMIT ||
      memchr(name.data, '\0', name.size))
    return name.size > TS_HOST_INSTANCE_NAME_LIMIT
               ? TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED
               : TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  return vstr_utf8_valid(vstr_from_buf(name.data, name.size))
             ? TURBO_SCRIPT_STATUS_OK
             : TURBO_SCRIPT_STATUS_INVALID_UTF8;
}

static size_t ts_host_instance_retained_bytes(const turbo_script_ctx_t *runtime) {
  size_t total = mem_pool_total_used(&runtime->env.arena);
  for (const exprtk_env_t *closure = runtime->env.next_closure; closure;
       closure = closure->next_closure) {
    size_t bytes = mem_pool_total_used(&closure->arena);
    if (total > SIZE_MAX - bytes) return SIZE_MAX;
    total += bytes;
  }
  return total;
}

static void ts_host_instance_runtime_destroy(turbo_script_ctx_t *runtime) {
  if (!runtime) return;
  exprtk_env_free(&runtime->env);
  free(runtime);
}

static turbo_script_status_t ts_host_instance_runtime_create(
    turbo_script_module_t *module,
    const turbo_script_instance_options_t *options,
    turbo_script_ctx_t **out_runtime) {
  turbo_script_ctx_t *runtime = NULL;
  turbo_script_status_t status;
  *out_runtime = NULL;
  runtime = (turbo_script_ctx_t *)calloc(1, sizeof(*runtime));
  if (!runtime) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  atomic_init(&runtime->ref_count, 1U);
  atomic_init(&runtime->closing, 0);
  runtime->owner_thread_token = module->ctx->owner_thread_token;
  runtime->memory_policy.max_context_bytes = options->max_retained_bytes;
  runtime->memory_policy.max_task_bytes = options->max_stack_bytes;
  runtime->memory_policy.max_external_value_bytes = options->max_result_bytes;
  exprtk_env_init(&runtime->env);
  runtime->env.eval_node = turbo_script_mir_eval_node;
  runtime->env.exec_script_body = turbo_script_mir_exec_script_body;
  runtime->env.max_recursion = options->max_recursion;
  runtime->env.max_nodes = options->max_value_nodes > UINT32_MAX
                               ? UINT32_MAX
                               : (uint32_t)options->max_value_nodes;
  runtime->env.max_external_value_bytes = options->max_result_bytes;
  status = ts_host_registry_bind_runtime(module->ctx, &runtime->env);
  if (status != TURBO_SCRIPT_STATUS_OK) {
    ts_host_instance_runtime_destroy(runtime);
    return status;
  }
  *out_runtime = runtime;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_instance_begin(
    turbo_script_instance_t *instance, turbo_script_result_t *result,
    turbo_script_error_phase_t phase) {
  turbo_script_ctx_t *ctx;
  turbo_script_status_t status;
  if (!instance || !instance->module) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  ctx = instance->module->ctx;
  status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_reset_checked(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (atomic_load_explicit(&ctx->closing, memory_order_acquire) &&
      phase != TURBO_SCRIPT_ERROR_PHASE_NONE)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_STATE, phase,
        ts_host_instance_module_name(instance->module),
        (turbo_script_string_view_t){0}, "context is closing",
        instance->limits.max_result_bytes);
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_instance_decode_handle(
    const turbo_script_instance_t *instance,
    turbo_script_export_handle_t handle, size_t *out_slot) {
  uint32_t generation;
  uint32_t encoded_slot;
  if (!instance || !out_slot || handle == 0)
    return TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE;
  generation = (uint32_t)(handle >> 32);
  encoded_slot = (uint32_t)handle;
  if (generation == 0 || encoded_slot == 0 ||
      generation != instance->generation ||
      (size_t)(encoded_slot - 1U) >=
          ts_host_module_export_count(instance->module))
    return TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE;
  *out_slot = (size_t)(encoded_slot - 1U);
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_export_handle_t ts_host_export_handle(uint32_t generation,
                                                          uint32_t slot) {
  return ((uint64_t)generation << 32) | ((uint64_t)slot + 1U);
}

void turbo_script_instance_options_init(
    turbo_script_instance_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->mode = TURBO_SCRIPT_EXEC_INTERPRETER;
  options->max_retained_bytes = TS_HOST_INSTANCE_RETAINED_LIMIT;
  options->max_stack_bytes = TS_HOST_INSTANCE_STACK_LIMIT;
  options->max_recursion = TS_HOST_INSTANCE_RECURSION_LIMIT;
  options->max_globals = TS_HOST_INSTANCE_GLOBAL_LIMIT;
  options->max_value_depth = TS_HOST_INSTANCE_VALUE_DEPTH_LIMIT;
  options->max_value_nodes = TS_HOST_INSTANCE_VALUE_NODE_LIMIT;
  options->max_result_bytes = TS_HOST_INSTANCE_RESULT_LIMIT;
}

turbo_script_status_t turbo_script_instance_create(
    turbo_script_module_t *module,
    const turbo_script_instance_options_t *options,
    turbo_script_result_t *result, turbo_script_instance_t **out_instance) {
  turbo_script_instance_t *instance = NULL;
  turbo_script_status_t status;
  turbo_script_ctx_t *ctx;
  if (!module) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  ctx = module->ctx;
  status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_reset_checked(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_instance)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
        TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "instance output is required", TS_HOST_INSTANCE_RESULT_LIMIT);
  *out_instance = NULL;
  if (atomic_load_explicit(&ctx->closing, memory_order_acquire) ||
      !module->accepting_instances)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_STATE,
        TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "module context is closing", TS_HOST_INSTANCE_RESULT_LIMIT);
  if (!ts_host_instance_options_valid(options))
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
        TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "invalid instance options", TS_HOST_INSTANCE_RESULT_LIMIT);
  if (ctx->next_instance_generation == 0 ||
      ctx->next_instance_generation > UINT32_MAX)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
        TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "instance generation exhausted", options->max_result_bytes);
  instance = (turbo_script_instance_t *)calloc(1, sizeof(*instance));
  if (!instance)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_OUT_OF_MEMORY,
        TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "instance allocation failed", options->max_result_bytes);
  instance->module = module;
  instance->limits = *options;
  instance->mode = options->mode;
  instance->state = TS_HOST_INSTANCE_READY;
  status = ts_host_instance_runtime_create(module, options,
                                           &instance->runtime_ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) goto fail;
  status = ts_host_module_execute_initializer_with_runtime(
      module, instance->runtime_ctx,
      instance->mode == TURBO_SCRIPT_EXEC_JIT);
  if (status != TURBO_SCRIPT_STATUS_OK) goto fail;
  instance->initializer_count = 1;
  if (ts_host_instance_retained_bytes(instance->runtime_ctx) >
      options->max_retained_bytes) {
    status = TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    goto fail;
  }
  instance->generation = (uint32_t)ctx->next_instance_generation;
  ctx->next_instance_generation++;
  ts_host_module_retain(module);
  *out_instance = instance;
  return TURBO_SCRIPT_STATUS_OK;

fail:
  ts_host_instance_runtime_destroy(instance->runtime_ctx);
  free(instance);
  return ts_host_instance_fail(
      result, status, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
      ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
      status == TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED
          ? "instance retained memory limit exceeded"
          : "instance initialization failed",
      options->max_result_bytes);
}

turbo_script_status_t turbo_script_instance_destroy(
    turbo_script_instance_t *instance, turbo_script_result_t *result) {
  turbo_script_status_t status =
      ts_host_instance_begin(instance, result, TURBO_SCRIPT_ERROR_PHASE_NONE);
  turbo_script_module_t *module;
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (instance->state != TS_HOST_INSTANCE_READY)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_STATE,
        TURBO_SCRIPT_ERROR_PHASE_NONE,
        ts_host_instance_module_name(instance->module),
        (turbo_script_string_view_t){0},
        "instance cannot be destroyed while executing",
        instance->limits.max_result_bytes);
  module = instance->module;
  ts_host_instance_runtime_destroy(instance->runtime_ctx);
  free(instance);
  ts_host_module_release(module);
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t turbo_script_instance_resolve_export(
    turbo_script_instance_t *instance, turbo_script_string_view_t name,
    turbo_script_result_t *result,
    turbo_script_export_handle_t *out_handle) {
  turbo_script_status_t status = ts_host_instance_begin(
      instance, result, TURBO_SCRIPT_ERROR_PHASE_RESOLVE);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_handle)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
        TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
        ts_host_instance_module_name(instance->module), name,
        "export handle output is required", instance->limits.max_result_bytes);
  *out_handle = 0;
  status = ts_host_instance_validate_name(name);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(
        result, status, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
        ts_host_instance_module_name(instance->module), name,
        "invalid export name", instance->limits.max_result_bytes);
  for (size_t i = 0; i < ts_host_module_export_count(instance->module); ++i) {
    const char *candidate = ts_host_module_export_name(instance->module, i);
    if (candidate && strlen(candidate) == name.size &&
        memcmp(candidate, name.data, name.size) == 0) {
      if (i >= UINT32_MAX)
        return ts_host_instance_fail(
            result, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
            TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
            ts_host_instance_module_name(instance->module), name,
            "export slot cannot be encoded", instance->limits.max_result_bytes);
      *out_handle = ts_host_export_handle(instance->generation, (uint32_t)i);
      return TURBO_SCRIPT_STATUS_OK;
    }
  }
  return ts_host_instance_fail(
      result, TURBO_SCRIPT_STATUS_NOT_FOUND,
      TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
      ts_host_instance_module_name(instance->module), name,
      "export was not found", instance->limits.max_result_bytes);
}

turbo_script_status_t turbo_script_instance_get_export_info(
    turbo_script_instance_t *instance, turbo_script_export_handle_t handle,
    turbo_script_result_t *result, turbo_script_export_info_t *out_info) {
  turbo_script_export_info_t info;
  turbo_script_status_t status = ts_host_instance_begin(
      instance, result, TURBO_SCRIPT_ERROR_PHASE_RESOLVE);
  size_t slot = 0;
  const char *name;
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_info || out_info->struct_size != sizeof(*out_info) ||
      out_info->reserved0 != 0)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
        TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
        ts_host_instance_module_name(instance->module),
        (turbo_script_string_view_t){0}, "invalid export info structure",
        instance->limits.max_result_bytes);
  for (size_t i = 0; i < 4; ++i)
    if (out_info->reserved[i] != 0)
      return ts_host_instance_fail(
          result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
          TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
          ts_host_instance_module_name(instance->module),
          (turbo_script_string_view_t){0}, "invalid export info structure",
          instance->limits.max_result_bytes);
  status = ts_host_instance_decode_handle(instance, handle, &slot);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(
        result, status, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
        ts_host_instance_module_name(instance->module),
        (turbo_script_string_view_t){0}, "invalid export handle",
        instance->limits.max_result_bytes);
  name = ts_host_module_export_name(instance->module, slot);
  if (!name) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  memset(&info, 0, sizeof(info));
  info.struct_size = sizeof(info);
  info.min_arity = ts_host_module_export_arity(instance->module, slot);
  info.max_arity = info.min_arity;
  info.name = (turbo_script_string_view_t){name, strlen(name)};
  *out_info = info;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_execution_mode_t
ts_host_instance_mode(const turbo_script_instance_t *instance) {
  return instance ? instance->mode : 0;
}

uint32_t ts_host_instance_generation(const turbo_script_instance_t *instance) {
  return instance ? instance->generation : 0;
}

size_t ts_host_instance_initializer_count(
    const turbo_script_instance_t *instance) {
  return instance ? instance->initializer_count : 0;
}

turbo_script_status_t ts_host_instance_execute_numeric(
    turbo_script_instance_t *instance, turbo_script_export_handle_t handle,
    const double *args, size_t arg_count, double *out_result) {
  turbo_script_status_t status;
  size_t slot = 0;
  if (!instance || (!args && arg_count) || !out_result)
    return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_check_owner_thread(instance->module->ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (atomic_load_explicit(&instance->module->ctx->closing,
                           memory_order_acquire))
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  if (instance->state != TS_HOST_INSTANCE_READY)
    return TURBO_SCRIPT_STATUS_REENTRANT_CALL;
  status = ts_host_instance_decode_handle(instance, handle, &slot);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  instance->state = TS_HOST_INSTANCE_CALLING;
  status = ts_host_module_execute_numeric_with_runtime(
      instance->module, instance->runtime_ctx, slot,
      instance->mode == TURBO_SCRIPT_EXEC_JIT, args, arg_count, out_result);
  instance->state = TS_HOST_INSTANCE_READY;
  return status;
}
