#include "turbo_script_class_databind.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ts_databind_text_builder_s {
  char *data;
  size_t length;
  size_t capacity;
} ts_databind_text_builder_t;

static void ts_databind_set_error(
    char *error, size_t error_len, const char *fmt, ...) {
  va_list args;
  if (!error || error_len == 0u) return;
  va_start(args, fmt);
  vsnprintf(error, error_len, fmt, args);
  va_end(args);
}

static int ts_builder_reserve(ts_databind_text_builder_t *builder,
                              size_t extra) {
  size_t needed;
  size_t capacity;
  char *grown;
  if (!builder || extra > SIZE_MAX - builder->length - 1u) return 0;
  needed = builder->length + extra + 1u;
  if (needed <= builder->capacity) return 1;
  capacity = builder->capacity ? builder->capacity : 256u;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2u) return 0;
    capacity *= 2u;
  }
  grown = (char *)realloc(builder->data, capacity);
  if (!grown) return 0;
  builder->data = grown;
  builder->capacity = capacity;
  return 1;
}

static int ts_builder_append_n(ts_databind_text_builder_t *builder,
                               const char *text, size_t length) {
  if (!builder || (!text && length != 0u) ||
      !ts_builder_reserve(builder, length))
    return 0;
  if (length != 0u) memcpy(builder->data + builder->length, text, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
  return 1;
}

static int ts_builder_append(ts_databind_text_builder_t *builder,
                             const char *text) {
  return text ? ts_builder_append_n(builder, text, strlen(text)) : 0;
}

static int ts_builder_printf(ts_databind_text_builder_t *builder,
                             const char *format, ...) {
  va_list args;
  va_list copy;
  int needed;
  if (!builder || !format) return 0;
  va_start(args, format);
  va_copy(copy, args);
  needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (needed < 0 || !ts_builder_reserve(builder, (size_t)needed)) {
    va_end(args);
    return 0;
  }
  vsnprintf(builder->data + builder->length,
            builder->capacity - builder->length, format, args);
  va_end(args);
  builder->length += (size_t)needed;
  return 1;
}

static int ts_builder_quoted(ts_databind_text_builder_t *builder, vstr value) {
  if (!ts_builder_append(builder, "\"")) return 0;
  for (size_t i = 0u; i < value.len; ++i) {
    const unsigned char ch = (unsigned char)value.data[i];
    switch (ch) {
      case '\\':
        if (!ts_builder_append(builder, "\\\\")) return 0;
        break;
      case '"':
        if (!ts_builder_append(builder, "\\\"")) return 0;
        break;
      case '\n':
        if (!ts_builder_append(builder, "\\n")) return 0;
        break;
      case '\r':
        if (!ts_builder_append(builder, "\\r")) return 0;
        break;
      case '\t':
        if (!ts_builder_append(builder, "\\t")) return 0;
        break;
      default:
        if (ch < 0x20u) return 0;
        if (!ts_builder_append_n(
                builder, (const char *)&value.data[i], 1u))
          return 0;
        break;
    }
  }
  return ts_builder_append(builder, "\"");
}

static const char *ts_databind_type(const cmeta_data_desc *data) {
  if (!cmeta_data_desc_valid(data)) return NULL;
  if (cmeta_data_desc_equal(data, &cmeta_data_int64)) return "int64";
  if (cmeta_data_desc_equal(data, &cmeta_data_double)) return "double";
  if (cmeta_data_desc_equal(data, &cmeta_data_bool)) return "bool";
  if (data->kind == CMETA_DATA_STRING && data->storage_type != NULL)
    return "string";
  return NULL;
}

static int ts_schema_default(
    ts_databind_text_builder_t *builder,
    const cmeta_data_desc *data,
    const exprtk_value_t *value) {
  if (!builder || !data || !value) return 0;
  if (cmeta_data_desc_equal(data, &cmeta_data_int64) &&
      value->type == EXPRTK_VAL_INTEGER)
    return ts_builder_printf(builder, " default %lld",
                             (long long)value->data.integer);
  if (cmeta_data_desc_equal(data, &cmeta_data_double) &&
      value->type == EXPRTK_VAL_NUMBER)
    return ts_builder_printf(builder, " default %.17g",
                             value->data.number);
  if (cmeta_data_desc_equal(data, &cmeta_data_bool) &&
      value->type == EXPRTK_VAL_BOOL)
    return ts_builder_append(
        builder, value->data.boolean ? " default true" : " default false");
  if (data->kind == CMETA_DATA_STRING &&
      value->type == EXPRTK_VAL_STRING) {
    if (!ts_builder_append(builder, " default ")) return 0;
    return ts_builder_quoted(builder, value->data.string);
  }
  return 0;
}

static int ts_build_schema(
    exprtk_class_t *klass, char **out_schema, size_t *out_length,
    char *error, size_t error_len) {
  ts_databind_text_builder_t builder = {0};
  const cmeta_data_desc *object_data;

  if (out_schema) *out_schema = NULL;
  if (out_length) *out_length = 0u;
  if (!klass || !out_schema || !out_length ||
      !exprtk_class_finalize_cmeta_data(klass)) {
    ts_databind_set_error(
        error, error_len, "DataBind class reflection is unavailable");
    return 0;
  }

  object_data = exprtk_class_cmeta_data(klass);
  if (!object_data || object_data->kind != CMETA_DATA_STRUCT) {
    ts_databind_set_error(
        error, error_len, "invalid class CMeta data surface");
    return 0;
  }

  if (!ts_builder_append(
          &builder, "schema TurboScriptMapper [version(1)]; message ") ||
      !ts_builder_append(&builder, klass->name) ||
      !ts_builder_append(&builder, " {")) {
    free(builder.data);
    ts_databind_set_error(
        error, error_len, "out of memory building DataBind contract");
    return 0;
  }

  for (size_t i = 0u; i < klass->instance_field_count; ++i) {
    const cmeta_data_desc *data =
        klass->cmeta_data_fields ? klass->cmeta_data_fields[i].value : NULL;
    if (!ts_databind_type(data) || !klass->instance_field_names[i]) {
      free(builder.data);
      return -1;
    }
  }

  /* Keep the established canonical ordering used by mapper: fixed-width
   * scalar fields first, variable string fields second. MessagePlan binds
   * provider-backed fields by name, so this does not alter class slot order. */
  for (unsigned pass = 0u; pass < 2u; ++pass) {
    for (size_t i = 0u; i < klass->instance_field_count; ++i) {
      const cmeta_data_desc *data = klass->cmeta_data_fields[i].value;
      const char *type = ts_databind_type(data);
      const char *name = klass->instance_field_names[i];
      const unsigned variable =
          data->kind == CMETA_DATA_STRING ? 1u : 0u;

      if (variable != pass) continue;
      if (!ts_builder_append(&builder, " ") ||
          !ts_builder_append(&builder, type) ||
          !ts_builder_append(&builder, " ") ||
          !ts_builder_append(&builder, name)) {
        free(builder.data);
        ts_databind_set_error(
            error, error_len, "out of memory building DataBind fields");
        return 0;
      }

      if (klass->instance_field_has_default &&
          klass->instance_field_has_default[i]) {
        if (!klass->instance_field_defaults ||
            !ts_schema_default(
                &builder, data, &klass->instance_field_defaults[i])) {
          free(builder.data);
          ts_databind_set_error(
              error, error_len,
              "unsupported typed class default for '%s'", name);
          return 0;
        }
      }

      if (!ts_builder_append(&builder, ";")) {
        free(builder.data);
        ts_databind_set_error(
            error, error_len, "out of memory building DataBind contract");
        return 0;
      }
    }
  }

  if (!ts_builder_append(&builder, " }")) {
    free(builder.data);
    ts_databind_set_error(
        error, error_len, "out of memory finalizing DataBind contract");
    return 0;
  }

  *out_schema = builder.data;
  *out_length = builder.length;
  return 1;
}

int turbo_script_class_databind_compile(
    exprtk_class_t *klass,
    DataBind **out_codec,
    DataBindMessagePlan **out_plan,
    char *error,
    size_t error_len) {
  DataBind *codec = NULL;
  DataBindMessagePlan *plan = NULL;
  DataBindError bind_error = DATA_BIND_ERROR_INIT;
  DataBindMessagePlanDiagnostic diagnostic =
      DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;
  const cmeta_data_desc *data;
  char *schema = NULL;
  size_t schema_len = 0u;
  DataBindStatus status;
  int schema_status;

  if (out_codec) *out_codec = NULL;
  if (out_plan) *out_plan = NULL;
  if (!klass || !out_codec || !out_plan ||
      !exprtk_class_finalize_cmeta_data(klass)) {
    ts_databind_set_error(error, error_len, "invalid reflected class");
    return 0;
  }

  data = exprtk_class_cmeta_data(klass);
  schema_status =
      ts_build_schema(klass, &schema, &schema_len, error, error_len);
  if (schema_status <= 0) return schema_status;

  status = data_bind_create_from_text(
      schema, schema_len, &codec, &bind_error);
  free(schema);
  if (status != DATA_BIND_OK) {
    ts_databind_set_error(
        error, error_len, "DataBind contract compile failed: %s",
        bind_error.message[0] ? bind_error.message : "invalid contract");
    return 0;
  }

  status = data_bind_message_plan_compile_object(
      codec, klass->name, data, &plan, &diagnostic);
  if (status != DATA_BIND_OK) {
    ts_databind_set_error(
        error, error_len, "MessagePlan compile failed: %s",
        diagnostic.message[0] ? diagnostic.message
                              : "invalid object binding");
    data_bind_free(codec);
    return 0;
  }

  *out_codec = codec;
  *out_plan = plan;
  return 1;
}
