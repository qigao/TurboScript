#include "ts_plugin_service_binding.h"

#include "exprtk_module.h"

#include <data_bind_binding_plan.h>
#include <data_bind_native.h>
#include <data_bind_plugin_catalog.h>

#include <cserde/reader.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_SERVICE_WORKSPACE_BYTES (16u * 1024u)

typedef struct ts_service_token_reader_s {
  cserde_token token;
  int emitted;
} ts_service_token_reader_t;

typedef struct ts_plugin_service_binding_s {
  const salts_plugin_export *entry;
  DataBindPluginOperationBinding operation;
  DataBindServiceNativeBinding native;
  DataBindBindingPlan *plan;
  salts_plugin_function_invoke_fn invoke;
  void *context;
} ts_plugin_service_binding_t;

typedef struct ts_service_provider_s {
  exprtk_env_t *env;
  exprtk_value_t request;
  ts_service_token_reader_t reader_state;
  exprtk_value_t staging;
  exprtk_value_t published;
  int output_open;
  int output_error;
} ts_service_provider_t;

static void service_set_error(
    char *error, size_t error_size, const char *message) {
  if (!error || error_size == 0u) return;
  snprintf(error, error_size, "%s", message ? message : "Service binding error");
}

static cserde_status service_token_next(void *context, cserde_token *out) {
  ts_service_token_reader_t *reader =
      (ts_service_token_reader_t *)context;
  if (!reader || !out) return CSERDE_INVALID_ARGUMENT;
  if (reader->emitted) return CSERDE_DONE;
  *out = reader->token;
  reader->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops TS_SERVICE_TOKEN_READER_OPS = {
    offsetof(cserde_reader_ops, next) + sizeof(cserde_reader_next_fn),
    CSERDE_READER_OPS_ABI_VERSION,
    service_token_next};

static DataBindStatus service_value_token(
    const exprtk_value_t *value, const cmeta_data_desc *data,
    cserde_token *out, DataBindError *error) {
  const cmeta_data_integer_shape *integer_shape;
  const cmeta_data_float_shape *float_shape;
  (void)error;

  if (!value || !cmeta_data_desc_valid(data) || !out)
    return DATA_BIND_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));

  if (value->type == EXPRTK_VAL_NULL) {
    out->kind = CSERDE_NULL;
    return DATA_BIND_OK;
  }

  switch (data->kind) {
    case CMETA_DATA_BOOL:
      if (value->type != EXPRTK_VAL_BOOL)
        return DATA_BIND_ERR_TYPE_MISMATCH;
      out->kind = CSERDE_BOOL;
      out->value.boolean = value->data.boolean != 0;
      return DATA_BIND_OK;

    case CMETA_DATA_SINT:
      integer_shape = (const cmeta_data_integer_shape *)data->shape;
      if (!integer_shape || value->type != EXPRTK_VAL_INTEGER)
        return DATA_BIND_ERR_TYPE_MISMATCH;
      if ((integer_shape->bits < 64u) &&
          (value->data.integer <
               -(INT64_C(1) << (integer_shape->bits - 1u)) ||
           value->data.integer >
               (INT64_C(1) << (integer_shape->bits - 1u)) - 1))
        return DATA_BIND_ERR_TYPE_MISMATCH;
      out->kind = CSERDE_SINT;
      out->value.sint = value->data.integer;
      return DATA_BIND_OK;

    case CMETA_DATA_UINT:
      integer_shape = (const cmeta_data_integer_shape *)data->shape;
      if (!integer_shape || value->type != EXPRTK_VAL_INTEGER ||
          value->data.integer < 0)
        return DATA_BIND_ERR_TYPE_MISMATCH;
      if (integer_shape->bits < 63u &&
          (uint64_t)value->data.integer >
              ((UINT64_C(1) << integer_shape->bits) - 1u))
        return DATA_BIND_ERR_TYPE_MISMATCH;
      out->kind = CSERDE_UINT;
      out->value.uint = (uint64_t)value->data.integer;
      return DATA_BIND_OK;

    case CMETA_DATA_FLOAT:
      float_shape = (const cmeta_data_float_shape *)data->shape;
      if (!float_shape ||
          (value->type != EXPRTK_VAL_NUMBER &&
           value->type != EXPRTK_VAL_INTEGER))
        return DATA_BIND_ERR_TYPE_MISMATCH;
      out->kind = CSERDE_FLOAT;
      out->value.floating =
          value->type == EXPRTK_VAL_NUMBER
              ? value->data.number
              : (double)value->data.integer;
      if (!isfinite(out->value.floating))
        return DATA_BIND_ERR_TYPE_MISMATCH;
      return DATA_BIND_OK;

    case CMETA_DATA_STRING:
      if (value->type != EXPRTK_VAL_STRING)
        return DATA_BIND_ERR_TYPE_MISMATCH;
      out->kind = CSERDE_STRING;
      out->value.slice.data =
          (const unsigned char *)value->data.string.data;
      out->value.slice.size = value->data.string.len;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      return DATA_BIND_OK;

    case CMETA_DATA_BYTES:
      if (value->type != EXPRTK_VAL_BYTES)
        return DATA_BIND_ERR_TYPE_MISMATCH;
      out->kind = CSERDE_BYTES;
      out->value.slice.data =
          (const unsigned char *)value->data.bytes.data;
      out->value.slice.size = value->data.bytes.len;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      return DATA_BIND_OK;

    default:
      return DATA_BIND_ERR_TYPE_MISMATCH;
  }
}

static DataBindStatus service_open_input(
    void *context, const DataBindBindingPlanEntry *entry,
    cserde_reader *reader, DataBindBindingValueState *state,
    DataBindError *error) {
  ts_service_provider_t *provider =
      (ts_service_provider_t *)context;
  const char *name;
  exprtk_value_t value;
  DataBindStatus status;

  if (!provider || !entry || !reader || !state ||
      !exprtk_value_is_object_like(&provider->request))
    return DATA_BIND_ERR_INVALID_ARG;

  name = entry->address.name ? entry->address.name : entry->schema_field;
  if (!name || !exprtk_map_has(&provider->request, name)) {
    *state = DATA_BIND_VALUE_STATE_ABSENT;
    return DATA_BIND_OK;
  }

  value = exprtk_map_get(&provider->request, name);
  if (value.type == EXPRTK_VAL_NULL) {
    *state = DATA_BIND_VALUE_STATE_NULL;
    return DATA_BIND_OK;
  }

  status = service_value_token(
      &value, entry->data, &provider->reader_state.token, error);
  if (status != DATA_BIND_OK) return status;
  provider->reader_state.emitted = 0;
  *state = DATA_BIND_VALUE_STATE_VALUE;
  return cserde_reader_init(
             reader, &TS_SERVICE_TOKEN_READER_OPS,
             &provider->reader_state) == CSERDE_OK
             ? DATA_BIND_OK
             : DATA_BIND_ERR_RUNTIME;
}

static int service_read_signed(
    const cmeta_data_desc *data, const void *value, int64_t *out) {
  const cmeta_data_integer_shape *shape =
      data ? (const cmeta_data_integer_shape *)data->shape : NULL;
  if (!shape || !value || !out) return 0;
  switch (shape->bits) {
    case 8: {
      int8_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    case 16: {
      int16_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    case 32: {
      int32_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    case 64: {
      int64_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    default:
      return 0;
  }
}

static int service_read_unsigned(
    const cmeta_data_desc *data, const void *value, uint64_t *out) {
  const cmeta_data_integer_shape *shape =
      data ? (const cmeta_data_integer_shape *)data->shape : NULL;
  if (!shape || !value || !out) return 0;
  switch (shape->bits) {
    case 8: {
      uint8_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    case 16: {
      uint16_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    case 32: {
      uint32_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    case 64: {
      uint64_t v;
      memcpy(&v, value, sizeof(v));
      *out = v;
      return 1;
    }
    default:
      return 0;
  }
}

static int service_native_value(
    exprtk_env_t *env, const cmeta_data_desc *data,
    const void *value, exprtk_value_t *out) {
  const unsigned char *bytes = NULL;
  size_t byte_count = 0u;

  if (!env || !cmeta_data_desc_valid(data) || !value || !out)
    return 0;

  switch (data->kind) {
    case CMETA_DATA_BOOL: {
      uint8_t boolean = 0u;
      memcpy(&boolean, value,
             data->storage_type->size < sizeof(boolean)
                 ? data->storage_type->size
                 : sizeof(boolean));
      *out = exprtk_val_bool(boolean != 0u);
      return 1;
    }

    case CMETA_DATA_SINT: {
      int64_t signed_value = 0;
      if (!service_read_signed(data, value, &signed_value)) return 0;
      *out = exprtk_val_int(signed_value);
      return 1;
    }

    case CMETA_DATA_UINT: {
      uint64_t unsigned_value = 0u;
      if (!service_read_unsigned(data, value, &unsigned_value) ||
          unsigned_value > (uint64_t)INT64_MAX)
        return 0;
      *out = exprtk_val_int((int64_t)unsigned_value);
      return 1;
    }

    case CMETA_DATA_FLOAT: {
      const cmeta_data_float_shape *shape =
          (const cmeta_data_float_shape *)data->shape;
      double number;
      if (!shape) return 0;
      if (shape->bits == 32u) {
        float v;
        memcpy(&v, value, sizeof(v));
        number = (double)v;
      } else if (shape->bits == 64u) {
        memcpy(&number, value, sizeof(number));
      } else {
        return 0;
      }
      if (!isfinite(number)) return 0;
      *out = exprtk_val_num(number);
      return 1;
    }

    case CMETA_DATA_STRING:
    case CMETA_DATA_BYTES: {
      exprtk_value_t borrowed;
      if (cmeta_data_buffer_read(
              data, value,
              env->max_external_value_bytes
                  ? env->max_external_value_bytes
                  : (size_t)(1024u * 1024u),
              &bytes, &byte_count) != CMETA_OK)
        return 0;
      borrowed =
          data->kind == CMETA_DATA_STRING
              ? exprtk_val_str(vstr_from_buf((char *)bytes, byte_count))
              : exprtk_val_bytes(vstr_from_buf((char *)bytes, byte_count));
      *out = exprtk_value_clone_to_env(borrowed, env);
      return !env->aborted;
    }

    case CMETA_DATA_STRUCT: {
      const cmeta_data_struct_shape *shape =
          (const cmeta_data_struct_shape *)data->shape;
      exprtk_value_t map = exprtk_val_map();
      if (!shape || !shape->fields) {
        exprtk_value_destroy(&map);
        return 0;
      }
      for (size_t i = 0u; i < shape->field_count; ++i) {
        const cmeta_data_field_desc *field = &shape->fields[i];
        exprtk_value_t child = {0};
        if (!field->name || !field->value ||
            field->offset == CMETA_FIELD_DYNAMIC_OFFSET ||
            !service_native_value(
                env, field->value,
                (const unsigned char *)value + field->offset,
                &child) ||
            exprtk_map_set(&map, field->name, child) != 0) {
          exprtk_value_destroy(&child);
          exprtk_value_destroy(&map);
          return 0;
        }
        exprtk_value_destroy(&child);
      }
      *out = map;
      return 1;
    }

    default:
      return 0;
  }
}

static DataBindStatus service_begin_output(
    void *context, DataBindError *error) {
  ts_service_provider_t *provider =
      (ts_service_provider_t *)context;
  (void)error;
  if (!provider) return DATA_BIND_ERR_INVALID_ARG;
  if (provider->output_open)
    exprtk_value_destroy(&provider->staging);
  provider->staging = exprtk_val_map();
  provider->output_open = 1;
  provider->output_error = 0;
  return DATA_BIND_OK;
}

static DataBindStatus service_write_output(
    void *context, const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state, const void *value,
    size_t value_bytes, DataBindError *error) {
  ts_service_provider_t *provider =
      (ts_service_provider_t *)context;
  exprtk_value_t converted = {0};
  const char *name;
  (void)value_bytes;
  (void)error;

  if (!provider || !entry || !provider->output_open)
    return DATA_BIND_ERR_INVALID_ARG;

  if (state == DATA_BIND_VALUE_STATE_NULL) {
    converted.type = EXPRTK_VAL_NULL;
  } else if (state == DATA_BIND_VALUE_STATE_VALUE) {
    if (!service_native_value(
            provider->env, entry->data, value, &converted))
      return DATA_BIND_ERR_TYPE_MISMATCH;
  } else {
    return DATA_BIND_ERR_INVALID_ARG;
  }

  if (entry->address.binding_class == DATA_BIND_BINDING_ERROR) {
    exprtk_value_destroy(&provider->staging);
    provider->staging = converted;
    provider->output_error = 1;
    return DATA_BIND_OK;
  }

  name = entry->address.name ? entry->address.name : entry->schema_field;
  if (!name || exprtk_map_set(&provider->staging, name, converted) != 0) {
    exprtk_value_destroy(&converted);
    return DATA_BIND_ERR_OOM;
  }
  exprtk_value_destroy(&converted);
  return DATA_BIND_OK;
}

static DataBindStatus service_commit_output(
    void *context, DataBindError *error) {
  ts_service_provider_t *provider =
      (ts_service_provider_t *)context;
  (void)error;
  if (!provider || !provider->output_open)
    return DATA_BIND_ERR_INVALID_ARG;
  exprtk_value_destroy(&provider->published);
  provider->published = provider->staging;
  memset(&provider->staging, 0, sizeof(provider->staging));
  provider->staging.type = EXPRTK_VAL_NULL;
  provider->output_open = 0;
  return DATA_BIND_OK;
}

static void service_abort_output(void *context) {
  ts_service_provider_t *provider =
      (ts_service_provider_t *)context;
  if (!provider) return;
  if (provider->output_open)
    exprtk_value_destroy(&provider->staging);
  memset(&provider->staging, 0, sizeof(provider->staging));
  provider->staging.type = EXPRTK_VAL_NULL;
  provider->output_open = 0;
  provider->output_error = 0;
}

static DataBindStatus service_projection(
    void *context, const DataBindServiceOperation *operation,
    const DataBindSchemaField *field,
    DataBindBindingDirection direction,
    DataBindBindingAddress *out, DataBindError *error) {
  (void)context;
  (void)operation;
  (void)error;
  if (!field || !out) return DATA_BIND_ERR_INVALID_ARG;
  *out = (DataBindBindingAddress)DATA_BIND_BINDING_ADDRESS_INIT;
  out->binding_class =
      direction == DATA_BIND_BINDING_INGRESS
          ? DATA_BIND_BINDING_VALUE
          : DATA_BIND_BINDING_RESULT;
  out->space = "turboscript";
  out->name = field->name;
  out->ordinal = 0u;
  return DATA_BIND_OK;
}

static int service_generated_abi_valid(
    const ts_plugin_service_binding_t *binding) {
  const cmeta_function_desc *function;
  const cmeta_function_abi_desc *abi;
  size_t expected_count;

  if (!binding || !binding->entry ||
      binding->entry->kind != SALTS_PLUGIN_EXPORT_FUNCTION ||
      !data_bind_plugin_operation_binding_valid(&binding->operation))
    return 0;

  function = binding->entry->value.function.desc;
  abi = binding->entry->value.function.abi;
  expected_count = binding->operation.error_count ? 3u : 2u;

  if (!function || !abi ||
      !cmeta_function_desc_equal(function, binding->operation.function) ||
      !cmeta_function_abi_desc_valid(abi) ||
      !cmeta_function_desc_equal(abi->function, function) ||
      function->param_count != expected_count ||
      abi->param_count != expected_count ||
      !cmeta_type_equal(function->return_type, &cmeta_type_int) ||
      abi->return_carrier != CMETA_ABI_SCALAR ||
      cmeta_function_param_abi(abi, 0u) != CMETA_ABI_OBJECT_POINTER ||
      cmeta_function_param_abi(abi, 1u) != CMETA_ABI_OBJECT_POINTER)
    return 0;

  if ((function->params[0].flags & CMETA_PARAM_DIRECTION_MASK) !=
          CMETA_PARAM_IN ||
      (function->params[1].flags & CMETA_PARAM_DIRECTION_MASK) !=
          CMETA_PARAM_OUT)
    return 0;

  if (!function->params[0].type ||
      function->params[0].type->kind != CMETA_T_POINTER ||
      !function->params[0].type->pointee ||
      !cmeta_type_equal(
          function->params[0].type->pointee,
          binding->operation.request.data->storage_type) ||
      !function->params[1].type ||
      function->params[1].type->kind != CMETA_T_POINTER ||
      !function->params[1].type->pointee ||
      !cmeta_type_equal(
          function->params[1].type->pointee,
          binding->operation.response.data->storage_type))
    return 0;

  if (binding->operation.error_count != 0u) {
    if (binding->operation.error_param_index != 2u ||
        cmeta_function_param_abi(abi, 2u) != CMETA_ABI_OBJECT_POINTER ||
        (function->params[2].flags & CMETA_PARAM_DIRECTION_MASK) !=
            CMETA_PARAM_OUT)
      return 0;
  }

  return 1;
}

static void service_clear_error_payload(
    const ts_plugin_service_binding_t *binding,
    const DataBindNativeOptions *options,
    void *error_storage) {
  uint32_t kind = 0u;
  DataBindError error = DATA_BIND_ERROR_INIT;
  const cmeta_data_desc *data = NULL;
  DataBindNativeDiagnostic diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;

  if (!binding || !options || !error_storage ||
      binding->operation.error_count == 0u ||
      binding->operation.error_kind_bytes != sizeof(kind) ||
      binding->operation.error_kind_offset >
          binding->operation.error_envelope_bytes - sizeof(kind))
    return;

  memcpy(
      &kind,
      (unsigned char *)error_storage +
          binding->operation.error_kind_offset,
      sizeof(kind));
  if (kind == 0u || kind > binding->operation.error_count) return;

  const DataBindNativeErrorBinding *error_binding =
      &binding->operation.errors[kind - 1u];
  if (!error_binding->data_resolver ||
      error_binding->data_resolver(&data, &error) != DATA_BIND_OK ||
      !cmeta_data_desc_valid(data) || !data->storage_type ||
      error_binding->payload_offset >
          binding->operation.error_envelope_bytes ||
      data->storage_type->size >
          binding->operation.error_envelope_bytes -
              error_binding->payload_offset)
    return;

  (void)data_bind_native_clear(
      options, data,
      (unsigned char *)error_storage +
          error_binding->payload_offset,
      data->storage_type->size, &diagnostic);
}

static exprtk_value_t service_call(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, void *user_data) {
  ts_plugin_service_binding_t *binding =
      (ts_plugin_service_binding_t *)user_data;
  DataBindBindingProvider provider;
  ts_service_provider_t provider_state;
  DataBindBindingCallFrame frame =
      DATA_BIND_BINDING_CALL_FRAME_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  DataBindBindingOutcome outcome =
      DATA_BIND_BINDING_OUTCOME_INIT;
  DataBindNativeOptions options =
      DATA_BIND_NATIVE_OPTIONS_INIT;
  DataBindNativeDiagnostic native_diag =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  unsigned char *workspace = NULL;
  void *request = NULL;
  void *response = NULL;
  void *error_storage = NULL;
  void *params[3] = {NULL, NULL, NULL};
  size_t param_bytes[3] = {0u, 0u, 0u};
  size_t request_bytes;
  size_t response_bytes;
  int native_status = -1;
  int request_live = 0;
  int response_live = 0;
  exprtk_value_t result = {0};
  DataBindStatus bind_status;

  result.type = EXPRTK_VAL_NULL;
  if (!binding || !env || argc != 1u ||
      !exprtk_value_is_object_like(&args[0])) {
    if (env) {
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "generated DataBind Service call requires one map/object request");
    }
    return result;
  }

  request_bytes = binding->operation.request.data->storage_type->size;
  response_bytes = binding->operation.response.data->storage_type->size;
  request = calloc(1u, request_bytes);
  response = calloc(1u, response_bytes);
  workspace = (unsigned char *)malloc(TS_SERVICE_WORKSPACE_BYTES);
  if (!request || !response || !workspace) goto oom;

  if (binding->operation.error_count != 0u) {
    error_storage =
        calloc(1u, binding->operation.error_envelope_bytes);
    if (!error_storage) goto oom;
  }

  options.workspace = workspace;
  options.workspace_bytes = TS_SERVICE_WORKSPACE_BYTES;
  options.max_depth = 32u;
  options.max_items = 512u;
  options.max_owned_bytes =
      env->max_external_value_bytes
          ? env->max_external_value_bytes
          : (size_t)(1024u * 1024u);

  memset(&provider_state, 0, sizeof(provider_state));
  provider_state.env = env;
  provider_state.request = args[0];
  provider_state.staging.type = EXPRTK_VAL_NULL;
  provider_state.published.type = EXPRTK_VAL_NULL;

  provider = (DataBindBindingProvider)
      DATA_BIND_BINDING_PROVIDER_INIT;
  provider.context = &provider_state;
  provider.open_input = service_open_input;
  provider.begin_output = service_begin_output;
  provider.write_output = service_write_output;
  provider.commit_output = service_commit_output;
  provider.abort_output = service_abort_output;

  params[0] = request;
  params[1] = response;
  param_bytes[0] = request_bytes;
  param_bytes[1] = response_bytes;
  if (binding->operation.error_count != 0u) {
    params[2] = error_storage;
    param_bytes[2] = binding->operation.error_envelope_bytes;
  }

  frame.request = request;
  frame.request_bytes = request_bytes;
  frame.return_value = &native_status;
  frame.return_bytes = sizeof(native_status);
  frame.params = params;
  frame.param_bytes = param_bytes;
  frame.param_count =
      binding->operation.error_count != 0u ? 3u : 2u;

  bind_status = data_bind_binding_plan_bind_inputs(
      binding->plan, &provider, &options, &frame, &diagnostic);
  if (bind_status != DATA_BIND_OK) goto binding_error;
  request_live = 1;

  if (data_bind_native_init(
          &options, binding->operation.response.data,
          response, response_bytes, &native_diag) != DATA_BIND_OK)
    goto native_error;
  response_live = 1;

  if (!binding->invoke(
          binding->context, &native_status,
          params, frame.param_count)) {
    env->aborted = 1;
    snprintf(env->error_msg, sizeof(env->error_msg),
             "generated DataBind Service exact adapter rejected invocation");
    goto cleanup;
  }

  bind_status = data_bind_binding_plan_write_outcome(
      binding->plan, &provider, &frame,
      native_status, &outcome, &diagnostic);
  if (bind_status != DATA_BIND_OK) goto binding_error;

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS) {
    env->aborted = 1;
    snprintf(
        env->error_msg, sizeof(env->error_msg),
        "generated DataBind Service returned native status %d",
        outcome.native_status);
    goto cleanup;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_SUCCESS) {
    result = provider_state.published;
    provider_state.published.type = EXPRTK_VAL_NULL;
    goto cleanup;
  }

  if (outcome.kind == DATA_BIND_BINDING_OUTCOME_TYPED_ERROR) {
    exprtk_value_t wrapper = exprtk_val_map();
    exprtk_value_t type_value = exprtk_value_clone_to_env(
        exprtk_val_str(vstr_from_cstr(
            outcome.typed_error ? outcome.typed_error : "ServiceError")),
        env);
    if (env->aborted ||
        exprtk_map_set(&wrapper, "typed_error", type_value) != 0 ||
        exprtk_map_set(
            &wrapper, "value", provider_state.published) != 0) {
      exprtk_value_destroy(&type_value);
      exprtk_value_destroy(&wrapper);
      env->aborted = 1;
      snprintf(env->error_msg, sizeof(env->error_msg),
               "could not publish typed Service error");
      goto cleanup;
    }
    exprtk_value_destroy(&type_value);
    exprtk_value_destroy(&provider_state.published);
    provider_state.published.type = EXPRTK_VAL_NULL;
    result = wrapper;
    goto cleanup;
  }

  env->aborted = 1;
  snprintf(env->error_msg, sizeof(env->error_msg),
           "generated DataBind Service produced no outcome");
  goto cleanup;

oom:
  if (env) {
    env->aborted = 1;
    snprintf(env->error_msg, sizeof(env->error_msg),
             "out of memory preparing generated DataBind Service call");
  }
  goto cleanup;

binding_error:
  if (env) {
    env->aborted = 1;
    snprintf(
        env->error_msg, sizeof(env->error_msg),
        "generated DataBind Service binding failed: %s",
        diagnostic.message[0]
            ? diagnostic.message
            : "invalid request/response binding");
  }
  goto cleanup;

native_error:
  env->aborted = 1;
  snprintf(
      env->error_msg, sizeof(env->error_msg),
      "generated DataBind Service native staging failed: %s",
      native_diag.error.message[0]
          ? native_diag.error.message
          : "could not initialize response");
  goto cleanup;

cleanup:
  service_abort_output(&provider_state);
  exprtk_value_destroy(&provider_state.published);

  if (error_storage)
    service_clear_error_payload(binding, &options, error_storage);
  if (response_live) {
    native_diag =
        (DataBindNativeDiagnostic)DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
    (void)data_bind_native_clear(
        &options, binding->operation.response.data,
        response, response_bytes, &native_diag);
  }
  if (request_live) {
    native_diag =
        (DataBindNativeDiagnostic)DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
    (void)data_bind_native_clear(
        &options, binding->operation.request.data,
        request, request_bytes, &native_diag);
  }

  free(error_storage);
  free(response);
  free(request);
  free(workspace);
  return result;
}

static int service_binding_find_index(
    const ts_plugin_handle_t *handle, const char *export_id) {
  const ts_plugin_service_binding_t *bindings;
  if (!handle || !export_id || !handle->service_bindings)
    return -1;
  bindings =
      (const ts_plugin_service_binding_t *)handle->service_bindings;
  for (size_t i = 0u; i < handle->service_binding_count; ++i) {
    if (bindings[i].operation.export_id &&
        strcmp(bindings[i].operation.export_id, export_id) == 0)
      return (int)i;
  }
  return -1;
}

int ts_plugin_service_binding_get(
    const ts_plugin_handle_t *handle, const char *export_id,
    exprtk_native_fn *out_fn, void **out_user_data) {
  int index;
  if (out_fn) *out_fn = NULL;
  if (out_user_data) *out_user_data = NULL;
  if (!out_fn || !out_user_data) return 0;
  index = service_binding_find_index(handle, export_id);
  if (index < 0) return 0;
  *out_fn = service_call;
  *out_user_data =
      &((ts_plugin_service_binding_t *)handle->service_bindings)[index];
  return 1;
}

void ts_plugin_service_bindings_destroy(ts_plugin_handle_t *handle) {
  ts_plugin_service_binding_t *bindings;
  if (!handle || !handle->service_bindings) return;
  bindings =
      (ts_plugin_service_binding_t *)handle->service_bindings;
  for (size_t i = 0u; i < handle->service_binding_count; ++i)
    data_bind_binding_plan_free(bindings[i].plan);
  free(bindings);
  handle->service_bindings = NULL;
  handle->service_binding_count = 0u;
}

int ts_plugin_service_bindings_prepare(
    ts_plugin_handle_t *handle, exprtk_env_t *env,
    char *error, size_t error_size) {
  const salts_plugin_export *catalog_entry = NULL;
  data_bind_plugin_catalog *catalog = NULL;
  DataBind *codec = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindBindingProjection projection = {
      sizeof(DataBindBindingProjection),
      DATA_BIND_BINDING_PLAN_ABI_VERSION,
      "turboscript",
      NULL,
      service_projection};
  ts_plugin_service_binding_t *bindings = NULL;
  size_t count;
  salts_plugin_status plugin_status;

  (void)env;
  if (error && error_size) error[0] = '\0';
  if (!handle || !handle->manifest)
    return -1;

  plugin_status = salts_plugin_manifest_find_export(
      handle->manifest, DATA_BIND_PLUGIN_CATALOG_EXPORT_ID,
      &catalog_entry);
  if (plugin_status == SALTS_PLUGIN_UNKNOWN_EXPORT)
    return 0;
  if (plugin_status != SALTS_PLUGIN_OK || !catalog_entry ||
      salts_plugin_export_require_interface(
          catalog_entry,
          DATA_BIND_PLUGIN_CATALOG_CONTRACT_ID,
          DATA_BIND_PLUGIN_CATALOG_CONTRACT_VERSION,
          0u,
          data_bind_plugin_catalog_interface()) != SALTS_PLUGIN_OK) {
    service_set_error(
        error, error_size,
        "invalid DataBind Service catalog Interface");
    return -1;
  }

  catalog =
      (data_bind_plugin_catalog *)catalog_entry->value.interface.value;
  if (!data_bind_plugin_catalog_valid(catalog)) {
    service_set_error(
        error, error_size,
        "invalid DataBind Service catalog value");
    return -1;
  }

  count = data_bind_plugin_catalog_operation_count(catalog);
  if (count == 0u) return 0;

  bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
  if (data_bind_plugin_catalog_create_codec(
          catalog, &codec, &bind_error) != DATA_BIND_OK ||
      !codec) {
    service_set_error(
        error, error_size,
        bind_error.message[0]
            ? bind_error.message
            : "DataBind Service catalog could not create its codec");
    return -1;
  }

  bindings = (ts_plugin_service_binding_t *)calloc(
      count, sizeof(*bindings));
  if (!bindings) {
    data_bind_free(codec);
    service_set_error(
        error, error_size,
        "out of memory compiling DataBind Service catalog");
    return -1;
  }

  for (size_t i = 0u; i < count; ++i) {
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
    const salts_plugin_export *entry = NULL;

    bindings[i].operation =
        (DataBindPluginOperationBinding)
            DATA_BIND_PLUGIN_OPERATION_BINDING_INIT;
    bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
    if (data_bind_plugin_catalog_operation_at(
            catalog, i, &bindings[i].operation,
            &bind_error) != DATA_BIND_OK ||
        !data_bind_plugin_operation_binding_valid(
            &bindings[i].operation)) {
      service_set_error(
          error, error_size,
          bind_error.message[0]
              ? bind_error.message
              : "invalid DataBind Service catalog operation");
      goto fail;
    }

    if (salts_plugin_manifest_find_export(
            handle->manifest,
            bindings[i].operation.export_id,
            &entry) != SALTS_PLUGIN_OK ||
        !entry ||
        salts_plugin_export_require_function(
            entry, entry->contract_id,
            entry->contract_version, 0u) != SALTS_PLUGIN_OK) {
      service_set_error(
          error, error_size,
          "DataBind Service catalog operation has no matching Function export");
      goto fail;
    }

    bindings[i].entry = entry;
    bindings[i].invoke = entry->value.function.invoke;
    bindings[i].context = entry->value.function.context;
    if (!data_bind_plugin_operation_native_binding(
            &bindings[i].operation, &bindings[i].native) ||
        !service_generated_abi_valid(&bindings[i])) {
      service_set_error(
          error, error_size,
          "DataBind Service Function does not match generated root-object ABI");
      goto fail;
    }

    if (data_bind_binding_plan_compile_service(
            codec,
            bindings[i].operation.service_name,
            bindings[i].operation.operation_name,
            &projection, &bindings[i].native,
            &bindings[i].plan, &diagnostic) != DATA_BIND_OK ||
        !bindings[i].plan) {
      service_set_error(
          error, error_size,
          diagnostic.message[0]
              ? diagnostic.message
              : "DataBind Service BindingPlan compile failed");
      goto fail;
    }
  }

  data_bind_free(codec);
  handle->service_bindings = bindings;
  handle->service_binding_count = count;
  return 0;

fail:
  data_bind_free(codec);
  for (size_t i = 0u; i < count; ++i)
    data_bind_binding_plan_free(bindings[i].plan);
  free(bindings);
  return -1;
}
