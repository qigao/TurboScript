#include "ts_plugin_databind.h"

#include <data_bind_binding_plan.h>
#include <data_bind_native.h>
#include <data_bind_plugin_catalog.h>

#include <cmeta/data.h>
#include <cmeta/function.h>
#include <cserde/reader.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TS_PLUGIN_DATABIND_WORKSPACE_BYTES = 16384u };

typedef struct ts_plugin_databind_binding_s {
  const salts_plugin_export *entry;
  DataBindPluginOperationBinding operation;
  DataBindServiceNativeBinding native;
  DataBindBindingPlan *plan;
  salts_plugin_function_invoke_fn invoke;
  void *context;
  size_t request_param_index;
  size_t response_param_index;
} ts_plugin_databind_binding_t;

typedef struct ts_plugin_databind_token_reader_s {
  cserde_token token;
  int emitted;
} ts_plugin_databind_token_reader_t;

typedef struct ts_plugin_databind_provider_s {
  const exprtk_value_t *input;
  exprtk_value_t staged;
  exprtk_value_t result;
  ts_plugin_databind_token_reader_t reader;
} ts_plugin_databind_provider_t;

static int databind_error(
    ts_plugin_error_t *error, ts_plugin_error_code_t code,
    uint32_t native_code, const char *message) {
  if (error) {
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->stage = TS_PLUGIN_STAGE_INITIALIZE;
    error->native_code = native_code;
    if (message)
      snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return code;
}

static cserde_status databind_token_next(void *context, cserde_token *out) {
  ts_plugin_databind_token_reader_t *reader =
      (ts_plugin_databind_token_reader_t *)context;
  if (!reader || !out) return CSERDE_INVALID_ARGUMENT;
  if (reader->emitted) return CSERDE_DONE;
  *out = reader->token;
  reader->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops TS_PLUGIN_DATABIND_READER_OPS = {
    offsetof(cserde_reader_ops, next) + sizeof(cserde_reader_next_fn),
    CSERDE_READER_OPS_ABI_VERSION,
    databind_token_next
};

static DataBindStatus databind_project_field(
    void *context,
    const DataBindServiceOperation *operation,
    const DataBindSchemaField *field,
    DataBindBindingDirection direction,
    DataBindBindingAddress *out,
    DataBindError *error) {
  (void)context;
  (void)operation;
  (void)error;
  if (!field || !out || !field->name) return DATA_BIND_ERR_INVALID_ARG;

  *out = (DataBindBindingAddress)DATA_BIND_BINDING_ADDRESS_INIT;
  out->binding_class =
      direction == DATA_BIND_BINDING_INGRESS
          ? DATA_BIND_BINDING_VALUE
          : DATA_BIND_BINDING_RESULT;
  out->space =
      direction == DATA_BIND_BINDING_INGRESS
          ? "turboscript.input"
          : "turboscript.output";
  out->name = field->name;
  out->ordinal = 0u;
  return DATA_BIND_OK;
}

static int databind_input_token(
    const DataBindBindingPlanEntry *entry,
    const exprtk_value_t *value,
    cserde_token *out) {
  const cmeta_data_desc *data;
  const cmeta_data_integer_shape *integer_shape;
  const cmeta_data_float_shape *float_shape;

  if (!entry || !value || !out ||
      !(data = entry->data) || !cmeta_data_desc_valid(data))
    return 0;

  memset(out, 0, sizeof(*out));
  switch (data->kind) {
    case CMETA_DATA_BOOL:
      if (value->type != EXPRTK_VAL_BOOL) return 0;
      out->kind = CSERDE_BOOL;
      out->value.boolean = value->data.boolean != 0;
      return 1;

    case CMETA_DATA_SINT:
      if (value->type != EXPRTK_VAL_INTEGER) return 0;
      integer_shape = (const cmeta_data_integer_shape *)data->shape;
      if (!integer_shape) return 0;
      if (integer_shape->bits < 64u) {
        const int64_t min_value =
            -(INT64_C(1) << (integer_shape->bits - 1u));
        const int64_t max_value =
            (INT64_C(1) << (integer_shape->bits - 1u)) - 1;
        if (value->data.integer < min_value ||
            value->data.integer > max_value)
          return 0;
      }
      out->kind = CSERDE_SINT;
      out->value.sint = value->data.integer;
      return 1;

    case CMETA_DATA_UINT:
      if (value->type != EXPRTK_VAL_INTEGER ||
          value->data.integer < 0)
        return 0;
      integer_shape = (const cmeta_data_integer_shape *)data->shape;
      if (!integer_shape) return 0;
      if (integer_shape->bits < 63u) {
        const uint64_t max_value =
            (UINT64_C(1) << integer_shape->bits) - 1u;
        if ((uint64_t)value->data.integer > max_value) return 0;
      }
      out->kind = CSERDE_UINT;
      out->value.uint = (uint64_t)value->data.integer;
      return 1;

    case CMETA_DATA_FLOAT:
      if (value->type == EXPRTK_VAL_INTEGER) {
        out->kind = CSERDE_FLOAT;
        out->value.floating = (double)value->data.integer;
        return 1;
      }
      if (value->type != EXPRTK_VAL_NUMBER ||
          !isfinite(value->data.number))
        return 0;
      float_shape = (const cmeta_data_float_shape *)data->shape;
      if (!float_shape ||
          (float_shape->bits != 32u && float_shape->bits != 64u))
        return 0;
      out->kind = CSERDE_FLOAT;
      out->value.floating = value->data.number;
      return 1;

    case CMETA_DATA_STRING:
      if (value->type != EXPRTK_VAL_STRING) return 0;
      out->kind = CSERDE_STRING;
      out->value.slice.data =
          (const unsigned char *)value->data.string.data;
      out->value.slice.size = value->data.string.len;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      return 1;

    case CMETA_DATA_BYTES:
      if (value->type != EXPRTK_VAL_BYTES) return 0;
      out->kind = CSERDE_BYTES;
      out->value.slice.data =
          (const unsigned char *)value->data.bytes.data;
      out->value.slice.size = value->data.bytes.len;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      return 1;

    default:
      return 0;
  }
}

static DataBindStatus databind_open_input(
    void *context,
    const DataBindBindingPlanEntry *entry,
    cserde_reader *reader,
    DataBindBindingValueState *state,
    DataBindError *error) {
  ts_plugin_databind_provider_t *provider =
      (ts_plugin_databind_provider_t *)context;
  exprtk_value_t value;

  (void)error;
  if (!provider || !entry || !reader || !state ||
      !provider->input ||
      !exprtk_value_is_object_like(provider->input) ||
      !entry->address.name)
    return DATA_BIND_ERR_INVALID_ARG;

  if (!exprtk_map_has(provider->input, entry->address.name)) {
    *state = DATA_BIND_VALUE_STATE_ABSENT;
    return DATA_BIND_OK;
  }

  value = exprtk_map_get(provider->input, entry->address.name);
  if (value.type == EXPRTK_VAL_NULL) {
    *state = DATA_BIND_VALUE_STATE_NULL;
    return DATA_BIND_OK;
  }

  if (!databind_input_token(entry, &value, &provider->reader.token))
    return DATA_BIND_ERR_TYPE_MISMATCH;
  provider->reader.emitted = 0;
  *state = DATA_BIND_VALUE_STATE_VALUE;

  return cserde_reader_init(
             reader, &TS_PLUGIN_DATABIND_READER_OPS,
             &provider->reader) == CSERDE_OK
             ? DATA_BIND_OK
             : DATA_BIND_ERR_RUNTIME;
}

static int databind_native_to_exprtk(
    const cmeta_data_desc *data, const void *value, size_t value_bytes,
    exprtk_value_t *out) {
  const cmeta_data_integer_shape *integer_shape;
  const cmeta_data_float_shape *float_shape;
  const unsigned char *buffer = NULL;
  size_t buffer_size = 0u;

  if (!data || !value || !out ||
      !cmeta_data_desc_valid(data) || !data->storage_type)
    return 0;

  switch (data->kind) {
    case CMETA_DATA_BOOL: {
      _Bool result = false;
      if (value_bytes < sizeof(result)) return 0;
      memcpy(&result, value, sizeof(result));
      *out = exprtk_val_bool(result ? 1 : 0);
      return 1;
    }

    case CMETA_DATA_SINT:
      integer_shape = (const cmeta_data_integer_shape *)data->shape;
      if (!integer_shape) return 0;
      switch (integer_shape->bits) {
        case 8u: {
          int8_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        case 16u: {
          int16_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        case 32u: {
          int32_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        case 64u: {
          int64_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int(x);
          return 1;
        }
        default:
          return 0;
      }

    case CMETA_DATA_UINT:
      integer_shape = (const cmeta_data_integer_shape *)data->shape;
      if (!integer_shape) return 0;
      switch (integer_shape->bits) {
        case 8u: {
          uint8_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        case 16u: {
          uint16_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        case 32u: {
          uint32_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        case 64u: {
          uint64_t x;
          if (value_bytes < sizeof(x)) return 0;
          memcpy(&x, value, sizeof(x));
          if (x > (uint64_t)INT64_MAX) return 0;
          *out = exprtk_val_int((int64_t)x);
          return 1;
        }
        default:
          return 0;
      }

    case CMETA_DATA_FLOAT:
      float_shape = (const cmeta_data_float_shape *)data->shape;
      if (!float_shape) return 0;
      if (float_shape->bits == 32u) {
        float x;
        if (value_bytes < sizeof(x)) return 0;
        memcpy(&x, value, sizeof(x));
        *out = exprtk_val_num((double)x);
        return 1;
      }
      if (float_shape->bits == 64u) {
        double x;
        if (value_bytes < sizeof(x)) return 0;
        memcpy(&x, value, sizeof(x));
        *out = exprtk_val_num(x);
        return 1;
      }
      return 0;

    case CMETA_DATA_STRING:
    case CMETA_DATA_BYTES:
      if (!data->buffer_ops || !data->buffer_ops->read ||
          data->buffer_ops->read(value, &buffer, &buffer_size) != CMETA_OK)
        return 0;
      if (buffer_size != 0u && !buffer) return 0;
      *out = data->kind == CMETA_DATA_STRING
                 ? exprtk_val_str(vstr_from_buf(
                       (char *)buffer, buffer_size))
                 : exprtk_val_bytes(vstr_from_buf(
                       (char *)buffer, buffer_size));
      return 1;

    default:
      return 0;
  }
}

static DataBindStatus databind_begin_output(
    void *context, DataBindError *error) {
  ts_plugin_databind_provider_t *provider =
      (ts_plugin_databind_provider_t *)context;
  (void)error;
  if (!provider) return DATA_BIND_ERR_INVALID_ARG;
  exprtk_value_destroy(&provider->staged);
  provider->staged = exprtk_val_map();
  return DATA_BIND_OK;
}

static DataBindStatus databind_write_output(
    void *context,
    const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state,
    const void *value,
    size_t value_bytes,
    DataBindError *error) {
  ts_plugin_databind_provider_t *provider =
      (ts_plugin_databind_provider_t *)context;
  exprtk_value_t script_value = {0};

  (void)error;
  if (!provider || !entry || !entry->address.name ||
      !exprtk_value_is_object_like(&provider->staged))
    return DATA_BIND_ERR_INVALID_ARG;

  if (state == DATA_BIND_VALUE_STATE_ABSENT) return DATA_BIND_OK;
  if (state == DATA_BIND_VALUE_STATE_NULL) {
    script_value.type = EXPRTK_VAL_NULL;
  } else if (state == DATA_BIND_VALUE_STATE_VALUE) {
    if (!value ||
        !databind_native_to_exprtk(
            entry->data, value, value_bytes, &script_value))
      return DATA_BIND_ERR_TYPE_MISMATCH;
  } else {
    return DATA_BIND_ERR_INVALID_ARG;
  }

  if (exprtk_map_set(
          &provider->staged, entry->address.name, script_value) != 0)
    return DATA_BIND_ERR_OOM;
  return DATA_BIND_OK;
}

static DataBindStatus databind_commit_output(
    void *context, DataBindError *error) {
  ts_plugin_databind_provider_t *provider =
      (ts_plugin_databind_provider_t *)context;
  (void)error;
  if (!provider ||
      !exprtk_value_is_object_like(&provider->staged))
    return DATA_BIND_ERR_INVALID_ARG;
  exprtk_value_destroy(&provider->result);
  provider->result = provider->staged;
  memset(&provider->staged, 0, sizeof(provider->staged));
  provider->staged.type = EXPRTK_VAL_NULL;
  return DATA_BIND_OK;
}

static void databind_abort_output(void *context) {
  ts_plugin_databind_provider_t *provider =
      (ts_plugin_databind_provider_t *)context;
  if (!provider) return;
  exprtk_value_destroy(&provider->staged);
  memset(&provider->staged, 0, sizeof(provider->staged));
  provider->staged.type = EXPRTK_VAL_NULL;
}

static DataBindBindingProvider databind_provider(
    ts_plugin_databind_provider_t *state) {
  DataBindBindingProvider provider = DATA_BIND_BINDING_PROVIDER_INIT;
  provider.context = state;
  provider.open_input = databind_open_input;
  provider.begin_output = databind_begin_output;
  provider.write_output = databind_write_output;
  provider.commit_output = databind_commit_output;
  provider.abort_output = databind_abort_output;
  return provider;
}

static int databind_param_pointee_equals(
    const cmeta_param_desc *param, const cmeta_type_desc *type) {
  return param && type && param->type &&
         param->type->kind == CMETA_T_POINTER &&
         param->type->pointee &&
         cmeta_type_equal(param->type->pointee, type);
}

static int databind_function_shape(
    const salts_plugin_export *entry,
    const DataBindServiceNativeBinding *native,
    size_t *request_index,
    size_t *response_index) {
  const cmeta_function_desc *function;
  const cmeta_function_abi_desc *abi;
  size_t request = SIZE_MAX;
  size_t response = SIZE_MAX;

  if (!entry || !native || !request_index || !response_index ||
      entry->kind != SALTS_PLUGIN_EXPORT_FUNCTION ||
      !entry->value.function.invoke ||
      !(function = native->function) ||
      !(abi = entry->value.function.abi) ||
      !cmeta_function_desc_valid(function) ||
      !cmeta_function_abi_desc_valid(abi) ||
      !cmeta_function_desc_equal(
          function, entry->value.function.desc) ||
      !cmeta_type_equal(function->return_type, &cmeta_type_int) ||
      abi->return_carrier != CMETA_ABI_SCALAR ||
      native->error_count != 0u ||
      native->error_param_index != SIZE_MAX ||
      !native->request || !native->response ||
      !native->request->data || !native->response->data ||
      !native->request->data->storage_type ||
      !native->response->data->storage_type)
    return 0;

  for (size_t i = 0u; i < function->param_count; ++i) {
    const cmeta_param_desc *param = cmeta_function_param(function, i);
    const cmeta_param_flags direction =
        param ? param->flags & CMETA_PARAM_DIRECTION_MASK : 0u;

    if (cmeta_function_param_abi(abi, i) != CMETA_ABI_OBJECT_POINTER)
      return 0;
    if (direction == CMETA_PARAM_IN &&
        databind_param_pointee_equals(
            param, native->request->data->storage_type)) {
      if (request != SIZE_MAX) return 0;
      request = i;
      continue;
    }
    if (direction == CMETA_PARAM_OUT &&
        databind_param_pointee_equals(
            param, native->response->data->storage_type)) {
      if (response != SIZE_MAX) return 0;
      response = i;
      continue;
    }
    return 0;
  }

  if (request == SIZE_MAX || response == SIZE_MAX) return 0;
  *request_index = request;
  *response_index = response;
  return 1;
}

static void databind_call_error(
    exprtk_env_t *env, const char *export_id, const char *detail) {
  if (!env) return;
  env->aborted = 1;
  snprintf(
      env->error_msg, sizeof(env->error_msg),
      "DataBind Service '%s' failed: %s",
      export_id ? export_id : "<unknown>",
      detail ? detail : "unknown error");
}

static exprtk_value_t databind_function_call(
    size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
  ts_plugin_databind_binding_t *binding =
      (ts_plugin_databind_binding_t *)user_data;
  ts_plugin_databind_provider_t provider_state = {0};
  DataBindBindingProvider provider;
  DataBindNativeOptions native_options = DATA_BIND_NATIVE_OPTIONS_INIT;
  DataBindNativeDiagnostic native_diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  DataBindBindingCallFrame frame =
      DATA_BIND_BINDING_CALL_FRAME_INIT;
  unsigned char *workspace = NULL;
  void *request = NULL;
  void *response = NULL;
  void **frame_params = NULL;
  void **invoke_params = NULL;
  size_t *param_bytes = NULL;
  size_t request_bytes;
  size_t response_bytes;
  size_t param_count;
  int native_status = -1;
  DataBindStatus bind_status;
  exprtk_value_t result = {0};

  result.type = EXPRTK_VAL_NULL;
  if (!binding || !binding->entry || !binding->plan ||
      !binding->invoke || !env ||
      argc != 1u || !args ||
      !exprtk_value_is_object_like(&args[0])) {
    databind_call_error(
        env, binding && binding->entry
                 ? binding->entry->export_id : NULL,
        "expected one map/object argument");
    return exprtk_val_num(0.0);
  }

  request_bytes =
      binding->native.request->data->storage_type->size;
  response_bytes =
      binding->native.response->data->storage_type->size;
  param_count = binding->native.function->param_count;
  if (request_bytes == 0u || response_bytes == 0u ||
      param_count == 0u || param_count > 32u) {
    databind_call_error(
        env, binding->entry->export_id,
        "invalid generated native storage contract");
    return exprtk_val_num(0.0);
  }

  workspace = (unsigned char *)calloc(
      1u, TS_PLUGIN_DATABIND_WORKSPACE_BYTES);
  request = calloc(1u, request_bytes);
  response = calloc(1u, response_bytes);
  frame_params = (void **)calloc(param_count, sizeof(*frame_params));
  invoke_params = (void **)calloc(param_count, sizeof(*invoke_params));
  param_bytes = (size_t *)calloc(param_count, sizeof(*param_bytes));
  if (!workspace || !request || !response ||
      !frame_params || !invoke_params || !param_bytes) {
    databind_call_error(
        env, binding->entry->export_id, "out of memory");
    goto cleanup;
  }

  native_options.workspace = workspace;
  native_options.workspace_bytes = TS_PLUGIN_DATABIND_WORKSPACE_BYTES;
  native_options.max_depth = 32u;
  native_options.max_items = 1024u;
  native_options.max_owned_bytes =
      env->max_external_value_bytes != 0u
          ? env->max_external_value_bytes
          : 1024u * 1024u;

  bind_status = data_bind_native_init(
      &native_options, binding->native.response->data,
      response, response_bytes, &native_diagnostic);
  if (bind_status != DATA_BIND_OK) {
    databind_call_error(
        env, binding->entry->export_id,
        native_diagnostic.error.message[0]
            ? native_diagnostic.error.message
            : "initialize response storage");
    goto cleanup;
  }

  frame_params[binding->response_param_index] = response;
  param_bytes[binding->response_param_index] = response_bytes;
  frame.request = request;
  frame.request_bytes = request_bytes;
  frame.return_value = &native_status;
  frame.return_bytes = sizeof(native_status);
  frame.params = frame_params;
  frame.param_bytes = param_bytes;
  frame.param_count = param_count;

  provider_state.input = &args[0];
  provider_state.staged.type = EXPRTK_VAL_NULL;
  provider_state.result.type = EXPRTK_VAL_NULL;
  provider = databind_provider(&provider_state);

  bind_status = data_bind_binding_plan_bind_inputs(
      binding->plan, &provider, &native_options,
      &frame, &diagnostic);
  if (bind_status != DATA_BIND_OK) {
    databind_call_error(
        env, binding->entry->export_id,
        diagnostic.message[0]
            ? diagnostic.message
            : "bind input map");
    goto cleanup;
  }

  for (size_t i = 0u; i < param_count; ++i)
    invoke_params[i] = frame_params[i];
  invoke_params[binding->request_param_index] = request;
  invoke_params[binding->response_param_index] = response;
  if (!binding->invoke(
          binding->context, &native_status,
          invoke_params, param_count)) {
    databind_call_error(
        env, binding->entry->export_id,
        "exact Plugin adapter rejected invocation");
    goto cleanup;
  }
  if (native_status != 0) {
    char message[96];
    snprintf(
        message, sizeof(message),
        "native Service status %d", native_status);
    databind_call_error(env, binding->entry->export_id, message);
    goto cleanup;
  }

  bind_status = data_bind_binding_plan_write_outputs(
      binding->plan, &provider, &frame, &diagnostic);
  if (bind_status != DATA_BIND_OK) {
    databind_call_error(
        env, binding->entry->export_id,
        diagnostic.message[0]
            ? diagnostic.message
            : "publish Service response");
    goto cleanup;
  }

  result = provider_state.result;
  provider_state.result.type = EXPRTK_VAL_NULL;

cleanup:
  exprtk_value_destroy(&provider_state.staged);
  exprtk_value_destroy(&provider_state.result);

  if (response && binding && binding->native.response &&
      binding->native.response->data) {
    native_diagnostic =
        (DataBindNativeDiagnostic)DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
    (void)data_bind_native_clear(
        &native_options, binding->native.response->data,
        response, response_bytes, &native_diagnostic);
  }
  if (request && binding && binding->native.request &&
      binding->native.request->data) {
    native_diagnostic =
        (DataBindNativeDiagnostic)DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
    (void)data_bind_native_clear(
        &native_options, binding->native.request->data,
        request, request_bytes, &native_diagnostic);
  }

  free(param_bytes);
  free(invoke_params);
  free(frame_params);
  free(response);
  free(request);
  free(workspace);

  return env && env->aborted ? exprtk_val_num(0.0) : result;
}

static int databind_binding_matches_export(
    const ts_plugin_databind_binding_t *binding,
    const char *export_id) {
  return binding && binding->entry &&
         binding->entry->export_id && export_id &&
         strcmp(binding->entry->export_id, export_id) == 0;
}

int ts_plugin_databind_covers(
    const ts_plugin_handle_t *handle, const char *export_id) {
  const ts_plugin_databind_binding_t *bindings;
  if (!handle || !export_id || !handle->databind_bindings)
    return 0;
  bindings =
      (const ts_plugin_databind_binding_t *)handle->databind_bindings;
  for (size_t i = 0u; i < handle->databind_binding_count; ++i)
    if (databind_binding_matches_export(&bindings[i], export_id))
      return 1;
  return 0;
}

void ts_plugin_databind_clear(ts_plugin_handle_t *handle) {
  ts_plugin_databind_binding_t *bindings;
  if (!handle) return;

  bindings =
      (ts_plugin_databind_binding_t *)handle->databind_bindings;
  if (bindings) {
    for (size_t i = 0u; i < handle->databind_binding_count; ++i)
      data_bind_binding_plan_free(bindings[i].plan);
    free(bindings);
  }
  handle->databind_bindings = NULL;
  handle->databind_binding_count = 0u;

  if (handle->databind_codec)
    data_bind_free((DataBind *)handle->databind_codec);
  handle->databind_codec = NULL;
}

int ts_plugin_databind_bind(
    ts_plugin_handle_t *handle, exprtk_env_t *env,
    ts_plugin_error_t *error) {
  const salts_plugin_export *catalog_entry = NULL;
  data_bind_plugin_catalog *catalog;
  DataBind *codec = NULL;
  ts_plugin_databind_binding_t *bindings = NULL;
  exprtk_native_registration_t *registrations = NULL;
  DataBindBindingProjection projection = {
      sizeof(DataBindBindingProjection),
      DATA_BIND_BINDING_PLAN_ABI_VERSION,
      "turboscript.databind.map",
      NULL,
      databind_project_field
  };
  size_t count;
  salts_plugin_status plugin_status = SALTS_PLUGIN_OK;
  exprtk_registration_status_t registration_status;

  if (!handle || !handle->manifest || !env)
    return databind_error(
        error, TS_PLUGIN_ERROR_INVALID_ARGUMENT, 0u,
        "DataBind Plugin binding requires a manifest and environment");

  plugin_status = salts_plugin_manifest_find_export(
      handle->manifest, DATA_BIND_PLUGIN_CATALOG_EXPORT_ID,
      &catalog_entry);
  if (plugin_status == SALTS_PLUGIN_UNKNOWN_EXPORT)
    return TS_PLUGIN_ERROR_NONE;
  if (plugin_status != SALTS_PLUGIN_OK || !catalog_entry ||
      salts_plugin_export_require_interface(
          catalog_entry,
          DATA_BIND_PLUGIN_CATALOG_CONTRACT_ID,
          DATA_BIND_PLUGIN_CATALOG_CONTRACT_VERSION,
          0u,
          data_bind_plugin_catalog_interface()) != SALTS_PLUGIN_OK)
    return databind_error(
        error, TS_PLUGIN_ERROR_DESCRIPTOR,
        (uint32_t)plugin_status,
        "invalid DataBind Service catalog export");

  catalog =
      (data_bind_plugin_catalog *)catalog_entry->value.interface.value;
  if (!data_bind_plugin_catalog_valid(catalog))
    return databind_error(
        error, TS_PLUGIN_ERROR_DESCRIPTOR, 0u,
        "invalid DataBind Service catalog interface");

  {
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindStatus status =
        data_bind_plugin_catalog_create_codec(
            catalog, &codec, &bind_error);
    if (status != DATA_BIND_OK || !codec)
      return databind_error(
          error, TS_PLUGIN_ERROR_DESCRIPTOR, (uint32_t)status,
          bind_error.message[0]
              ? bind_error.message
              : "DataBind Service catalog codec creation failed");
  }

  count = data_bind_plugin_catalog_operation_count(catalog);
  if (count == 0u || count > 256u) {
    data_bind_free(codec);
    return databind_error(
        error, TS_PLUGIN_ERROR_DESCRIPTOR, 0u,
        "DataBind Service catalog operation count is invalid");
  }

  bindings = (ts_plugin_databind_binding_t *)calloc(
      count, sizeof(*bindings));
  registrations = (exprtk_native_registration_t *)calloc(
      count, sizeof(*registrations));
  if (!bindings || !registrations) {
    free(registrations);
    free(bindings);
    data_bind_free(codec);
    return databind_error(
        error, TS_PLUGIN_ERROR_OUT_OF_MEMORY, 0u,
        "out of memory compiling DataBind Service catalog");
  }

  for (size_t i = 0u; i < count; ++i) {
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    DataBindStatus status;
    const salts_plugin_export *function_entry = NULL;

    bindings[i].operation =
        (DataBindPluginOperationBinding)
            DATA_BIND_PLUGIN_OPERATION_BINDING_INIT;
    status = data_bind_plugin_catalog_operation_at(
        catalog, i, &bindings[i].operation, &bind_error);
    if (status != DATA_BIND_OK ||
        !data_bind_plugin_operation_binding_valid(
            &bindings[i].operation))
      goto descriptor_failure;

    plugin_status = salts_plugin_manifest_find_export(
        handle->manifest, bindings[i].operation.export_id,
        &function_entry);
    if (plugin_status != SALTS_PLUGIN_OK || !function_entry ||
        function_entry->kind != SALTS_PLUGIN_EXPORT_FUNCTION ||
        !function_entry->value.function.invoke ||
        !cmeta_function_desc_equal(
            function_entry->value.function.desc,
            bindings[i].operation.function))
      goto descriptor_failure;

    if (!data_bind_plugin_operation_native_binding(
            &bindings[i].operation, &bindings[i].native) ||
        !databind_function_shape(
            function_entry, &bindings[i].native,
            &bindings[i].request_param_index,
            &bindings[i].response_param_index))
      goto descriptor_failure;

    status = data_bind_binding_plan_compile_service(
        codec,
        bindings[i].operation.service_name,
        bindings[i].operation.operation_name,
        &projection,
        &bindings[i].native,
        &bindings[i].plan,
        &diagnostic);
    if (status != DATA_BIND_OK || !bindings[i].plan)
      goto descriptor_failure;

    bindings[i].entry = function_entry;
    bindings[i].invoke = function_entry->value.function.invoke;
    bindings[i].context = function_entry->value.function.context;

    registrations[i].name = bindings[i].operation.export_id;
    registrations[i].fn = databind_function_call;
    registrations[i].user_data = &bindings[i];
    registrations[i].flags = EXPRTK_NATIVE_PRESERVE_VALUE_TYPES;
    continue;

descriptor_failure:
    for (size_t j = 0u; j <= i; ++j)
      data_bind_binding_plan_free(bindings[j].plan);
    free(registrations);
    free(bindings);
    data_bind_free(codec);
    return databind_error(
        error, TS_PLUGIN_ERROR_DESCRIPTOR,
        (uint32_t)plugin_status,
        diagnostic.message[0]
            ? diagnostic.message
            : bind_error.message[0]
                  ? bind_error.message
                  : "DataBind Service catalog operation is not supported");
  }

  registration_status = exprtk_env_register_funcs_checked(
      env, registrations, count);
  free(registrations);
  if (registration_status != EXPRTK_REGISTRATION_OK) {
    for (size_t i = 0u; i < count; ++i)
      data_bind_binding_plan_free(bindings[i].plan);
    free(bindings);
    data_bind_free(codec);
    return databind_error(
        error,
        registration_status == EXPRTK_REGISTRATION_OUT_OF_MEMORY
            ? TS_PLUGIN_ERROR_OUT_OF_MEMORY
            : TS_PLUGIN_ERROR_DESCRIPTOR,
        (uint32_t)registration_status,
        registration_status == EXPRTK_REGISTRATION_CONFLICT
            ? "DataBind Service export conflicts with an existing script binding"
            : "failed to register DataBind Service bindings");
  }

  handle->databind_bindings = bindings;
  handle->databind_binding_count = count;
  handle->databind_codec = codec;
  return TS_PLUGIN_ERROR_NONE;
}
