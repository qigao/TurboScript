#include "ts_plugin_databind.h"
#include "ts_plugin_loader.h"

#include <data_bind_plugin_catalog.h>
#include <data_bind_native.h>
#include <cserde/writer.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_DATABIND_WORKSPACE_BASE 16384u
#define TS_DATABIND_VALUE_DEPTH 32u
#define TS_DATABIND_MAX_ITEMS 4096u

typedef struct ts_plugin_databind_binding_s {
  const salts_plugin_export *entry;
  salts_plugin_function_invoke_fn invoke;
  void *context;
  DataBindPluginOperationBinding operation;
  DataBindServiceNativeBinding native;
  DataBindBindingPlan *plan;
  size_t request_param;
  size_t response_param;
} ts_plugin_databind_binding_t;

typedef struct ts_databind_token_reader_s {
  cserde_token token;
  int emitted;
} ts_databind_token_reader_t;

typedef struct ts_databind_value_frame_s {
  exprtk_value_t container;
  char *key;
  size_t key_len;
  int is_map;
  int expect_key;
} ts_databind_value_frame_t;

typedef struct ts_databind_value_writer_s {
  exprtk_env_t *env;
  exprtk_value_t root;
  int has_root;
  ts_databind_value_frame_t frames[TS_DATABIND_VALUE_DEPTH];
  size_t depth;
} ts_databind_value_writer_t;

typedef struct ts_databind_provider_s {
  exprtk_env_t *env;
  const exprtk_value_t *input;
  exprtk_value_t staged;
  exprtk_value_t *result;
  int has_staged;
  ts_databind_token_reader_t reader;
} ts_databind_provider_t;

static void ts_databind_error(
    char *error, size_t error_size, const char *text) {
  if (!error || error_size == 0u) return;
  snprintf(error, error_size, "%s", text ? text : "DataBind plugin binding failed");
}

static cserde_status ts_databind_token_next(
    void *context, cserde_token *out) {
  ts_databind_token_reader_t *reader =
      (ts_databind_token_reader_t *)context;
  if (!reader || !out) return CSERDE_INVALID_ARGUMENT;
  if (reader->emitted) return CSERDE_DONE;
  *out = reader->token;
  reader->emitted = 1;
  return CSERDE_OK;
}

static const cserde_reader_ops TS_DATABIND_TOKEN_READER_OPS = {
    offsetof(cserde_reader_ops, next) + sizeof(cserde_reader_next_fn),
    CSERDE_READER_OPS_ABI_VERSION,
    ts_databind_token_next
};

static int ts_databind_token_from_value(
    exprtk_value_t value, cserde_token *out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  switch (value.type) {
    case EXPRTK_VAL_BOOL:
      out->kind = CSERDE_BOOL;
      out->value.boolean = value.data.boolean != 0;
      return 1;
    case EXPRTK_VAL_INTEGER:
      out->kind = CSERDE_SINT;
      out->value.sint = value.data.integer;
      return 1;
    case EXPRTK_VAL_NUMBER:
      out->kind = CSERDE_FLOAT;
      out->value.floating = value.data.number;
      return 1;
    case EXPRTK_VAL_STRING:
      out->kind = CSERDE_STRING;
      out->value.slice.data =
          (const unsigned char *)value.data.string.data;
      out->value.slice.size = value.data.string.len;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      return 1;
    case EXPRTK_VAL_BYTES:
      out->kind = CSERDE_BYTES;
      out->value.slice.data =
          (const unsigned char *)value.data.bytes.data;
      out->value.slice.size = value.data.bytes.len;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      return 1;
    default:
      return 0;
  }
}

static exprtk_value_t ts_databind_null_value(void) {
  exprtk_value_t value = {0};
  value.type = EXPRTK_VAL_NULL;
  return value;
}

static int ts_databind_value_from_token(
    const cserde_token *token, exprtk_value_t *out) {
  if (!token || !out) return 0;
  switch (token->kind) {
    case CSERDE_NULL:
      *out = ts_databind_null_value();
      return 1;
    case CSERDE_BOOL:
      *out = exprtk_val_bool(token->value.boolean);
      return 1;
    case CSERDE_SINT:
      *out = exprtk_val_int(token->value.sint);
      return 1;
    case CSERDE_UINT:
      if (token->value.uint > (uint64_t)INT64_MAX) return 0;
      *out = exprtk_val_int((int64_t)token->value.uint);
      return 1;
    case CSERDE_FLOAT:
      *out = exprtk_val_num(token->value.floating);
      return 1;
    case CSERDE_STRING:
      *out = exprtk_val_str(vstr_from_buf(
          (char *)token->value.slice.data, token->value.slice.size));
      return 1;
    case CSERDE_BYTES:
      *out = exprtk_val_bytes(vstr_from_buf(
          (char *)token->value.slice.data, token->value.slice.size));
      return 1;
    default:
      return 0;
  }
}

static void ts_databind_writer_clear(ts_databind_value_writer_t *writer) {
  if (!writer) return;
  for (size_t i = 0u; i < writer->depth; ++i) {
    free(writer->frames[i].key);
    writer->frames[i].key = NULL;
    exprtk_value_destroy(&writer->frames[i].container);
  }
  if (writer->has_root) exprtk_value_destroy(&writer->root);
  memset(writer, 0, sizeof(*writer));
}

static int ts_databind_writer_attach(
    ts_databind_value_writer_t *writer, exprtk_value_t *value) {
  ts_databind_value_frame_t *frame;
  if (!writer || !value) return 0;

  if (writer->depth == 0u) {
    if (writer->has_root) return 0;
    writer->root = *value;
    writer->has_root = 1;
    *value = ts_databind_null_value();
    return 1;
  }

  frame = &writer->frames[writer->depth - 1u];
  if (frame->is_map) {
    int ok;
    if (frame->expect_key || !frame->key) return 0;
    ok = exprtk_map_set(&frame->container, frame->key, *value) == 0;
    free(frame->key);
    frame->key = NULL;
    frame->key_len = 0u;
    frame->expect_key = 1;
    return ok;
  }

  return exprtk_list_push(&frame->container, *value) == 0;
}

static cserde_status ts_databind_writer_write(
    void *context, const cserde_token *token) {
  ts_databind_value_writer_t *writer =
      (ts_databind_value_writer_t *)context;
  ts_databind_value_frame_t *frame;
  exprtk_value_t value;

  if (!writer || !token || !cserde_token_valid(token))
    return CSERDE_INVALID_ARGUMENT;

  if (writer->depth != 0u) {
    frame = &writer->frames[writer->depth - 1u];
    if (frame->is_map && frame->expect_key) {
      if (token->kind == CSERDE_MAP_END) {
        exprtk_value_t completed;
        if (frame->key) return CSERDE_INVALID_STATE;
        completed = frame->container;
        frame->container = ts_databind_null_value();
        --writer->depth;
        return ts_databind_writer_attach(writer, &completed)
                   ? CSERDE_OK : CSERDE_SINK_ERROR;
      }
      if (token->kind != CSERDE_STRING)
        return CSERDE_INVALID_TOKEN;
      frame->key = (char *)malloc(token->value.slice.size + 1u);
      if (!frame->key) return CSERDE_SINK_ERROR;
      if (token->value.slice.size != 0u)
        memcpy(frame->key, token->value.slice.data,
               token->value.slice.size);
      frame->key[token->value.slice.size] = '\0';
      frame->key_len = token->value.slice.size;
      frame->expect_key = 0;
      return CSERDE_OK;
    }
    if (!frame->is_map && token->kind == CSERDE_ARRAY_END) {
      exprtk_value_t completed = frame->container;
      frame->container = ts_databind_null_value();
      --writer->depth;
      return ts_databind_writer_attach(writer, &completed)
                 ? CSERDE_OK : CSERDE_SINK_ERROR;
    }
  }

  if (token->kind == CSERDE_MAP_BEGIN ||
      token->kind == CSERDE_ARRAY_BEGIN) {
    if (writer->depth >= TS_DATABIND_VALUE_DEPTH)
      return CSERDE_LIMIT_EXCEEDED;
    frame = &writer->frames[writer->depth++];
    memset(frame, 0, sizeof(*frame));
    frame->is_map = token->kind == CSERDE_MAP_BEGIN;
    frame->expect_key = frame->is_map;
    frame->container =
        frame->is_map ? exprtk_val_map() : exprtk_val_list_empty();
    return CSERDE_OK;
  }

  if (token->kind == CSERDE_MAP_END ||
      token->kind == CSERDE_ARRAY_END)
    return CSERDE_INVALID_STATE;

  if (!ts_databind_value_from_token(token, &value))
    return CSERDE_UNSUPPORTED;
  if (!ts_databind_writer_attach(writer, &value)) {
    exprtk_value_destroy(&value);
    return CSERDE_SINK_ERROR;
  }
  exprtk_value_destroy(&value);
  return CSERDE_OK;
}

static cserde_status ts_databind_writer_finish(void *context) {
  ts_databind_value_writer_t *writer =
      (ts_databind_value_writer_t *)context;
  return writer && writer->has_root && writer->depth == 0u
             ? CSERDE_OK : CSERDE_INVALID_STATE;
}

static const cserde_writer_ops TS_DATABIND_VALUE_WRITER_OPS = {
    sizeof(cserde_writer_ops),
    CSERDE_WRITER_OPS_ABI_VERSION,
    ts_databind_writer_write,
    ts_databind_writer_finish
};

static int ts_databind_native_value(
    exprtk_env_t *env, const cmeta_data_desc *data,
    const void *source, size_t source_bytes,
    exprtk_value_t *out) {
  ts_databind_value_writer_t sink = {0};
  cserde_writer writer = {0};
  DataBindNativeOptions options = DATA_BIND_NATIVE_OPTIONS_INIT;
  DataBindNativeDiagnostic diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  unsigned char workspace[4096];
  DataBindStatus status;
  (void)source_bytes;

  if (out) *out = ts_databind_null_value();
  if (!env || !data || !source || !out) return 0;

  sink.env = env;
  if (cserde_writer_init(
          &writer, &TS_DATABIND_VALUE_WRITER_OPS, &sink) != CSERDE_OK)
    return 0;

  options.workspace = workspace;
  options.workspace_bytes = sizeof(workspace);
  options.max_depth = TS_DATABIND_VALUE_DEPTH;
  options.max_items = TS_DATABIND_MAX_ITEMS;
  options.max_owned_bytes =
      env->max_external_value_bytes != 0u
          ? env->max_external_value_bytes
          : 1024u * 1024u;

  status = data_bind_native_encode(
      &options, data, source,
      data->storage_type ? data->storage_type->size : 0u,
      &writer, &diagnostic);
  if (status == DATA_BIND_OK &&
      cserde_writer_finish(&writer) != CSERDE_OK)
    status = DATA_BIND_ERR_RUNTIME;
  if (status != DATA_BIND_OK || !sink.has_root) {
    ts_databind_writer_clear(&sink);
    return 0;
  }

  *out = sink.root;
  sink.root = ts_databind_null_value();
  sink.has_root = 0;
  ts_databind_writer_clear(&sink);
  return 1;
}

static DataBindStatus ts_databind_project_field(
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
  out->space = "turboscript.plugin";
  out->name = field->name;
  out->ordinal = 0u;
  return DATA_BIND_OK;
}

static const DataBindBindingProjection TS_DATABIND_PROJECTION = {
    sizeof(DataBindBindingProjection),
    DATA_BIND_BINDING_PLAN_ABI_VERSION,
    "turboscript.plugin",
    NULL,
    ts_databind_project_field
};

static DataBindStatus ts_databind_open_input(
    void *context, const DataBindBindingPlanEntry *entry,
    cserde_reader *reader, DataBindBindingValueState *state,
    DataBindError *error) {
  ts_databind_provider_t *provider =
      (ts_databind_provider_t *)context;
  exprtk_value_t value;
  const char *name;
  (void)error;

  if (!provider || !entry || !reader || !state ||
      !provider->input ||
      !exprtk_value_is_object_like(provider->input))
    return DATA_BIND_ERR_INVALID_ARG;

  name = entry->address.name
             ? entry->address.name
             : entry->schema_field;
  if (!name || !exprtk_map_has(provider->input, name)) {
    *state = DATA_BIND_VALUE_STATE_ABSENT;
    return DATA_BIND_OK;
  }

  value = exprtk_map_get(provider->input, name);
  if (value.type == EXPRTK_VAL_NULL) {
    *state = DATA_BIND_VALUE_STATE_NULL;
    return DATA_BIND_OK;
  }

  if (!ts_databind_token_from_value(value, &provider->reader.token))
    return DATA_BIND_ERR_TYPE_MISMATCH;
  provider->reader.emitted = 0;
  *state = DATA_BIND_VALUE_STATE_VALUE;
  return cserde_reader_init(
             reader, &TS_DATABIND_TOKEN_READER_OPS,
             &provider->reader) == CSERDE_OK
             ? DATA_BIND_OK : DATA_BIND_ERR_RUNTIME;
}

static DataBindStatus ts_databind_begin_output(
    void *context, DataBindError *error) {
  ts_databind_provider_t *provider =
      (ts_databind_provider_t *)context;
  (void)error;
  if (!provider || !provider->result) return DATA_BIND_ERR_INVALID_ARG;
  if (provider->has_staged)
    exprtk_value_destroy(&provider->staged);
  provider->staged = exprtk_val_map();
  provider->has_staged = 1;
  return exprtk_value_is_object_like(&provider->staged)
             ? DATA_BIND_OK : DATA_BIND_ERR_OOM;
}

static DataBindStatus ts_databind_write_output(
    void *context, const DataBindBindingPlanEntry *entry,
    DataBindBindingValueState state, const void *value,
    size_t value_bytes, DataBindError *error) {
  ts_databind_provider_t *provider =
      (ts_databind_provider_t *)context;
  exprtk_value_t converted = ts_databind_null_value();
  const char *name;
  (void)error;

  if (!provider || !entry || !provider->has_staged)
    return DATA_BIND_ERR_INVALID_ARG;

  name = entry->schema_field;
  if (!name) return DATA_BIND_ERR_SCHEMA;

  if (entry->address.binding_class == DATA_BIND_BINDING_ERROR) {
    exprtk_value_t envelope = exprtk_val_map();
    exprtk_value_t type =
        exprtk_val_str(vstr_from_cstr(name));

    if (state != DATA_BIND_VALUE_STATE_VALUE || !value ||
        !ts_databind_native_value(
            provider->env, entry->data, value, value_bytes,
            &converted) ||
        exprtk_map_set(&envelope, "type", type) != 0 ||
        exprtk_map_set(&envelope, "payload", converted) != 0 ||
        exprtk_map_set(&provider->staged, "error", envelope) != 0) {
      exprtk_value_destroy(&converted);
      exprtk_value_destroy(&envelope);
      return DATA_BIND_ERR_RUNTIME;
    }
    exprtk_value_destroy(&converted);
    exprtk_value_destroy(&envelope);
    return DATA_BIND_OK;
  }

  if (entry->address.binding_class != DATA_BIND_BINDING_RESULT)
    return DATA_BIND_ERR_SCHEMA;

  if (state == DATA_BIND_VALUE_STATE_ABSENT)
    return DATA_BIND_OK;
  if (state == DATA_BIND_VALUE_STATE_NULL) {
    converted = ts_databind_null_value();
  } else if (!value ||
             !ts_databind_native_value(
                 provider->env, entry->data, value, value_bytes,
                 &converted)) {
    return DATA_BIND_ERR_TYPE_MISMATCH;
  }

  if (exprtk_map_set(&provider->staged, name, converted) != 0) {
    exprtk_value_destroy(&converted);
    return DATA_BIND_ERR_OOM;
  }
  exprtk_value_destroy(&converted);
  return DATA_BIND_OK;
}

static DataBindStatus ts_databind_commit_output(
    void *context, DataBindError *error) {
  ts_databind_provider_t *provider =
      (ts_databind_provider_t *)context;
  (void)error;
  if (!provider || !provider->result || !provider->has_staged)
    return DATA_BIND_ERR_INVALID_ARG;
  exprtk_value_destroy(provider->result);
  *provider->result = provider->staged;
  provider->staged = ts_databind_null_value();
  provider->has_staged = 0;
  return DATA_BIND_OK;
}

static void ts_databind_abort_output(void *context) {
  ts_databind_provider_t *provider =
      (ts_databind_provider_t *)context;
  if (!provider || !provider->has_staged) return;
  exprtk_value_destroy(&provider->staged);
  provider->staged = ts_databind_null_value();
  provider->has_staged = 0;
}

static const cmeta_type_desc *ts_databind_param_value_type(
    const cmeta_param_desc *param) {
  if (!param || !param->type) return NULL;
  return param->type->kind == CMETA_T_POINTER
             ? param->type->pointee : param->type;
}

static int ts_databind_generated_abi(
    ts_plugin_databind_binding_t *binding,
    const cmeta_function_abi_desc *abi,
    char *error, size_t error_size) {
  const cmeta_function_desc *function;
  size_t request = SIZE_MAX;
  size_t response = SIZE_MAX;
  size_t expected_params;

  if (!binding || !abi ||
      !cmeta_function_desc_valid(binding->native.function) ||
      !cmeta_function_abi_desc_valid(abi) ||
      abi->function != binding->native.function ||
      abi->return_carrier != CMETA_ABI_SCALAR ||
      !cmeta_type_equal(
          binding->native.function->return_type,
          &cmeta_type_int)) {
    ts_databind_error(
        error, error_size,
        "generated Service Function ABI must return native int status");
    return 0;
  }

  function = binding->native.function;
  expected_params =
      binding->native.error_count != 0u ? 3u : 2u;
  if (function->param_count != expected_params) {
    ts_databind_error(
        error, error_size,
        "generated Service Function has unsupported parameter shape");
    return 0;
  }

  for (size_t i = 0u; i < function->param_count; ++i) {
    const cmeta_param_desc *param = &function->params[i];
    const cmeta_type_desc *value_type =
        ts_databind_param_value_type(param);
    const cmeta_param_flags direction =
        param->flags & CMETA_PARAM_DIRECTION_MASK;

    if (binding->native.error_count != 0u &&
        i == binding->native.error_param_index) {
      if (direction != CMETA_PARAM_OUT ||
          param->type->kind != CMETA_T_POINTER ||
          !value_type ||
          value_type->size != binding->native.error_envelope_bytes ||
          cmeta_function_param_abi(abi, i) !=
              CMETA_ABI_OBJECT_POINTER) {
        ts_databind_error(
            error, error_size,
            "generated Service typed-error parameter ABI is invalid");
        return 0;
      }
      continue;
    }

    if (direction == CMETA_PARAM_IN &&
        value_type &&
        cmeta_type_equal(
            value_type,
            binding->operation.request.data->storage_type)) {
      if (request != SIZE_MAX ||
          param->type->kind != CMETA_T_POINTER ||
          cmeta_function_param_abi(abi, i) !=
              CMETA_ABI_OBJECT_POINTER) {
        ts_databind_error(
            error, error_size,
            "generated Service request parameter ABI is ambiguous");
        return 0;
      }
      request = i;
      continue;
    }

    if (direction == CMETA_PARAM_OUT &&
        value_type &&
        cmeta_type_equal(
            value_type,
            binding->operation.response.data->storage_type)) {
      if (response != SIZE_MAX ||
          param->type->kind != CMETA_T_POINTER ||
          cmeta_function_param_abi(abi, i) !=
              CMETA_ABI_OBJECT_POINTER) {
        ts_databind_error(
            error, error_size,
            "generated Service response parameter ABI is ambiguous");
        return 0;
      }
      response = i;
      continue;
    }

    ts_databind_error(
        error, error_size,
        "generated Service Function contains unsupported native parameters");
    return 0;
  }

  if (request == SIZE_MAX || response == SIZE_MAX) {
    ts_databind_error(
        error, error_size,
        "generated Service request/response root parameters are unavailable");
    return 0;
  }

  binding->request_param = request;
  binding->response_param = response;
  return 1;
}

static void ts_databind_binding_clear(
    ts_plugin_databind_binding_t *binding) {
  if (!binding) return;
  data_bind_binding_plan_free(binding->plan);
  memset(binding, 0, sizeof(*binding));
}

void ts_plugin_databind_clear(
    struct ts_plugin_handle_s *handle) {
  ts_plugin_databind_binding_t *bindings;
  if (!handle) return;
  bindings =
      (ts_plugin_databind_binding_t *)handle->databind_bindings;
  for (size_t i = 0u; i < handle->databind_binding_count; ++i)
    ts_databind_binding_clear(&bindings[i]);
  free(bindings);
  handle->databind_bindings = NULL;
  handle->databind_binding_count = 0u;
  if (handle->databind_codec) {
    data_bind_free((DataBind *)handle->databind_codec);
    handle->databind_codec = NULL;
  }
}

int ts_plugin_databind_prepare(
    struct ts_plugin_handle_s *handle,
    char *error, size_t error_size) {
  const salts_plugin_export *catalog_entry = NULL;
  data_bind_plugin_catalog *catalog;
  DataBind *codec = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  ts_plugin_databind_binding_t *bindings = NULL;
  size_t count;
  salts_plugin_status plugin_status;

  if (!handle || !handle->manifest) {
    ts_databind_error(error, error_size, "invalid Plugin handle");
    return 0;
  }

  plugin_status = salts_plugin_manifest_find_export(
      handle->manifest, DATA_BIND_PLUGIN_CATALOG_EXPORT_ID,
      &catalog_entry);
  if (plugin_status == SALTS_PLUGIN_UNKNOWN_EXPORT)
    return 1;
  if (plugin_status != SALTS_PLUGIN_OK || !catalog_entry ||
      salts_plugin_export_require_interface(
          catalog_entry,
          DATA_BIND_PLUGIN_CATALOG_CONTRACT_ID,
          DATA_BIND_PLUGIN_CATALOG_CONTRACT_VERSION,
          0u, data_bind_plugin_catalog_interface()) !=
          SALTS_PLUGIN_OK) {
    ts_databind_error(
        error, error_size,
        "invalid DataBind Service catalog Interface export");
    return 0;
  }

  catalog =
      (data_bind_plugin_catalog *)catalog_entry->value.interface.value;
  if (!data_bind_plugin_catalog_valid(catalog)) {
    ts_databind_error(
        error, error_size,
        "DataBind Service catalog value is invalid");
    return 0;
  }

  if (data_bind_plugin_catalog_create_codec(
          catalog, &codec, &bind_error) != DATA_BIND_OK ||
      !codec) {
    ts_databind_error(
        error, error_size,
        bind_error.message[0]
            ? bind_error.message
            : "DataBind Service catalog codec creation failed");
    return 0;
  }

  count = data_bind_plugin_catalog_operation_count(catalog);
  if (count == 0u) {
    data_bind_free(codec);
    return 1;
  }

  bindings = (ts_plugin_databind_binding_t *)calloc(
      count, sizeof(*bindings));
  if (!bindings) {
    data_bind_free(codec);
    ts_databind_error(error, error_size, "out of memory binding DataBind catalog");
    return 0;
  }

  for (size_t i = 0u; i < count; ++i) {
    ts_plugin_databind_binding_t *binding = &bindings[i];
    const salts_plugin_export *function_export = NULL;
    DataBindBindingPlanDiagnostic diagnostic =
        DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;

    binding->operation =
        (DataBindPluginOperationBinding)
            DATA_BIND_PLUGIN_OPERATION_BINDING_INIT;
    bind_error = (DataBindError)DATA_BIND_ERROR_INIT;
    if (data_bind_plugin_catalog_operation_at(
            catalog, i, &binding->operation,
            &bind_error) != DATA_BIND_OK ||
        !data_bind_plugin_operation_binding_valid(
            &binding->operation)) {
      ts_databind_error(
          error, error_size,
          bind_error.message[0]
              ? bind_error.message
              : "DataBind catalog operation is invalid");
      goto fail;
    }

    if (salts_plugin_manifest_find_export(
            handle->manifest, binding->operation.export_id,
            &function_export) != SALTS_PLUGIN_OK ||
        !function_export ||
        function_export->kind != SALTS_PLUGIN_EXPORT_FUNCTION ||
        salts_plugin_export_require_function(
            function_export, function_export->contract_id,
            function_export->contract_version, 0u) !=
            SALTS_PLUGIN_OK ||
        !cmeta_function_desc_equal(
            binding->operation.function,
            function_export->value.function.desc)) {
      ts_databind_error(
          error, error_size,
          "DataBind catalog operation does not match its Function export");
      goto fail;
    }

    binding->entry = function_export;
    binding->invoke = function_export->value.function.invoke;
    binding->context = function_export->value.function.context;
    if (!binding->invoke ||
        !data_bind_plugin_operation_native_binding(
            &binding->operation, &binding->native) ||
        !ts_databind_generated_abi(
            binding, function_export->value.function.abi,
            error, error_size)) {
      if (!error || error[0] == '\0')
        ts_databind_error(
            error, error_size,
            "DataBind generated Service ABI is unsupported");
      goto fail;
    }

    if (data_bind_binding_plan_compile_service(
            codec,
            binding->operation.service_name,
            binding->operation.operation_name,
            &TS_DATABIND_PROJECTION,
            &binding->native,
            &binding->plan,
            &diagnostic) != DATA_BIND_OK ||
        !binding->plan) {
      ts_databind_error(
          error, error_size,
          diagnostic.message[0]
              ? diagnostic.message
              : "DataBind Service BindingPlan compilation failed");
      goto fail;
    }
  }

  handle->databind_codec = codec;
  handle->databind_bindings = bindings;
  handle->databind_binding_count = count;
  return 1;

fail:
  for (size_t i = 0u; i < count; ++i)
    ts_databind_binding_clear(&bindings[i]);
  free(bindings);
  data_bind_free(codec);
  return 0;
}

size_t ts_plugin_databind_count(
    const struct ts_plugin_handle_s *handle) {
  return handle ? handle->databind_binding_count : 0u;
}

void *ts_plugin_databind_find(
    const struct ts_plugin_handle_s *handle,
    const char *export_id) {
  ts_plugin_databind_binding_t *bindings;
  if (!handle || !export_id || !handle->databind_bindings)
    return NULL;
  bindings =
      (ts_plugin_databind_binding_t *)handle->databind_bindings;
  for (size_t i = 0u; i < handle->databind_binding_count; ++i) {
    if (bindings[i].operation.export_id &&
        strcmp(bindings[i].operation.export_id, export_id) == 0)
      return &bindings[i];
  }
  return NULL;
}

static void ts_databind_set_env_error(
    exprtk_env_t *env, const char *prefix, const char *detail) {
  if (!env) return;
  env->aborted = 1;
  snprintf(
      env->error_msg, sizeof(env->error_msg),
      "%s%s%s",
      prefix ? prefix : "DataBind Service call failed",
      detail && detail[0] ? ": " : "",
      detail && detail[0] ? detail : "");
}

static int ts_databind_clear_native(
    const DataBindNativeOptions *options,
    const cmeta_data_desc *data,
    void *storage, size_t bytes) {
  DataBindNativeDiagnostic diagnostic =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  if (!data || !storage) return 1;
  return data_bind_native_clear(
             options, data, storage, bytes,
             &diagnostic) == DATA_BIND_OK;
}

exprtk_value_t ts_plugin_databind_call(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, void *user_data) {
  ts_plugin_databind_binding_t *binding =
      (ts_plugin_databind_binding_t *)user_data;
  const cmeta_function_desc *function;
  size_t param_count;
  void *request = NULL;
  void *response = NULL;
  void *error_envelope = NULL;
  void **params = NULL;
  void **invoke_params = NULL;
  size_t *param_bytes = NULL;
  unsigned char *workspace = NULL;
  size_t workspace_bytes;
  int native_status = 0;
  int response_initialized = 0;
  int request_bound = 0;
  exprtk_value_t result = exprtk_val_map();
  ts_databind_provider_t provider_state = {0};
  DataBindBindingProvider provider =
      DATA_BIND_BINDING_PROVIDER_INIT;
  DataBindNativeOptions options =
      DATA_BIND_NATIVE_OPTIONS_INIT;
  DataBindBindingCallFrame frame =
      DATA_BIND_BINDING_CALL_FRAME_INIT;
  DataBindBindingPlanDiagnostic diagnostic =
      DATA_BIND_BINDING_PLAN_DIAGNOSTIC_INIT;
  DataBindBindingOutcome outcome =
      DATA_BIND_BINDING_OUTCOME_INIT;
  DataBindNativeDiagnostic native_diag =
      DATA_BIND_NATIVE_DIAGNOSTIC_INIT;
  DataBindStatus status;
  int invoke_ok = 0;

  if (!binding || !binding->plan || !binding->invoke || !env) {
    ts_databind_set_env_error(
        env, "invalid DataBind Service binding", NULL);
    return exprtk_val_num(0.0);
  }

  if (data_bind_binding_plan_ingress_count(binding->plan) == 0u) {
    if (argc > 1u ||
        (argc == 1u && !exprtk_value_is_object_like(&args[0]))) {
      ts_databind_set_env_error(
          env, "DataBind Service expects zero fields or one map/object",
          NULL);
      return exprtk_val_num(0.0);
    }
  } else if (argc != 1u ||
             !exprtk_value_is_object_like(&args[0])) {
    ts_databind_set_env_error(
        env, "DataBind Service expects one map/object argument", NULL);
    return exprtk_val_num(0.0);
  }

  function = binding->native.function;
  param_count = function->param_count;
  request = calloc(
      1u, binding->operation.request.data->storage_type->size);
  response = calloc(
      1u, binding->operation.response.data->storage_type->size);
  params = (void **)calloc(param_count, sizeof(*params));
  invoke_params = (void **)calloc(
      param_count, sizeof(*invoke_params));
  param_bytes = (size_t *)calloc(
      param_count, sizeof(*param_bytes));
  workspace_bytes =
      TS_DATABIND_WORKSPACE_BASE +
      (data_bind_binding_plan_ingress_count(binding->plan) +
       data_bind_binding_plan_egress_count(binding->plan) +
       data_bind_binding_plan_error_count(binding->plan)) *
          256u;
  workspace = (unsigned char *)calloc(1u, workspace_bytes);

  if (!request || !response || !params || !invoke_params ||
      !param_bytes || !workspace) {
    ts_databind_set_env_error(
        env, "out of memory staging DataBind Service call", NULL);
    goto done;
  }

  if (binding->native.error_count != 0u) {
    error_envelope =
        calloc(1u, binding->native.error_envelope_bytes);
    if (!error_envelope) {
      ts_databind_set_env_error(
          env, "out of memory staging typed Service error", NULL);
      goto done;
    }
  }

  params[binding->request_param] = request;
  param_bytes[binding->request_param] =
      binding->operation.request.data->storage_type->size;
  params[binding->response_param] = response;
  param_bytes[binding->response_param] =
      binding->operation.response.data->storage_type->size;
  if (binding->native.error_count != 0u) {
    params[binding->native.error_param_index] = error_envelope;
    param_bytes[binding->native.error_param_index] =
        binding->native.error_envelope_bytes;
  }
  for (size_t i = 0u; i < param_count; ++i)
    invoke_params[i] = params[i];

  options.workspace = workspace;
  options.workspace_bytes = workspace_bytes;
  options.max_depth = TS_DATABIND_VALUE_DEPTH;
  options.max_items = TS_DATABIND_MAX_ITEMS;
  options.max_owned_bytes =
      env->max_external_value_bytes != 0u
          ? env->max_external_value_bytes
          : 1024u * 1024u;

  provider_state.env = env;
  provider_state.input = argc == 1u ? &args[0] : NULL;
  provider_state.result = &result;
  provider.context = &provider_state;
  provider.open_input = ts_databind_open_input;
  provider.begin_output = ts_databind_begin_output;
  provider.write_output = ts_databind_write_output;
  provider.commit_output = ts_databind_commit_output;
  provider.abort_output = ts_databind_abort_output;

  frame.request = request;
  frame.request_bytes =
      binding->operation.request.data->storage_type->size;
  frame.return_value = &native_status;
  frame.return_bytes = sizeof(native_status);
  frame.params = params;
  frame.param_bytes = param_bytes;
  frame.param_count = param_count;

  status = data_bind_binding_plan_bind_inputs(
      binding->plan, &provider, &options,
      &frame, &diagnostic);
  if (status != DATA_BIND_OK) {
    ts_databind_set_env_error(
        env, "DataBind Service input binding failed",
        diagnostic.message);
    goto done;
  }
  request_bound = 1;

  status = data_bind_native_init(
      &options, binding->operation.response.data,
      response, binding->operation.response.data->storage_type->size,
      &native_diag);
  if (status != DATA_BIND_OK) {
    ts_databind_set_env_error(
        env, "DataBind Service response staging failed",
        native_diag.error.message);
    goto done;
  }
  response_initialized = 1;

  invoke_ok = binding->invoke(
      binding->context, &native_status,
      invoke_params, param_count);
  if (!invoke_ok) {
    ts_databind_set_env_error(
        env, "DataBind Service exact adapter rejected invocation",
        binding->operation.export_id);
    goto done;
  }

  status = data_bind_binding_plan_write_outcome(
      binding->plan, &provider, &frame,
      native_status, &outcome, &diagnostic);
  if (status != DATA_BIND_OK) {
    ts_databind_set_env_error(
        env, "DataBind Service output binding failed",
        diagnostic.message);
    goto done;
  }

  if (outcome.kind ==
      DATA_BIND_BINDING_OUTCOME_NATIVE_STATUS) {
    char detail[128];
    snprintf(
        detail, sizeof(detail),
        "%s returned native status %d",
        binding->operation.export_id,
        outcome.native_status);
    ts_databind_set_env_error(
        env, "DataBind Service invocation failed", detail);
    goto done;
  }

done:
  ts_databind_abort_output(&provider_state);

  if (binding && request && request_bound) {
    (void)ts_databind_clear_native(
        &options, binding->operation.request.data,
        request,
        binding->operation.request.data->storage_type->size);
  }
  if (binding && response && response_initialized) {
    (void)ts_databind_clear_native(
        &options, binding->operation.response.data,
        response,
        binding->operation.response.data->storage_type->size);
  }
  if (binding && error_envelope &&
      outcome.kind == DATA_BIND_BINDING_OUTCOME_TYPED_ERROR &&
      outcome.typed_error_index <
          binding->operation.error_count) {
    const DataBindNativeErrorBinding *error_binding =
        &binding->operation.errors[outcome.typed_error_index];
    const cmeta_data_desc *error_data = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    if (error_binding->data_resolver &&
        error_binding->data_resolver(
            &error_data, &error) == DATA_BIND_OK &&
        cmeta_data_desc_valid(error_data) &&
        error_binding->payload_offset <=
            binding->native.error_envelope_bytes &&
        error_data->storage_type->size <=
            binding->native.error_envelope_bytes -
                error_binding->payload_offset) {
      (void)ts_databind_clear_native(
          &options, error_data,
          (unsigned char *)error_envelope +
              error_binding->payload_offset,
          error_data->storage_type->size);
    }
  }

  free(workspace);
  free(param_bytes);
  free(invoke_params);
  free(params);
  free(error_envelope);
  free(response);
  free(request);

  if (env->aborted) {
    exprtk_value_destroy(&result);
    return exprtk_val_num(0.0);
  }
  return result;
}
