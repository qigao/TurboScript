#include "turbo_script_host_internal.h"

#include "../mir/turbo_script_mir_internal.h"
#include "../turbo_script_internal.h"
#include "exprtk.h"
#include "turbo_vstr.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  TS_HOST_INSTANCE_RETAINED_LIMIT = 128 * 1024 * 1024,
  TS_HOST_INSTANCE_STACK_LIMIT = 1024 * 1024,
  TS_HOST_INSTANCE_RECURSION_LIMIT = 100,
  TS_HOST_INSTANCE_GLOBAL_LIMIT = 4096,
  TS_HOST_INSTANCE_VALUE_DEPTH_LIMIT = 64,
  TS_HOST_INSTANCE_VALUE_NODE_LIMIT = 65536,
  TS_HOST_INSTANCE_RESULT_LIMIT = 4 * 1024 * 1024,
  TS_HOST_INSTANCE_NAME_LIMIT = 4 * 1024 * 1024,
  TS_HOST_CALL_STEP_LIMIT = 100000,
  TS_HOST_CALL_LOOP_LIMIT = 10000,
  TS_HOST_CALL_CALLBACK_LIMIT = 1024,
};

static turbo_script_status_t ts_host_instance_fail(turbo_script_result_t *result,
                                                   turbo_script_status_t status,
                                                   turbo_script_error_phase_t phase,
                                                   turbo_script_string_view_t module_name,
                                                   turbo_script_string_view_t function_name,
                                                   const char *message, size_t max_bytes) {
  int32_t error_code = TURBO_SCRIPT_ERROR_RUNTIME;
  if (status == TURBO_SCRIPT_STATUS_INVALID_ARGUMENT ||
      status == TURBO_SCRIPT_STATUS_VALIDATION_ERROR ||
      status == TURBO_SCRIPT_STATUS_INVALID_UTF8 ||
      status == TURBO_SCRIPT_STATUS_DUPLICATE_RECORD_KEY ||
      status == TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE ||
      status == TURBO_SCRIPT_STATUS_NOT_FOUND)
    error_code = TURBO_SCRIPT_ERROR_ARGUMENT;
  else if (status == TURBO_SCRIPT_STATUS_OUT_OF_MEMORY) error_code = TURBO_SCRIPT_ERROR_OOM;
  else if (status == TURBO_SCRIPT_STATUS_INVALID_STATE ||
           status == TURBO_SCRIPT_STATUS_REENTRANT_CALL ||
           status == TURBO_SCRIPT_STATUS_CONTEXT_MISMATCH ||
           status == TURBO_SCRIPT_STATUS_WRONG_THREAD)
    error_code = TURBO_SCRIPT_ERROR_STATE;
  else if (status == TURBO_SCRIPT_STATUS_INTERRUPTED) error_code = TURBO_SCRIPT_ERROR_CANCELLED;
  turbo_script_error_info_t error = {
      .struct_size = sizeof(error),
      .status = status,
      .error_code = error_code,
      .phase = phase,
      .module_name = module_name,
      .function_name = function_name,
      .message = {message, strlen(message)},
  };
  (void)ts_host_result_set_error(result, &error, max_bytes);
  return status;
}

static void ts_host_instance_clear_diagnostic(turbo_script_instance_t *instance) {
  if (!instance) return;
  tstr_freep(&instance->diagnostic.message);
  memset(&instance->diagnostic, 0, sizeof(instance->diagnostic));
}

turbo_script_status_t ts_host_instance_begin_call(turbo_script_instance_t *instance) {
  turbo_script_ctx_t *ctx;
  if (!instance || !instance->module || !instance->module->ctx)
    return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  ctx = instance->module->ctx;
  if (instance->state == TS_HOST_INSTANCE_FAULTED) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  if (instance->state != TS_HOST_INSTANCE_READY || ctx->host_callback_depth != 0 ||
      ctx->active_host_instance != NULL)
    return TURBO_SCRIPT_STATUS_REENTRANT_CALL;
  ts_host_instance_clear_diagnostic(instance);
  instance->runtime_ctx->error_code = TURBO_SCRIPT_ERROR_NONE;
  instance->runtime_ctx->error_msg[0] = '\0';
  instance->runtime_ctx->env.error_msg[0] = '\0';
  instance->runtime_ctx->env.error_line = 0;
  instance->runtime_ctx->env.error_column = 0;
  instance->runtime_ctx->env.last_line = 0;
  instance->runtime_ctx->env.last_column = 0;
  exprtk_value_destroy(&instance->runtime_ctx->env.error_value);
  instance->runtime_ctx->env.error_value.type = EXPRTK_VAL_NULL;
  ctx->active_host_instance = instance;
  instance->state = TS_HOST_INSTANCE_CALLING;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_instance_enter_callback(turbo_script_ctx_t *ctx,
                                                      turbo_script_instance_t **out_instance) {
  turbo_script_instance_t *instance;
  if (!ctx || !out_instance) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_instance = NULL;
  if (ctx->host_callback_depth == SIZE_MAX) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  instance = ctx->active_host_instance;
  if (instance && instance->state != TS_HOST_INSTANCE_CALLING)
    return TURBO_SCRIPT_STATUS_REENTRANT_CALL;
  ctx->host_callback_depth++;
  if (instance) instance->state = TS_HOST_INSTANCE_CALLING_HOST;
  *out_instance = instance;
  return TURBO_SCRIPT_STATUS_OK;
}

void ts_host_instance_leave_callback(turbo_script_ctx_t *ctx, turbo_script_instance_t *instance) {
  if (!ctx || ctx->host_callback_depth == 0) return;
  if (instance && ctx->active_host_instance == instance &&
      instance->state == TS_HOST_INSTANCE_CALLING_HOST)
    instance->state = TS_HOST_INSTANCE_CALLING;
  ctx->host_callback_depth--;
}

turbo_script_status_t ts_host_instance_finish_call(turbo_script_instance_t *instance,
                                                   turbo_script_status_t status) {
  turbo_script_ctx_t *ctx;
  if (!instance || !instance->module || !instance->module->ctx)
    return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  ctx = instance->module->ctx;
  if (ctx->active_host_instance != instance || instance->state != TS_HOST_INSTANCE_CALLING ||
      ctx->host_callback_depth != 0) {
    if (ctx->active_host_instance == instance) ctx->active_host_instance = NULL;
    instance->state = TS_HOST_INSTANCE_FAULTED;
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  }
  ctx->active_host_instance = NULL;
  instance->state =
      status == TURBO_SCRIPT_STATUS_OK ? TS_HOST_INSTANCE_READY : TS_HOST_INSTANCE_FAULTED;
  return status;
}

void ts_host_instance_record_callback_error(turbo_script_instance_t *instance,
                                            turbo_script_status_t status, int32_t cause_code,
                                            turbo_script_string_view_t function_name,
                                            turbo_script_string_view_t message, uint32_t line,
                                            uint32_t column) {
  ts_host_runtime_diagnostic_t *diagnostic;
  if (!instance) return;
  diagnostic = &instance->diagnostic;
  ts_host_instance_clear_diagnostic(instance);
  diagnostic->status = TURBO_SCRIPT_STATUS_HOST_ERROR;
  diagnostic->error_code = status == TURBO_SCRIPT_STATUS_OUT_OF_MEMORY ? TURBO_SCRIPT_ERROR_OOM
                           : status == TURBO_SCRIPT_STATUS_INTERRUPTED
                               ? TURBO_SCRIPT_ERROR_CANCELLED
                               : TURBO_SCRIPT_ERROR_RUNTIME;
  diagnostic->phase = TURBO_SCRIPT_ERROR_PHASE_HOST_CALLBACK;
  diagnostic->line = line;
  diagnostic->column = column;
  diagnostic->length = line != 0 ? 1U : 0U;
  diagnostic->cause_code = cause_code;
  diagnostic->function_name = function_name;
  diagnostic->message = tstr_dup_len(message.data ? message.data : "", message.size);
  diagnostic->present = 1;
}

static turbo_script_string_view_t
ts_host_instance_module_name(const turbo_script_module_t *module) {
  turbo_script_string_view_t view = {0};
  if (module && module->module_name) {
    view.data = module->module_name;
    view.size = tstr_len(module->module_name);
  }
  return view;
}

static void ts_host_instance_export_span(const turbo_script_module_t *module, size_t slot,
                                         uint32_t *out_line, uint32_t *out_column) {
  const ts_host_export_entry_t *entry;
  const exprtk_node_t *node;
  if (!out_line || !out_column) return;
  *out_line = 0;
  *out_column = 0;
  if (!module) return;
  entry = (const ts_host_export_entry_t *)vec_at_const(&module->exports.entries, slot);
  if (!entry) return;
  node = entry->function_node ? entry->function_node : entry->declaration_node;
  if (!node) return;
  if (node->line > 0) *out_line = (uint32_t)node->line;
  if (node->column > 0) *out_column = (uint32_t)node->column;
}

static turbo_script_status_t ts_host_instance_publish_execution_error(
    turbo_script_instance_t *instance, turbo_script_result_t *result, turbo_script_status_t status,
    turbo_script_error_phase_t phase, turbo_script_string_view_t function_name, size_t slot,
    const char *default_message) {
  ts_host_runtime_diagnostic_t *diagnostic = &instance->diagnostic;
  exprtk_env_t *runtime_env = &instance->runtime_ctx->env;
  turbo_script_error_info_t error = {
      .struct_size = sizeof(error),
      .status = status,
      .error_code = status == TURBO_SCRIPT_STATUS_INTERRUPTED ? TURBO_SCRIPT_ERROR_CANCELLED
                                                              : TURBO_SCRIPT_ERROR_RUNTIME,
      .phase = phase,
      .module_name = ts_host_instance_module_name(instance->module),
      .function_name = function_name,
  };
  turbo_script_string_view_t message = {0};
  turbo_script_status_t store_status;

  if (diagnostic->present) {
    error.status = diagnostic->status;
    error.error_code = diagnostic->error_code;
    error.phase = diagnostic->phase;
    error.line = diagnostic->line;
    error.column = diagnostic->column;
    error.length = diagnostic->length;
    error.cause_code = diagnostic->cause_code;
    error.function_name = diagnostic->function_name;
    if (diagnostic->message) {
      message.data = diagnostic->message;
      message.size = tstr_len(diagnostic->message);
    }
  } else {
    if (runtime_env->error_line > 0) error.line = (uint32_t)runtime_env->error_line;
    else if (runtime_env->last_line > 0) error.line = (uint32_t)runtime_env->last_line;
    if (runtime_env->error_column > 0) error.column = (uint32_t)runtime_env->error_column;
    else if (runtime_env->last_column > 0) error.column = (uint32_t)runtime_env->last_column;
    if (runtime_env->error_msg[0] != '\0') {
      message.data = runtime_env->error_msg;
      message.size = strlen(runtime_env->error_msg);
    } else if (runtime_env->error_value.type == EXPRTK_VAL_STRING &&
               runtime_env->error_value.data.string.data) {
      message.data = runtime_env->error_value.data.string.data;
      message.size = runtime_env->error_value.data.string.len;
    }
  }
  if (error.line == 0 || error.column == 0) {
    uint32_t fallback_line;
    uint32_t fallback_column;
    ts_host_instance_export_span(instance->module, slot, &fallback_line, &fallback_column);
    if (error.line == 0) error.line = fallback_line;
    if (error.column == 0) error.column = fallback_column;
  }
  error.length = error.line != 0 ? 1U : 0U;
  if (!message.data) {
    message.data = default_message;
    message.size = strlen(default_message);
  }
  error.message = message;
  store_status = ts_host_result_set_error(result, &error, instance->limits.max_result_bytes);
  ts_host_instance_clear_diagnostic(instance);
  return store_status == TURBO_SCRIPT_STATUS_OK ? error.status : store_status;
}

static int ts_host_instance_options_valid(const turbo_script_instance_options_t *options) {
  if (!options || options->struct_size != sizeof(*options) ||
      (options->mode != TURBO_SCRIPT_EXEC_INTERPRETER && options->mode != TURBO_SCRIPT_EXEC_JIT) ||
      options->max_retained_bytes == 0 || options->max_stack_bytes == 0 ||
      options->max_recursion == 0 || options->max_globals == 0 || options->max_value_depth == 0 ||
      options->reserved0 != 0 || options->max_value_nodes == 0 || options->max_result_bytes == 0)
    return 0;
  for (size_t i = 0; i < 4; ++i)
    if (options->reserved[i] != 0) return 0;
  return 1;
}

static int ts_host_call_options_valid(const turbo_script_call_options_t *options) {
  if (!options || options->struct_size != sizeof(*options) || options->max_recursion == 0 ||
      options->max_steps == 0 || options->max_loop_iterations == 0 ||
      options->max_host_callbacks == 0 || options->reserved0 != 0 || options->max_result_bytes == 0)
    return 0;
  for (size_t i = 0; i < 4; ++i)
    if (options->reserved[i] != 0) return 0;
  return 1;
}

static turbo_script_status_t ts_host_instance_validate_name(turbo_script_string_view_t name) {
  if (!name.data && name.size != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (name.size == 0 || name.size > TS_HOST_INSTANCE_NAME_LIMIT ||
      memchr(name.data, '\0', name.size))
    return name.size > TS_HOST_INSTANCE_NAME_LIMIT ? TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED
                                                   : TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  return vstr_utf8_valid(vstr_from_buf(name.data, name.size)) ? TURBO_SCRIPT_STATUS_OK
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

static turbo_script_status_t ts_host_instance_bind_exports(turbo_script_module_t *module,
                                                           turbo_script_ctx_t *runtime) {
  size_t export_count;
  exprtk_func_t **bindings;
  if (!module || !runtime) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  export_count = ts_host_module_export_count(module);
  if (export_count == 0) return TURBO_SCRIPT_STATUS_OK;
  bindings =
      (exprtk_func_t **)mem_alloc_array(&runtime->env.arena, sizeof(*bindings), export_count);
  if (!bindings) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  for (size_t i = 0; i < export_count; ++i) {
    const char *name = ts_host_module_export_name(module, i);
    exprtk_func_t *match = NULL;
    if (!name) return TURBO_SCRIPT_STATUS_INVALID_STATE;
    for (exprtk_env_t *env = &runtime->env; env && !match; env = env->parent) {
      for (exprtk_func_t *candidate = env->funcs; candidate; candidate = candidate->next) {
        if (candidate->is_script && candidate->name && strcmp(candidate->name, name) == 0) {
          match = candidate;
          break;
        }
      }
    }
    if (!match || !match->data.script.body ||
        match->data.script.arg_count != ts_host_module_export_arity(module, i))
      return TURBO_SCRIPT_STATUS_INVALID_STATE;
    bindings[i] = match;
  }
  runtime->host_export_functions = bindings;
  runtime->host_export_function_count = export_count;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t
ts_host_instance_runtime_create(turbo_script_module_t *module,
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
  runtime->env.max_nodes =
      options->max_value_nodes > UINT32_MAX ? UINT32_MAX : (uint32_t)options->max_value_nodes;
  runtime->env.max_external_value_bytes = options->max_result_bytes;
  status = ts_host_registry_bind_runtime(module->ctx, &runtime->env);
  if (status != TURBO_SCRIPT_STATUS_OK) {
    ts_host_instance_runtime_destroy(runtime);
    return status;
  }
  *out_runtime = runtime;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_instance_begin(turbo_script_instance_t *instance,
                                                    turbo_script_result_t *result,
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
    return ts_host_instance_fail(result, TURBO_SCRIPT_STATUS_INVALID_STATE, phase,
                                 ts_host_instance_module_name(instance->module),
                                 (turbo_script_string_view_t){0}, "context is closing",
                                 instance->limits.max_result_bytes);
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_instance_decode_handle(const turbo_script_instance_t *instance,
                                                            turbo_script_export_handle_t handle,
                                                            size_t *out_slot) {
  uint32_t generation;
  uint32_t encoded_slot;
  if (!instance || !out_slot || handle == 0) return TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE;
  generation = (uint32_t)(handle >> 32);
  encoded_slot = (uint32_t)handle;
  if (generation == 0 || encoded_slot == 0 || generation != instance->generation ||
      (size_t)(encoded_slot - 1U) >= ts_host_module_export_count(instance->module))
    return TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE;
  *out_slot = (size_t)(encoded_slot - 1U);
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_export_handle_t ts_host_export_handle(uint32_t generation, uint32_t slot) {
  return ((uint64_t)generation << 32) | ((uint64_t)slot + 1U);
}

void turbo_script_instance_options_init(turbo_script_instance_options_t *options) {
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

void turbo_script_call_options_init(turbo_script_call_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->max_recursion = TS_HOST_INSTANCE_RECURSION_LIMIT;
  options->max_steps = TS_HOST_CALL_STEP_LIMIT;
  options->max_loop_iterations = TS_HOST_CALL_LOOP_LIMIT;
  options->max_host_callbacks = TS_HOST_CALL_CALLBACK_LIMIT;
  options->max_result_bytes = TS_HOST_INSTANCE_RESULT_LIMIT;
}

turbo_script_status_t turbo_script_instance_create(turbo_script_module_t *module,
                                                   const turbo_script_instance_options_t *options,
                                                   turbo_script_result_t *result,
                                                   turbo_script_instance_t **out_instance) {
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
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "instance output is required", TS_HOST_INSTANCE_RESULT_LIMIT);
  *out_instance = NULL;
  if (atomic_load_explicit(&ctx->closing, memory_order_acquire) || !module->accepting_instances)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_STATE, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "module context is closing", TS_HOST_INSTANCE_RESULT_LIMIT);
  if (!ts_host_instance_options_valid(options))
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "invalid instance options", TS_HOST_INSTANCE_RESULT_LIMIT);
  if (ctx->next_instance_generation == 0 || ctx->next_instance_generation > UINT32_MAX)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "instance generation exhausted", options->max_result_bytes);
  instance = (turbo_script_instance_t *)calloc(1, sizeof(*instance));
  if (!instance)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_OUT_OF_MEMORY, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE,
        ts_host_instance_module_name(module), (turbo_script_string_view_t){0},
        "instance allocation failed", options->max_result_bytes);
  /* Initializer callbacks may close the public module handle.  Pin the code,
   * AST and parent context before constructing any callback-capable runtime
   * state; this reference becomes the instance's ownership on success. */
  ts_host_module_retain(module);
  instance->module = module;
  instance->limits = *options;
  instance->mode = options->mode;
  instance->state = TS_HOST_INSTANCE_READY;
  status = ts_host_instance_runtime_create(module, options, &instance->runtime_ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) goto fail;
  status = ts_host_module_execute_initializer_with_runtime(module, instance->runtime_ctx,
                                                           instance->mode == TURBO_SCRIPT_EXEC_JIT);
  if (status != TURBO_SCRIPT_STATUS_OK) goto fail;
  status = ts_host_instance_bind_exports(module, instance->runtime_ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) goto fail;
  instance->initializer_count = 1;
  if (ts_host_instance_retained_bytes(instance->runtime_ctx) > options->max_retained_bytes) {
    status = TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    goto fail;
  }
  instance->generation = (uint32_t)ctx->next_instance_generation;
  ctx->next_instance_generation++;
  *out_instance = instance;
  return TURBO_SCRIPT_STATUS_OK;

fail:
  ts_host_instance_runtime_destroy(instance->runtime_ctx);
  free(instance);
  status = ts_host_instance_fail(
      result, status, TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE, ts_host_instance_module_name(module),
      (turbo_script_string_view_t){0},
      status == TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED ? "instance retained memory limit exceeded"
                                                   : "instance initialization failed",
      options->max_result_bytes);
  ts_host_module_release(module);
  return status;
}

turbo_script_status_t turbo_script_instance_destroy(turbo_script_instance_t *instance,
                                                    turbo_script_result_t *result) {
  turbo_script_status_t status =
      ts_host_instance_begin(instance, result, TURBO_SCRIPT_ERROR_PHASE_NONE);
  turbo_script_module_t *module;
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (instance->state != TS_HOST_INSTANCE_READY && instance->state != TS_HOST_INSTANCE_FAULTED)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_STATE, TURBO_SCRIPT_ERROR_PHASE_NONE,
        ts_host_instance_module_name(instance->module), (turbo_script_string_view_t){0},
        "instance cannot be destroyed while executing", instance->limits.max_result_bytes);
  module = instance->module;
  ts_host_instance_clear_diagnostic(instance);
  instance->state = TS_HOST_INSTANCE_DESTROYED;
  ts_host_instance_runtime_destroy(instance->runtime_ctx);
  free(instance);
  ts_host_module_release(module);
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t
turbo_script_instance_resolve_export(turbo_script_instance_t *instance,
                                     turbo_script_string_view_t name, turbo_script_result_t *result,
                                     turbo_script_export_handle_t *out_handle) {
  turbo_script_status_t status =
      ts_host_instance_begin(instance, result, TURBO_SCRIPT_ERROR_PHASE_RESOLVE);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_handle)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
        ts_host_instance_module_name(instance->module), name, "export handle output is required",
        instance->limits.max_result_bytes);
  *out_handle = 0;
  status = ts_host_instance_validate_name(name);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(result, status, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
                                 ts_host_instance_module_name(instance->module), name,
                                 "invalid export name", instance->limits.max_result_bytes);
  for (size_t i = 0; i < ts_host_module_export_count(instance->module); ++i) {
    const char *candidate = ts_host_module_export_name(instance->module, i);
    if (candidate && strlen(candidate) == name.size &&
        memcmp(candidate, name.data, name.size) == 0) {
      if (i >= UINT32_MAX)
        return ts_host_instance_fail(
            result, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
            ts_host_instance_module_name(instance->module), name, "export slot cannot be encoded",
            instance->limits.max_result_bytes);
      *out_handle = ts_host_export_handle(instance->generation, (uint32_t)i);
      return TURBO_SCRIPT_STATUS_OK;
    }
  }
  return ts_host_instance_fail(result, TURBO_SCRIPT_STATUS_NOT_FOUND,
                               TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
                               ts_host_instance_module_name(instance->module), name,
                               "export was not found", instance->limits.max_result_bytes);
}

turbo_script_status_t turbo_script_instance_get_export_info(turbo_script_instance_t *instance,
                                                            turbo_script_export_handle_t handle,
                                                            turbo_script_result_t *result,
                                                            turbo_script_export_info_t *out_info) {
  turbo_script_export_info_t info;
  turbo_script_status_t status =
      ts_host_instance_begin(instance, result, TURBO_SCRIPT_ERROR_PHASE_RESOLVE);
  size_t slot = 0;
  const char *name;
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_info || out_info->struct_size != sizeof(*out_info) || out_info->reserved0 != 0)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
        ts_host_instance_module_name(instance->module), (turbo_script_string_view_t){0},
        "invalid export info structure", instance->limits.max_result_bytes);
  for (size_t i = 0; i < 4; ++i)
    if (out_info->reserved[i] != 0)
      return ts_host_instance_fail(
          result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
          ts_host_instance_module_name(instance->module), (turbo_script_string_view_t){0},
          "invalid export info structure", instance->limits.max_result_bytes);
  status = ts_host_instance_decode_handle(instance, handle, &slot);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(result, status, TURBO_SCRIPT_ERROR_PHASE_RESOLVE,
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

turbo_script_execution_mode_t ts_host_instance_mode(const turbo_script_instance_t *instance) {
  return instance ? instance->mode : 0;
}

uint32_t ts_host_instance_generation(const turbo_script_instance_t *instance) {
  return instance ? instance->generation : 0;
}

size_t ts_host_instance_initializer_count(const turbo_script_instance_t *instance) {
  return instance ? instance->initializer_count : 0;
}

turbo_script_status_t ts_host_instance_execute_numeric(turbo_script_instance_t *instance,
                                                       turbo_script_export_handle_t handle,
                                                       const double *args, size_t arg_count,
                                                       double *out_result) {
  turbo_script_status_t status;
  size_t slot = 0;
  if (!instance || (!args && arg_count) || !out_result) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_check_owner_thread(instance->module->ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (atomic_load_explicit(&instance->module->ctx->closing, memory_order_acquire))
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  if (instance->state == TS_HOST_INSTANCE_FAULTED) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  if (instance->state != TS_HOST_INSTANCE_READY || instance->module->ctx->host_callback_depth != 0)
    return TURBO_SCRIPT_STATUS_REENTRANT_CALL;
  status = ts_host_instance_decode_handle(instance, handle, &slot);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_instance_begin_call(instance);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_module_execute_numeric_with_runtime(
      instance->module, instance->runtime_ctx, slot, instance->mode == TURBO_SCRIPT_EXEC_JIT, args,
      arg_count, out_result);
  status = ts_host_instance_finish_call(instance, status);
  ts_host_instance_clear_diagnostic(instance);
  return status;
}

turbo_script_status_t turbo_script_instance_call(turbo_script_instance_t *instance,
                                                 turbo_script_export_handle_t handle,
                                                 const turbo_script_value_view_t *args,
                                                 size_t arg_count,
                                                 const turbo_script_call_options_t *options,
                                                 turbo_script_result_t *result) {
  exprtk_env_t call_env;
  exprtk_value_t call_args[16];
  exprtk_value_t output = {.type = EXPRTK_VAL_NULL};
  ts_host_call_budget_t budget;
  ts_host_value_limits_t input_limits;
  ts_host_value_limits_t output_limits;
  turbo_script_status_t status;
  turbo_script_status_t finish_status;
  turbo_script_error_phase_t failure_phase = TURBO_SCRIPT_ERROR_PHASE_CALL;
  turbo_script_string_view_t function_name = {0};
  const char *failure_message = "call value conversion failed";
  size_t slot = 0;
  size_t converted = 0;
  uint32_t saved_max_recursion = 0;
  uint32_t saved_max_nodes = 0;
  uint32_t saved_max_loop_iterations = 0;
  int call_env_initialized = 0;
  int call_started = 0;
  int budget_attached = 0;
  int limits_applied = 0;
  int backend_status;

  status = ts_host_instance_begin(instance, result, TURBO_SCRIPT_ERROR_PHASE_CALL);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (instance->state == TS_HOST_INSTANCE_FAULTED)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_INVALID_STATE, TURBO_SCRIPT_ERROR_PHASE_CALL,
        ts_host_instance_module_name(instance->module), function_name,
        "faulted instance rejects future calls", instance->limits.max_result_bytes);
  if (instance->state != TS_HOST_INSTANCE_READY ||
      instance->module->ctx->host_callback_depth != 0 ||
      instance->module->ctx->active_host_instance != NULL)
    return ts_host_instance_fail(
        result, TURBO_SCRIPT_STATUS_REENTRANT_CALL, TURBO_SCRIPT_ERROR_PHASE_CALL,
        ts_host_instance_module_name(instance->module), function_name,
        "instance call is not reentrant", instance->limits.max_result_bytes);
  if (!ts_host_call_options_valid(options))
    return ts_host_instance_fail(result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
                                 TURBO_SCRIPT_ERROR_PHASE_CALL,
                                 ts_host_instance_module_name(instance->module), function_name,
                                 "invalid call options", instance->limits.max_result_bytes);
  if ((!args && arg_count != 0) || arg_count > 16)
    return ts_host_instance_fail(result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
                                 TURBO_SCRIPT_ERROR_PHASE_CALL,
                                 ts_host_instance_module_name(instance->module), function_name,
                                 "invalid call arguments", instance->limits.max_result_bytes);
  status = ts_host_instance_decode_handle(instance, handle, &slot);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(result, status, TURBO_SCRIPT_ERROR_PHASE_CALL,
                                 ts_host_instance_module_name(instance->module), function_name,
                                 "invalid export handle", instance->limits.max_result_bytes);
  {
    const char *name = ts_host_module_export_name(instance->module, slot);
    if (!name)
      return ts_host_instance_fail(
          result, TURBO_SCRIPT_STATUS_INVALID_STATE, TURBO_SCRIPT_ERROR_PHASE_CALL,
          ts_host_instance_module_name(instance->module), function_name,
          "export metadata is unavailable", instance->limits.max_result_bytes);
    function_name = (turbo_script_string_view_t){name, strlen(name)};
  }
  if (arg_count != ts_host_module_export_arity(instance->module, slot))
    return ts_host_instance_fail(result, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT,
                                 TURBO_SCRIPT_ERROR_PHASE_CALL,
                                 ts_host_instance_module_name(instance->module), function_name,
                                 "export arity mismatch", instance->limits.max_result_bytes);

  input_limits.max_depth = instance->limits.max_value_depth;
  input_limits.max_nodes = instance->limits.max_value_nodes;
  input_limits.max_bytes = instance->limits.max_retained_bytes;
  status = ts_host_call_budget_init(&budget, instance, options);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(result, status, TURBO_SCRIPT_ERROR_PHASE_CALL,
                                 ts_host_instance_module_name(instance->module), function_name,
                                 "invalid call budget", instance->limits.max_result_bytes);
  output_limits.max_depth = instance->limits.max_value_depth;
  output_limits.max_nodes = instance->limits.max_value_nodes;
  output_limits.max_bytes = budget.result_bytes_left;
  status = ts_host_validate_value_views(args, arg_count, &input_limits);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_instance_fail(result, status, TURBO_SCRIPT_ERROR_PHASE_CALL,
                                 ts_host_instance_module_name(instance->module), function_name,
                                 "call argument validation failed",
                                 instance->limits.max_result_bytes);
  exprtk_env_init(&call_env);
  call_env_initialized = 1;
  call_env.max_external_value_bytes = instance->limits.max_retained_bytes;
  for (; converted < arg_count; ++converted) {
    call_args[converted] = (exprtk_value_t){.type = EXPRTK_VAL_NULL};
    status = ts_host_value_from_view_in_env(&call_env, &args[converted], &input_limits,
                                            &call_args[converted]);
    if (status != TURBO_SCRIPT_STATUS_OK) goto preflight_fail;
  }

  saved_max_recursion = instance->runtime_ctx->env.max_recursion;
  saved_max_nodes = instance->runtime_ctx->env.max_nodes;
  saved_max_loop_iterations = instance->runtime_ctx->env.max_loop_iterations;
  instance->runtime_ctx->env.max_recursion = budget.recursion_left;
  instance->runtime_ctx->env.max_nodes = budget.steps_left;
  instance->runtime_ctx->env.max_loop_iterations = budget.loops_left;
  limits_applied = 1;
  status = ts_host_instance_begin_call(instance);
  if (status != TURBO_SCRIPT_STATUS_OK) goto preflight_fail;
  call_started = 1;
  status = ts_host_call_budget_attach(instance->runtime_ctx, &budget);
  if (status != TURBO_SCRIPT_STATUS_OK) {
    failure_message = "call budget attachment failed";
    goto finish;
  }
  budget_attached = 1;
  backend_status =
      instance->mode == TURBO_SCRIPT_EXEC_JIT
          ? ts_mir_artifact_call_jit(instance->module->artifact, instance->runtime_ctx, slot,
                                     call_args, arg_count, &output)
          : ts_mir_artifact_call_interp(instance->module->artifact, instance->runtime_ctx, slot,
                                        call_args, arg_count, &output);
  if (backend_status != 0) {
    if (ts_host_call_budget_status(&budget) != TURBO_SCRIPT_STATUS_OK) {
      status = ts_host_call_budget_status(&budget);
      failure_phase = ts_host_call_budget_phase(&budget);
      failure_message = ts_host_call_budget_message(&budget);
    } else {
      status = instance->diagnostic.present ? instance->diagnostic.status
                                            : TURBO_SCRIPT_STATUS_RUNTIME_ERROR;
      failure_phase =
          instance->diagnostic.present ? instance->diagnostic.phase : TURBO_SCRIPT_ERROR_PHASE_CALL;
      failure_message = "export execution failed: recursion or runtime error";
    }
    goto finish;
  }
  status = ts_host_result_store_exprtk(result, &output, &output_limits);
  if (status != TURBO_SCRIPT_STATUS_OK) {
    failure_message = status == TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED
                          ? "call result quota exceeded"
                          : "call result conversion failed";
    goto finish;
  }
  {
    size_t retained_bytes = ts_host_instance_retained_bytes(instance->runtime_ctx);
    if (ts_host_safe_point(instance->runtime_ctx, EXPRTK_SAFE_POINT_RETAINED_BYTES,
                           retained_bytes) != 0) {
      status = ts_host_call_budget_status(&budget);
      failure_phase = ts_host_call_budget_phase(&budget);
      failure_message = ts_host_call_budget_message(&budget);
      goto finish;
    }
  }

finish:
  if (budget_attached) {
    ts_host_call_budget_detach(instance->runtime_ctx, &budget);
    budget_attached = 0;
  }
  if (limits_applied) {
    instance->runtime_ctx->env.max_recursion = saved_max_recursion;
    instance->runtime_ctx->env.max_nodes = saved_max_nodes;
    instance->runtime_ctx->env.max_loop_iterations = saved_max_loop_iterations;
    limits_applied = 0;
  }
  finish_status = ts_host_instance_finish_call(instance, status);
  call_started = 0;
  if (finish_status != status) status = finish_status;

  exprtk_value_destroy(&output);
  for (size_t i = 0; i < converted; ++i)
    exprtk_value_destroy(&call_args[i]);
  exprtk_env_free(&call_env);
  call_env_initialized = 0;
  if (status == TURBO_SCRIPT_STATUS_OK) {
    ts_host_instance_clear_diagnostic(instance);
    return TURBO_SCRIPT_STATUS_OK;
  }
  return ts_host_instance_publish_execution_error(instance, result, status, failure_phase,
                                                  function_name, slot, failure_message);

preflight_fail:
  if (budget_attached) ts_host_call_budget_detach(instance->runtime_ctx, &budget);
  if (call_started) (void)ts_host_instance_finish_call(instance, status);
  if (limits_applied) {
    instance->runtime_ctx->env.max_recursion = saved_max_recursion;
    instance->runtime_ctx->env.max_nodes = saved_max_nodes;
    instance->runtime_ctx->env.max_loop_iterations = saved_max_loop_iterations;
  }
  exprtk_value_destroy(&output);
  for (size_t i = 0; i < converted; ++i)
    exprtk_value_destroy(&call_args[i]);
  if (call_env_initialized) exprtk_env_free(&call_env);
  return ts_host_instance_fail(result, status, TURBO_SCRIPT_ERROR_PHASE_CALL,
                               ts_host_instance_module_name(instance->module), function_name,
                               failure_message, instance->limits.max_result_bytes);
}
