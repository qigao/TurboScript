#include "turbo_script_host_internal.h"

#include "../turbo_script_internal.h"
#include "exprtk.h"
#include "exprtk_runtime_internal.h"
#include "turbo_vstr.h"
#include <turbostl/vec.h>

#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TS_HOST_REGISTRY_CAPACITY = 256,
  TS_HOST_MAX_VALUE_DEPTH = 64,
  TS_HOST_MAX_VALUE_NODES = 65536,
  TS_HOST_MAX_STRING_BYTES = 4 * 1024 * 1024,
};

typedef struct ts_host_arg_view_state_s {
  mem_pool_t *arena;
  size_t nodes;
  size_t bytes;
} ts_host_arg_view_state_t;

static exprtk_value_t ts_host_registry_null_value(void) {
  exprtk_value_t value = {0};
  value.type = EXPRTK_VAL_NULL;
  return value;
}

static turbo_script_status_t ts_host_registry_vec_status(stl_status status) {
  if (status == STL_OK) return TURBO_SCRIPT_STATUS_OK;
  if (status == STL_OUT_OF_MEMORY) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status == STL_CAPACITY_EXCEEDED) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  return TURBO_SCRIPT_STATUS_INVALID_STATE;
}

static turbo_script_status_t ts_host_registry_charge(ts_host_arg_view_state_t *state,
                                                     size_t count, size_t element_size) {
  size_t amount;
  if (count != 0 && element_size > SIZE_MAX / count)
    return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  amount = count * element_size;
  if (state->bytes > TS_HOST_MAX_STRING_BYTES ||
      amount > TS_HOST_MAX_STRING_BYTES - state->bytes)
    return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->bytes += amount;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_registry_validate_name(turbo_script_string_view_t name) {
  if (!name.data && name.size != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (name.size == 0) return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  if (name.size > TS_HOST_MAX_STRING_BYTES) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  if (memchr(name.data, '\0', name.size) != NULL)
    return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  if (!vstr_utf8_valid(vstr_from_buf(name.data, name.size)))
    return TURBO_SCRIPT_STATUS_INVALID_UTF8;
  return TURBO_SCRIPT_STATUS_OK;
}

static int ts_host_registry_name_equal(const ts_host_function_entry_t *entry,
                                       turbo_script_string_view_t name) {
  size_t entry_size = tstr_len(entry->name);
  return entry_size == name.size && memcmp(entry->name, name.data, name.size) == 0;
}

static turbo_script_status_t ts_host_registry_begin_api(turbo_script_ctx_t *ctx,
                                                        turbo_script_result_t *result) {
  turbo_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_result_check_context(result, ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  return ts_host_result_reset_checked(result);
}

static turbo_script_status_t ts_host_registry_fail(turbo_script_result_t *result,
                                                   turbo_script_status_t status,
                                                   const char *message) {
  turbo_script_error_info_t error = {
      .struct_size = sizeof(error),
      .status = status,
      .phase = TURBO_SCRIPT_ERROR_PHASE_NONE,
      .message = {message, strlen(message)},
  };
  (void)ts_host_result_set_error(result, &error, TS_HOST_MAX_STRING_BYTES);
  return status;
}

static turbo_script_status_t ts_host_registry_validate_descriptor(
    const turbo_script_host_function_descriptor_t *descriptor,
    turbo_script_host_function_t callback) {
  turbo_script_status_t status;
  if (!descriptor || descriptor->struct_size != sizeof(*descriptor) || !callback ||
      descriptor->min_arity > descriptor->max_arity || descriptor->reserved0 != 0)
    return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  for (size_t i = 0; i < sizeof(descriptor->reserved) / sizeof(descriptor->reserved[0]); ++i) {
    if (descriptor->reserved[i] != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  }
  status = ts_host_registry_validate_name(descriptor->name);
  return status;
}

turbo_script_status_t ts_host_registry_init(turbo_script_ctx_t *ctx) {
  stl_status status;
  if (!ctx) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = vec_init_bytes(&ctx->host_functions, sizeof(ts_host_function_entry_t),
                          alignof(ts_host_function_entry_t), TS_HOST_REGISTRY_CAPACITY);
  if (status != STL_OK) return ts_host_registry_vec_status(status);
  ctx->active_host_modules = 0;
  ctx->host_callback_depth = 0;
  ctx->next_instance_generation = 1;
  return TURBO_SCRIPT_STATUS_OK;
}

void ts_host_registry_destroy(turbo_script_ctx_t *ctx) {
  if (!ctx || !ctx->host_functions.initialized) return;
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    ts_host_function_entry_t *entry =
        (ts_host_function_entry_t *)vec_at(&ctx->host_functions, i);
    if (entry) tstr_freep(&entry->name);
  }
  vec_destroy(&ctx->host_functions);
  ctx->active_host_modules = 0;
  ctx->host_callback_depth = 0;
}

turbo_script_status_t ts_host_check_owner_thread(const turbo_script_ctx_t *ctx) {
  return ts_host_context_check_thread(ctx);
}

turbo_script_status_t ts_host_registry_acquire_module(turbo_script_ctx_t *ctx) {
  turbo_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules == SIZE_MAX) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  ctx->active_host_modules++;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_registry_release_module(turbo_script_ctx_t *ctx) {
  turbo_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules == 0) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  ctx->active_host_modules--;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_registry_find_slot(const turbo_script_ctx_t *ctx,
                                                 turbo_script_string_view_t name,
                                                 size_t *out_slot) {
  turbo_script_status_t status;
  status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_slot) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_slot = SIZE_MAX;
  status = ts_host_registry_validate_name(name);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    const ts_host_function_entry_t *entry =
        (const ts_host_function_entry_t *)vec_at_const(&ctx->host_functions, i);
    if (entry && ts_host_registry_name_equal(entry, name)) {
      *out_slot = i;
      return TURBO_SCRIPT_STATUS_OK;
    }
  }
  return TURBO_SCRIPT_STATUS_NOT_FOUND;
}

turbo_script_status_t ts_host_registry_get_slot(
    const turbo_script_ctx_t *ctx, size_t slot,
    const ts_host_function_entry_t **out_entry) {
  turbo_script_status_t status;
  status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_entry) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_entry = NULL;
  if (slot >= vec_size(&ctx->host_functions)) return TURBO_SCRIPT_STATUS_NOT_FOUND;
  *out_entry = (const ts_host_function_entry_t *)vec_at_const(&ctx->host_functions, slot);
  return *out_entry ? TURBO_SCRIPT_STATUS_OK : TURBO_SCRIPT_STATUS_INVALID_STATE;
}

turbo_script_status_t turbo_script_context_register_host_function(
    turbo_script_ctx_t *ctx, const turbo_script_host_function_descriptor_t *descriptor,
    turbo_script_host_function_t callback, void *user_data, turbo_script_result_t *result) {
  ts_host_function_entry_t entry = {0};
  turbo_script_status_t status = ts_host_registry_begin_api(ctx, result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules != 0)
    return ts_host_registry_fail(result, TURBO_SCRIPT_STATUS_INVALID_STATE,
                                 "host registry is frozen while modules are active");
  status = ts_host_registry_validate_descriptor(descriptor, callback);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "invalid host function descriptor");
  status = ts_host_registry_find_slot(ctx, descriptor->name, &(size_t){0});
  if (status == TURBO_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, TURBO_SCRIPT_STATUS_INVALID_STATE,
                                 "host function name is already registered");
  if (status != TURBO_SCRIPT_STATUS_NOT_FOUND)
    return ts_host_registry_fail(result, status, "failed to search host registry");
  if (vec_size(&ctx->host_functions) >= TS_HOST_REGISTRY_CAPACITY)
    return ts_host_registry_fail(result, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED,
                                 "host registry capacity exceeded");

  entry.ctx = ctx;
  entry.name = tstr_dup_len(descriptor->name.data, descriptor->name.size);
  if (!entry.name)
    return ts_host_registry_fail(result, TURBO_SCRIPT_STATUS_OUT_OF_MEMORY,
                                 "failed to copy host function name");
  entry.min_arity = descriptor->min_arity;
  entry.max_arity = descriptor->max_arity;
  entry.callback = callback;
  entry.user_data = user_data;
  status = ts_host_registry_vec_status(vec_push(&ctx->host_functions, &entry));
  if (status != TURBO_SCRIPT_STATUS_OK) {
    tstr_free(entry.name);
    return ts_host_registry_fail(result, status, "failed to append host registry slot");
  }
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t turbo_script_context_unregister_host_function(
    turbo_script_ctx_t *ctx, turbo_script_string_view_t name,
    turbo_script_result_t *result) {
  ts_host_function_entry_t removed = {0};
  size_t slot;
  turbo_script_status_t status = ts_host_registry_begin_api(ctx, result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (ctx->active_host_modules != 0)
    return ts_host_registry_fail(result, TURBO_SCRIPT_STATUS_INVALID_STATE,
                                 "host registry is frozen while modules are active");
  status = ts_host_registry_validate_name(name);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "invalid host function name");
  status = ts_host_registry_find_slot(ctx, name, &slot);
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "host function is not registered");
  status = ts_host_registry_vec_status(vec_erase(&ctx->host_functions, slot, &removed));
  if (status != TURBO_SCRIPT_STATUS_OK)
    return ts_host_registry_fail(result, status, "failed to remove host registry slot");
  tstr_free(removed.name);
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_arg_view_from_exprtk(
    ts_host_arg_view_state_t *state, const exprtk_value_t *source,
    turbo_script_value_view_t *target, size_t depth) {
  turbo_script_status_t status = TURBO_SCRIPT_STATUS_OK;
  if (!source || !target) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (depth > TS_HOST_MAX_VALUE_DEPTH || state->nodes == TS_HOST_MAX_VALUE_NODES)
    return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->nodes++;
  memset(target, 0, sizeof(*target));

  switch (source->type) {
  case EXPRTK_VAL_NULL:
    target->kind = TURBO_SCRIPT_VALUE_NULL;
    return TURBO_SCRIPT_STATUS_OK;
  case EXPRTK_VAL_BOOL:
    if (source->data.boolean != 0 && source->data.boolean != 1)
      return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
    target->kind = TURBO_SCRIPT_VALUE_BOOL;
    target->as.boolean = (uint8_t)source->data.boolean;
    return TURBO_SCRIPT_STATUS_OK;
  case EXPRTK_VAL_INTEGER:
    target->kind = TURBO_SCRIPT_VALUE_INT64;
    target->as.integer = source->data.integer;
    return TURBO_SCRIPT_STATUS_OK;
  case EXPRTK_VAL_NUMBER:
    target->kind = TURBO_SCRIPT_VALUE_NUMBER;
    target->as.number = source->data.number;
    return TURBO_SCRIPT_STATUS_OK;
  case EXPRTK_VAL_STRING:
    if (!source->data.string.data && source->data.string.len != 0)
      return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
    status = ts_host_registry_charge(state, source->data.string.len, 1);
    if (status != TURBO_SCRIPT_STATUS_OK) return status;
    if (!vstr_utf8_valid(source->data.string)) return TURBO_SCRIPT_STATUS_INVALID_UTF8;
    target->kind = TURBO_SCRIPT_VALUE_STRING;
    target->as.string.data = source->data.string.data;
    target->as.string.size = source->data.string.len;
    return TURBO_SCRIPT_STATUS_OK;
  case EXPRTK_VAL_VECTOR: {
    turbo_script_value_view_t *items = NULL;
    if (!source->data.vector.data && source->data.vector.size != 0)
      return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (source->data.vector.size > TS_HOST_MAX_VALUE_NODES - state->nodes)
      return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_registry_charge(state, source->data.vector.size, sizeof(*items));
    if (status != TURBO_SCRIPT_STATUS_OK) return status;
    if (source->data.vector.size != 0) {
      items = (turbo_script_value_view_t *)mem_alloc_array(
          state->arena, sizeof(*items), source->data.vector.size);
      if (!items) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->data.vector.size; ++i) {
        memset(&items[i], 0, sizeof(items[i]));
        items[i].kind = TURBO_SCRIPT_VALUE_NUMBER;
        items[i].as.number = source->data.vector.data[i];
        state->nodes++;
      }
    }
    target->kind = TURBO_SCRIPT_VALUE_ARRAY;
    target->as.array.items = items;
    target->as.array.count = source->data.vector.size;
    return TURBO_SCRIPT_STATUS_OK;
  }
  case EXPRTK_VAL_LIST: {
    turbo_script_value_view_t *items = NULL;
    if (!source->data.list.items && source->data.list.count != 0)
      return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (source->data.list.count > TS_HOST_MAX_VALUE_NODES - state->nodes)
      return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_registry_charge(state, source->data.list.count, sizeof(*items));
    if (status != TURBO_SCRIPT_STATUS_OK) return status;
    if (source->data.list.count != 0) {
      items = (turbo_script_value_view_t *)mem_alloc_array(
          state->arena, sizeof(*items), source->data.list.count);
      if (!items) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->data.list.count; ++i) {
        status = ts_host_arg_view_from_exprtk(state, &source->data.list.items[i], &items[i],
                                              depth + 1);
        if (status != TURBO_SCRIPT_STATUS_OK) return status;
      }
    }
    target->kind = TURBO_SCRIPT_VALUE_ARRAY;
    target->as.array.items = items;
    target->as.array.count = source->data.list.count;
    return TURBO_SCRIPT_STATUS_OK;
  }
  case EXPRTK_VAL_MAP:
  case EXPRTK_VAL_OBJECT: {
    size_t count = exprtk_map_count(source);
    turbo_script_record_entry_view_t *entries = NULL;
    exprtk_map_iter_t iterator;
    const char *key = NULL;
    exprtk_value_t child;
    size_t index = 0;
    if (count > TS_HOST_MAX_VALUE_NODES - state->nodes)
      return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_registry_charge(state, count, sizeof(*entries));
    if (status != TURBO_SCRIPT_STATUS_OK) return status;
    if (count != 0) {
      entries = (turbo_script_record_entry_view_t *)mem_alloc_array(
          state->arena, sizeof(*entries), count);
      if (!entries) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
    }
    iterator = exprtk_map_iter_begin(source);
    while (index < count && exprtk_map_iter_next(&iterator, &key, &child)) {
      size_t key_size;
      if (!key) return TURBO_SCRIPT_STATUS_INVALID_STATE;
      key_size = strlen(key);
      if (key_size == 0) return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
      status = ts_host_registry_charge(state, key_size, 1);
      if (status != TURBO_SCRIPT_STATUS_OK) return status;
      if (!vstr_utf8_valid(vstr_from_buf(key, key_size)))
        return TURBO_SCRIPT_STATUS_INVALID_UTF8;
      entries[index].key.data = key;
      entries[index].key.size = key_size;
      status = ts_host_arg_view_from_exprtk(state, &child, &entries[index].value, depth + 1);
      if (status != TURBO_SCRIPT_STATUS_OK) return status;
      index++;
    }
    if (index != count) return TURBO_SCRIPT_STATUS_INVALID_STATE;
    target->kind = TURBO_SCRIPT_VALUE_RECORD;
    target->as.record.entries = entries;
    target->as.record.count = count;
    return TURBO_SCRIPT_STATUS_OK;
  }
  default:
    return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  }
}

static const char *ts_host_status_name(turbo_script_status_t status) {
  switch (status) {
  case TURBO_SCRIPT_STATUS_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case TURBO_SCRIPT_STATUS_OUT_OF_MEMORY:
    return "OUT_OF_MEMORY";
  case TURBO_SCRIPT_STATUS_WRONG_THREAD:
    return "WRONG_THREAD";
  case TURBO_SCRIPT_STATUS_INVALID_STATE:
    return "INVALID_STATE";
  case TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED:
    return "LIMIT_EXCEEDED";
  case TURBO_SCRIPT_STATUS_INVALID_UTF8:
    return "INVALID_UTF8";
  case TURBO_SCRIPT_STATUS_HOST_ERROR:
    return "HOST_ERROR";
  default:
    return "HOST_ERROR";
  }
}

static void ts_host_runtime_throw(exprtk_env_t *runtime_ctx, turbo_script_status_t status,
                                  int32_t cause_code, const char *message,
                                  const char *function_name) {
  exprtk_value_t error_value = ts_host_registry_null_value();
  const char *status_name = ts_host_status_name(status);
  if (!message) message = "host callback failed";
  if (!function_name) function_name = "<host>";
  if (cause_code != 0) {
    (void)snprintf(runtime_ctx->error_msg, sizeof(runtime_ctx->error_msg),
                   "%s: %s (function=%s, cause=%d)", status_name, message,
                   function_name, cause_code);
  } else {
    (void)snprintf(runtime_ctx->error_msg, sizeof(runtime_ctx->error_msg),
                   "%s: %s (function=%s)", status_name, message, function_name);
  }
  exprtk_value_destroy(&runtime_ctx->error_value);
  if (exprtk_value_copy_to_env(
          exprtk_val_str(vstr_from_cstr(runtime_ctx->error_msg)), runtime_ctx,
          &error_value) == 0)
    runtime_ctx->error_value = error_value;
  runtime_ctx->flow = exprtk_FLOW_THROW;
}

turbo_script_status_t turbo_script_host_result_set_value(
    turbo_script_host_result_builder_t *builder,
    const turbo_script_value_view_t *value) {
  ts_host_value_limits_t limits = {
      TS_HOST_MAX_VALUE_DEPTH,
      TS_HOST_MAX_VALUE_NODES,
      TS_HOST_MAX_STRING_BYTES,
  };
  if (!builder) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (builder->state != TS_HOST_BUILDER_EMPTY)
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  builder->state = TS_HOST_BUILDER_VALUE;
  builder->terminal_status = ts_host_value_from_view(builder->ctx, value, &limits,
                                                     &builder->value);
  return builder->terminal_status;
}

turbo_script_status_t turbo_script_host_result_set_error(
    turbo_script_host_result_builder_t *builder, int32_t cause_code,
    turbo_script_string_view_t message) {
  if (!builder) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (builder->state != TS_HOST_BUILDER_EMPTY)
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  builder->state = TS_HOST_BUILDER_ERROR;
  builder->cause_code = cause_code;
  if (!message.data && message.size != 0) {
    builder->terminal_status = TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
    return builder->terminal_status;
  }
  if (message.size > TS_HOST_MAX_STRING_BYTES) {
    builder->terminal_status = TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    return builder->terminal_status;
  }
  if (!vstr_utf8_valid(vstr_from_buf(message.data, message.size))) {
    builder->terminal_status = TURBO_SCRIPT_STATUS_INVALID_UTF8;
    return builder->terminal_status;
  }
  builder->error_message = tstr_dup_len(message.data ? message.data : "", message.size);
  if (!builder->error_message) {
    builder->terminal_status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
    return builder->terminal_status;
  }
  builder->terminal_status = TURBO_SCRIPT_STATUS_OK;
  return TURBO_SCRIPT_STATUS_OK;
}

static exprtk_value_t ts_host_registry_callback_adapter(
    size_t arg_count, exprtk_value_t *args, exprtk_env_t *runtime_ctx,
    void *user_data) {
  const ts_host_function_entry_t *entry = (const ts_host_function_entry_t *)user_data;
  turbo_script_host_result_builder_t builder = {0};
  ts_host_arg_view_state_t view_state = {0};
  turbo_script_value_view_t *views = NULL;
  mem_pool_t view_arena;
  turbo_script_status_t status;
  exprtk_value_t result = ts_host_registry_null_value();

  if (!entry || !entry->ctx || !runtime_ctx) return result;
  status = ts_host_check_owner_thread(entry->ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) {
    ts_host_runtime_throw(runtime_ctx, status, 0, "host callback ran outside its owner thread",
                          entry->name);
    return result;
  }
  if (arg_count < (size_t)entry->min_arity || arg_count > (size_t)entry->max_arity) {
    ts_host_runtime_throw(runtime_ctx, TURBO_SCRIPT_STATUS_HOST_ERROR, 0,
                          "host function arity mismatch", entry->name);
    return result;
  }
  if (arg_count != 0 && !args) {
    ts_host_runtime_throw(runtime_ctx, TURBO_SCRIPT_STATUS_INVALID_ARGUMENT, 0,
                          "host arguments have no backing storage", entry->name);
    return result;
  }
  if (mem_init(&view_arena, 0) != 0) {
    ts_host_runtime_throw(runtime_ctx, TURBO_SCRIPT_STATUS_OUT_OF_MEMORY, 0,
                          "failed to initialize host argument arena", entry->name);
    return result;
  }
  view_state.arena = &view_arena;
  if (arg_count != 0) {
    status = ts_host_registry_charge(&view_state, arg_count, sizeof(*views));
    if (status == TURBO_SCRIPT_STATUS_OK) {
      views = (turbo_script_value_view_t *)mem_alloc_array(&view_arena, sizeof(*views),
                                                           arg_count);
      if (!views) status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
    }
    for (size_t i = 0; status == TURBO_SCRIPT_STATUS_OK && i < arg_count; ++i)
      status = ts_host_arg_view_from_exprtk(&view_state, &args[i], &views[i], 1);
    if (status != TURBO_SCRIPT_STATUS_OK) {
      ts_host_runtime_throw(runtime_ctx, status, 0, "failed to adapt host arguments",
                            entry->name);
      mem_destroy(&view_arena);
      return result;
    }
  }

  builder.ctx = entry->ctx;
  builder.state = TS_HOST_BUILDER_EMPTY;
  builder.terminal_status = TURBO_SCRIPT_STATUS_OK;
  builder.value = ts_host_registry_null_value();
  if (entry->ctx->host_callback_depth == SIZE_MAX) {
    ts_host_runtime_throw(runtime_ctx, TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED, 0,
                          "host callback depth overflow", entry->name);
    mem_destroy(&view_arena);
    return result;
  }
  entry->ctx->host_callback_depth++;
  status = entry->callback(entry->user_data, views, arg_count, &builder);
  entry->ctx->host_callback_depth--;

  if (status == TURBO_SCRIPT_STATUS_OK && builder.state == TS_HOST_BUILDER_EMPTY) {
    status = TURBO_SCRIPT_STATUS_HOST_ERROR;
    ts_host_runtime_throw(runtime_ctx, status, 0,
                          "callback returned success without a result", entry->name);
  } else if (builder.terminal_status != TURBO_SCRIPT_STATUS_OK) {
    status = builder.terminal_status;
    ts_host_runtime_throw(runtime_ctx, status, builder.cause_code,
                          "host result builder rejected its terminal write", entry->name);
  } else if (status != TURBO_SCRIPT_STATUS_OK) {
    ts_host_runtime_throw(runtime_ctx, status, builder.cause_code,
                          builder.error_message ? builder.error_message : "host callback failed",
                          entry->name);
  } else if (builder.state == TS_HOST_BUILDER_ERROR) {
    status = TURBO_SCRIPT_STATUS_HOST_ERROR;
    ts_host_runtime_throw(runtime_ctx, status, builder.cause_code,
                          builder.error_message ? builder.error_message : "host callback failed",
                          entry->name);
  } else {
    result = builder.value;
    builder.value = ts_host_registry_null_value();
  }

  exprtk_value_destroy(&builder.value);
  tstr_free(builder.error_message);
  mem_destroy(&view_arena);
  return result;
}

static turbo_script_status_t ts_host_registry_bind_runtime_impl(
    turbo_script_ctx_t *ctx, exprtk_env_t *runtime_ctx,
    exprtk_registration_fault_fn should_fail, void *fault_user_data) {
  exprtk_native_registration_t registrations[TS_HOST_REGISTRY_CAPACITY];
  exprtk_registration_status_t registration_status;
  turbo_script_status_t status = ts_host_check_owner_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!runtime_ctx) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (ctx->active_host_modules == 0) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    const ts_host_function_entry_t *entry =
        (const ts_host_function_entry_t *)vec_at_const(&ctx->host_functions, i);
    if (!entry || exprtk_env_has_func(runtime_ctx, entry->name))
      return TURBO_SCRIPT_STATUS_INVALID_STATE;
  }
  for (size_t i = 0; i < vec_size(&ctx->host_functions); ++i) {
    ts_host_function_entry_t *entry =
        (ts_host_function_entry_t *)vec_at(&ctx->host_functions, i);
    if (!entry) return TURBO_SCRIPT_STATUS_INVALID_STATE;
    registrations[i].name = entry->name;
    registrations[i].fn = ts_host_registry_callback_adapter;
    registrations[i].user_data = entry;
  }
  registration_status = exprtk_env_register_funcs_checked_with_fault(
      runtime_ctx, registrations, vec_size(&ctx->host_functions), should_fail,
      fault_user_data);
  if (registration_status == EXPRTK_REGISTRATION_OK)
    return TURBO_SCRIPT_STATUS_OK;
  if (registration_status == EXPRTK_REGISTRATION_OUT_OF_MEMORY)
    return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (registration_status == EXPRTK_REGISTRATION_CONFLICT)
    return TURBO_SCRIPT_STATUS_INVALID_STATE;
  return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
}

turbo_script_status_t ts_host_registry_bind_runtime(turbo_script_ctx_t *ctx,
                                                    exprtk_env_t *runtime_ctx) {
  return ts_host_registry_bind_runtime_impl(ctx, runtime_ctx, NULL, NULL);
}

typedef struct ts_host_registration_fault_s {
  size_t allocation_index;
} ts_host_registration_fault_t;

static int ts_host_registration_should_fail(size_t allocation_index,
                                            void *user_data) {
  const ts_host_registration_fault_t *fault =
      (const ts_host_registration_fault_t *)user_data;
  return allocation_index == fault->allocation_index;
}

turbo_script_status_t ts_host_registry_bind_runtime_test_fault(
    turbo_script_ctx_t *ctx, exprtk_env_t *runtime_ctx,
    size_t allocation_index) {
  ts_host_registration_fault_t fault = {allocation_index};
  return ts_host_registry_bind_runtime_impl(
      ctx, runtime_ctx, ts_host_registration_should_fail, &fault);
}
