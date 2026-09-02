#include "turbo_script_host_internal.h"

#include "../turbo_script_internal.h"
#include "turbo_vstr.h"
#include <rocida/stl.h>

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define TS_HOST_FNV_OFFSET_BASIS UINT64_C(14695981039346656037)
#define TS_HOST_FNV_PRIME UINT64_C(1099511628211)

typedef struct ts_host_validation_state_s {
  const ts_host_value_limits_t *limits;
  size_t nodes;
  size_t bytes;
  hash_set_t active_containers;
} ts_host_validation_state_t;

static turbo_script_value_view_t ts_host_null_view(void);

turbo_script_status_t ts_host_context_check_thread(const turbo_script_ctx_t *ctx) {
  if (!ctx) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (!ts_context_is_owner_thread(ctx)) return TURBO_SCRIPT_STATUS_WRONG_THREAD;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_result_check_thread(const turbo_script_result_t *result) {
  if (!result) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  return ts_host_context_check_thread(result->ctx);
}

static void ts_host_result_clear_unchecked(turbo_script_result_t *result) {
  mem_reset(&result->view_arena);
  result->value = ts_host_null_view();
  memset(&result->error, 0, sizeof(result->error));
  result->has_value = 0;
  result->has_error = 0;
}

static void ts_host_result_contract_violation(void) {
  /* The locked void ABI cannot report WRONG_THREAD. Continuing would either
   * leak retained ownership or free another thread's arena, so terminate. */
  abort();
}

static turbo_script_value_view_t ts_host_null_view(void) {
  turbo_script_value_view_t value = {0};
  value.kind = TURBO_SCRIPT_VALUE_NULL;
  return value;
}

static exprtk_value_t ts_host_exprtk_null(void) {
  exprtk_value_t value = {0};
  value.type = EXPRTK_VAL_NULL;
  return value;
}

static turbo_script_status_t ts_host_stl_status(stl_status status) {
  if (status == STL_OK) return TURBO_SCRIPT_STATUS_OK;
  if (status == STL_OUT_OF_MEMORY) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status == STL_CAPACITY_EXCEEDED) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
}

static turbo_script_status_t ts_host_charge_bytes(ts_host_validation_state_t *state, size_t count,
                                                  size_t element_size) {
  size_t bytes;
  if (count != 0 && element_size > SIZE_MAX / count) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  bytes = count * element_size;
  if (bytes > state->limits->max_bytes - state->bytes) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->bytes += bytes;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_validate_string(ts_host_validation_state_t *state,
                                                     turbo_script_string_view_t value,
                                                     int require_nonempty,
                                                     int reject_embedded_nul) {
  turbo_script_status_t status;
  if (!value.data && value.size != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (require_nonempty && value.size == 0) return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  /* exprtk map keys are C strings, so ABI record keys must have one exact
   * representation after conversion. Ordinary string values remain length-based. */
  if (reject_embedded_nul && memchr(value.data, '\0', value.size) != NULL)
    return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  if (value.size == SIZE_MAX) return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  status = ts_host_charge_bytes(state, value.size + 1, 1);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!vstr_utf8_valid(vstr_from_buf(value.data, value.size)))
    return TURBO_SCRIPT_STATUS_INVALID_UTF8;
  return TURBO_SCRIPT_STATUS_OK;
}

static size_t ts_host_string_hash(const void *key, size_t key_size, void *context) {
  const turbo_script_string_view_t *view = (const turbo_script_string_view_t *)key;
  uint64_t hash = TS_HOST_FNV_OFFSET_BASIS;
  (void)key_size;
  (void)context;
  for (size_t i = 0; i < view->size; ++i) {
    hash ^= (uint8_t)view->data[i];
    hash *= TS_HOST_FNV_PRIME;
  }
  return (size_t)hash;
}

static bool ts_host_string_equal(const void *left, const void *right, size_t key_size,
                                 void *context) {
  const turbo_script_string_view_t *lhs = (const turbo_script_string_view_t *)left;
  const turbo_script_string_view_t *rhs = (const turbo_script_string_view_t *)right;
  (void)key_size;
  (void)context;
  return lhs->size == rhs->size && (lhs->size == 0 || memcmp(lhs->data, rhs->data, lhs->size) == 0);
}

static turbo_script_status_t ts_host_validate_node(ts_host_validation_state_t *state,
                                                   const turbo_script_value_view_t *value,
                                                   size_t depth) {
  turbo_script_status_t status = TURBO_SCRIPT_STATUS_OK;
  const turbo_script_value_view_t *identity = value;
  int active = 0;

  if (!value) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (depth > state->limits->max_depth || state->nodes == state->limits->max_nodes)
    return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
  state->nodes++;
  if (value->reserved != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;

  switch (value->kind) {
  case TURBO_SCRIPT_VALUE_NULL:
  case TURBO_SCRIPT_VALUE_INT64:
  case TURBO_SCRIPT_VALUE_NUMBER:
    return TURBO_SCRIPT_STATUS_OK;
  case TURBO_SCRIPT_VALUE_BOOL:
    return value->as.boolean <= 1 ? TURBO_SCRIPT_STATUS_OK : TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  case TURBO_SCRIPT_VALUE_STRING:
    return ts_host_validate_string(state, value->as.string, 0, 0);
  case TURBO_SCRIPT_VALUE_ARRAY:
    if (!value->as.array.items && value->as.array.count != 0)
      return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (value->as.array.count > state->limits->max_nodes - state->nodes)
      return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_charge_bytes(state, value->as.array.count, sizeof(turbo_script_value_view_t));
    break;
  case TURBO_SCRIPT_VALUE_RECORD:
    if (!value->as.record.entries && value->as.record.count != 0)
      return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
    if (value->as.record.count > state->limits->max_nodes - state->nodes)
      return TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED;
    status = ts_host_charge_bytes(state, value->as.record.count,
                                  sizeof(turbo_script_record_entry_view_t));
    break;
  default:
    return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  }
  if (status != TURBO_SCRIPT_STATUS_OK) return status;

  if (hash_set_contains(&state->active_containers, &identity))
    return TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
  status = ts_host_stl_status(hash_set_add(&state->active_containers, &identity));
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  active = 1;

  if (value->kind == TURBO_SCRIPT_VALUE_ARRAY) {
    for (size_t i = 0; i < value->as.array.count; ++i) {
      status = ts_host_validate_node(state, &value->as.array.items[i], depth + 1);
      if (status != TURBO_SCRIPT_STATUS_OK) break;
    }
  } else {
    hash_set_t keys = {0};
    stl_status init_status = STL_OK;
    if (value->as.record.count != 0) {
      init_status = hash_set_init_bytes(&keys, sizeof(turbo_script_string_view_t),
                                        alignof(turbo_script_string_view_t), value->as.record.count,
                                        ts_host_string_hash, ts_host_string_equal, NULL);
      if (init_status != STL_OK) status = ts_host_stl_status(init_status);
    }
    for (size_t i = 0; status == TURBO_SCRIPT_STATUS_OK && i < value->as.record.count; ++i) {
      const turbo_script_record_entry_view_t *entry = &value->as.record.entries[i];
      status = ts_host_validate_string(state, entry->key, 1, 1);
      if (status != TURBO_SCRIPT_STATUS_OK) break;
      if (hash_set_contains(&keys, &entry->key)) {
        status = TURBO_SCRIPT_STATUS_DUPLICATE_RECORD_KEY;
        break;
      }
      init_status = hash_set_add(&keys, &entry->key);
      if (init_status != STL_OK) {
        status = ts_host_stl_status(init_status);
        break;
      }
      status = ts_host_validate_node(state, &entry->value, depth + 1);
    }
    hash_set_destroy(&keys);
  }

  if (active && hash_set_remove(&state->active_containers, &identity) != STL_OK &&
      status == TURBO_SCRIPT_STATUS_OK)
    status = TURBO_SCRIPT_STATUS_INVALID_STATE;
  return status;
}

turbo_script_status_t ts_host_validate_value_view(const turbo_script_value_view_t *value,
                                                  const ts_host_value_limits_t *limits) {
  ts_host_validation_state_t state = {0};
  stl_status init_status;
  turbo_script_status_t status;
  if (!value || !limits || limits->max_depth == 0 || limits->max_nodes == 0 ||
      limits->max_bytes == 0)
    return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;

  state.limits = limits;
  init_status =
      hash_set_init_bytes(&state.active_containers, sizeof(const turbo_script_value_view_t *),
                          alignof(const turbo_script_value_view_t *), limits->max_nodes, hash_bytes,
                          hash_key_equal, NULL);
  if (init_status != STL_OK) return ts_host_stl_status(init_status);
  status = ts_host_validate_node(&state, value, 1);
  hash_set_destroy(&state.active_containers);
  return status;
}

static char *ts_host_copy_string(mem_pool_t *arena, turbo_script_string_view_t source) {
  char *copy = (char *)mem_alloc(arena, source.size + 1);
  if (!copy) return NULL;
  if (source.size != 0) memcpy(copy, source.data, source.size);
  copy[source.size] = '\0';
  return copy;
}

static turbo_script_status_t ts_host_copy_view(mem_pool_t *arena,
                                               const turbo_script_value_view_t *source,
                                               turbo_script_value_view_t *target) {
  turbo_script_value_view_t copy = *source;
  if (source->kind == TURBO_SCRIPT_VALUE_STRING) {
    copy.as.string.data = ts_host_copy_string(arena, source->as.string);
    if (!copy.as.string.data) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  } else if (source->kind == TURBO_SCRIPT_VALUE_ARRAY) {
    turbo_script_value_view_t *items = NULL;
    if (source->as.array.count != 0) {
      items = (turbo_script_value_view_t *)mem_alloc_array(arena, sizeof(*items),
                                                           source->as.array.count);
      if (!items) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->as.array.count; ++i) {
        turbo_script_status_t status =
            ts_host_copy_view(arena, &source->as.array.items[i], &items[i]);
        if (status != TURBO_SCRIPT_STATUS_OK) return status;
      }
    }
    copy.as.array.items = items;
  } else if (source->kind == TURBO_SCRIPT_VALUE_RECORD) {
    turbo_script_record_entry_view_t *entries = NULL;
    if (source->as.record.count != 0) {
      entries = (turbo_script_record_entry_view_t *)mem_alloc_array(arena, sizeof(*entries),
                                                                    source->as.record.count);
      if (!entries) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      for (size_t i = 0; i < source->as.record.count; ++i) {
        entries[i].key = source->as.record.entries[i].key;
        entries[i].key.data = ts_host_copy_string(arena, source->as.record.entries[i].key);
        if (!entries[i].key.data) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
        {
          turbo_script_status_t status =
              ts_host_copy_view(arena, &source->as.record.entries[i].value, &entries[i].value);
          if (status != TURBO_SCRIPT_STATUS_OK) return status;
        }
      }
    }
    copy.as.record.entries = entries;
  }
  *target = copy;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_build_exprtk_value(mem_pool_t *key_arena,
                                                        const turbo_script_value_view_t *source,
                                                        exprtk_value_t *target) {
  exprtk_value_t value = ts_host_exprtk_null();
  turbo_script_status_t status = TURBO_SCRIPT_STATUS_OK;

  switch (source->kind) {
  case TURBO_SCRIPT_VALUE_NULL:
    break;
  case TURBO_SCRIPT_VALUE_BOOL:
    value = exprtk_val_bool(source->as.boolean);
    break;
  case TURBO_SCRIPT_VALUE_INT64:
    value = exprtk_val_int(source->as.integer);
    break;
  case TURBO_SCRIPT_VALUE_NUMBER:
    value = exprtk_val_num(source->as.number);
    break;
  case TURBO_SCRIPT_VALUE_STRING:
    value = exprtk_val_str(vstr_from_buf(source->as.string.data, source->as.string.size));
    break;
  case TURBO_SCRIPT_VALUE_ARRAY:
    value = exprtk_val_list_empty();
    for (size_t i = 0; i < source->as.array.count; ++i) {
      exprtk_value_t child = ts_host_exprtk_null();
      status = ts_host_build_exprtk_value(key_arena, &source->as.array.items[i], &child);
      if (status == TURBO_SCRIPT_STATUS_OK && exprtk_list_push(&value, child) != 0)
        status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      exprtk_value_destroy(&child);
      if (status != TURBO_SCRIPT_STATUS_OK) break;
    }
    break;
  case TURBO_SCRIPT_VALUE_RECORD:
    value = exprtk_val_map();
    if (!value.data.map.htab) {
      status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      break;
    }
    for (size_t i = 0; i < source->as.record.count; ++i) {
      const turbo_script_record_entry_view_t *entry = &source->as.record.entries[i];
      exprtk_value_t child = ts_host_exprtk_null();
      char *key = ts_host_copy_string(key_arena, entry->key);
      if (!key) {
        status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
        break;
      }
      status = ts_host_build_exprtk_value(key_arena, &entry->value, &child);
      if (status == TURBO_SCRIPT_STATUS_OK && exprtk_map_set(&value, key, child) != 0)
        status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
      exprtk_value_destroy(&child);
      if (status != TURBO_SCRIPT_STATUS_OK) break;
    }
    break;
  default:
    status = TURBO_SCRIPT_STATUS_VALIDATION_ERROR;
    break;
  }

  if (status != TURBO_SCRIPT_STATUS_OK) {
    exprtk_value_destroy(&value);
    return status;
  }
  *target = value;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_value_from_view(turbo_script_ctx_t *target_ctx,
                                              const turbo_script_value_view_t *value,
                                              const ts_host_value_limits_t *limits,
                                              exprtk_value_t *out_value) {
  exprtk_value_t temporary = ts_host_exprtk_null();
  mem_pool_t key_arena;
  turbo_script_status_t status;
  if (!out_value) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_value = ts_host_exprtk_null();
  if (!target_ctx) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_context_check_thread(target_ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;

  status = ts_host_validate_value_view(value, limits);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (mem_init(&key_arena, 0) != 0) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  status = ts_host_build_exprtk_value(&key_arena, value, &temporary);
  if (status == TURBO_SCRIPT_STATUS_OK &&
      exprtk_value_copy_to_env(temporary, &target_ctx->env, out_value) != 0)
    status = TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (status != TURBO_SCRIPT_STATUS_OK) *out_value = ts_host_exprtk_null();
  exprtk_value_destroy(&temporary);
  mem_destroy(&key_arena);
  return status;
}

turbo_script_status_t turbo_script_result_create(turbo_script_ctx_t *ctx,
                                                 turbo_script_result_t **out_result) {
  turbo_script_result_t *result;
  turbo_script_status_t status;
  if (!out_result) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  *out_result = NULL;
  if (!ctx) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  status = ts_host_context_check_thread(ctx);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  result = (turbo_script_result_t *)calloc(1, sizeof(*result));
  if (!result) return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  if (mem_init(&result->view_arena, 0) != 0) {
    free(result);
    return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  }
  result->ctx = ctx;
  result->value = ts_host_null_view();
  ts_context_retain(ctx);
  *out_result = result;
  return TURBO_SCRIPT_STATUS_OK;
}

void turbo_script_result_reset(turbo_script_result_t *result) {
  turbo_script_status_t status = ts_host_result_reset_checked(result);
  if (status == TURBO_SCRIPT_STATUS_WRONG_THREAD) ts_host_result_contract_violation();
}

void turbo_script_result_destroy(turbo_script_result_t *result) {
  turbo_script_status_t status = ts_host_result_destroy_checked(result);
  if (status == TURBO_SCRIPT_STATUS_WRONG_THREAD) ts_host_result_contract_violation();
}

turbo_script_status_t ts_host_result_reset_checked(turbo_script_result_t *result) {
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_result_destroy_checked(turbo_script_result_t *result) {
  turbo_script_ctx_t *ctx;
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  ctx = result->ctx;
  mem_destroy(&result->view_arena);
  memset(result, 0, sizeof(*result));
  free(result);
  ts_context_release(ctx);
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t turbo_script_result_get_value(const turbo_script_result_t *result,
                                                    turbo_script_value_view_t *out_value) {
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_value) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (!result->has_value) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  *out_value = result->value;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t turbo_script_result_get_error(const turbo_script_result_t *result,
                                                    turbo_script_error_info_t *out_error) {
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!out_error) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (!result->has_error) return TURBO_SCRIPT_STATUS_INVALID_STATE;
  *out_error = result->error;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_result_store_view(turbo_script_result_t *result,
                                                const turbo_script_value_view_t *value,
                                                const ts_host_value_limits_t *limits) {
  turbo_script_value_view_t copy = ts_host_null_view();
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  status = ts_host_validate_value_view(value, limits);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  status = ts_host_copy_view(&result->view_arena, value, &copy);
  if (status != TURBO_SCRIPT_STATUS_OK) {
    ts_host_result_clear_unchecked(result);
    return status;
  }
  result->value = copy;
  result->has_value = 1;
  return TURBO_SCRIPT_STATUS_OK;
}

static turbo_script_status_t ts_host_validate_error_info(const turbo_script_error_info_t *error,
                                                         size_t max_bytes) {
  ts_host_value_limits_t limits = {1, 1, max_bytes};
  ts_host_validation_state_t state = {.limits = &limits};
  turbo_script_status_t status;

  if (!error || error->struct_size < sizeof(*error) || max_bytes == 0 ||
      error->status < TURBO_SCRIPT_STATUS_INVALID_ARGUMENT ||
      error->status > TURBO_SCRIPT_STATUS_INTERRUPTED ||
      error->phase > TURBO_SCRIPT_ERROR_PHASE_INTERRUPT)
    return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  for (size_t i = 0; i < sizeof(error->reserved) / sizeof(error->reserved[0]); ++i) {
    if (error->reserved[i] != 0) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  }

  status = ts_host_validate_string(&state, error->module_name, 0, 0);
  if (status == TURBO_SCRIPT_STATUS_OK)
    status = ts_host_validate_string(&state, error->function_name, 0, 0);
  if (status == TURBO_SCRIPT_STATUS_OK)
    status = ts_host_validate_string(&state, error->message, 0, 0);
  return status;
}

turbo_script_status_t ts_host_result_set_error(turbo_script_result_t *result,
                                               const turbo_script_error_info_t *error,
                                               size_t max_bytes) {
  turbo_script_error_info_t copy;
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  ts_host_result_clear_unchecked(result);
  status = ts_host_validate_error_info(error, max_bytes);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;

  copy = *error;
  copy.module_name.data = ts_host_copy_string(&result->view_arena, error->module_name);
  copy.function_name.data = ts_host_copy_string(&result->view_arena, error->function_name);
  copy.message.data = ts_host_copy_string(&result->view_arena, error->message);
  if (!copy.module_name.data || !copy.function_name.data || !copy.message.data) {
    ts_host_result_clear_unchecked(result);
    return TURBO_SCRIPT_STATUS_OUT_OF_MEMORY;
  }
  result->error = copy;
  result->has_error = 1;
  return TURBO_SCRIPT_STATUS_OK;
}

turbo_script_status_t ts_host_result_check_context(const turbo_script_result_t *result,
                                                   const turbo_script_ctx_t *ctx) {
  turbo_script_status_t status = ts_host_result_check_thread(result);
  if (status != TURBO_SCRIPT_STATUS_OK) return status;
  if (!ctx) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  if (result->ctx != ctx) return TURBO_SCRIPT_STATUS_CONTEXT_MISMATCH;
  return TURBO_SCRIPT_STATUS_OK;
}
