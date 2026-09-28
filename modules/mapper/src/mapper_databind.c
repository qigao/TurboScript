/**
 * @file mapper_databind.c
 * @brief Typed class mapping through canonical CMeta + DataBind MessagePlan.
 */
#include "mapper_databind.h"

#include "exprtk_module.h"

#include <data_bind_format_provider.h>
#include <data_bind_json_provider.h>
#include <data_bind_message_plan.h>
#include <data_bind_native.h>
#include <data_bind_xml_provider.h>
#include <data_bind_yaml_provider.h>
#include <json_parser.h>
#include <cyaml/cyaml.h>
#include <cyaml/cyaml_json_adapter.h>
#include <xml_parser/xml_parser.h>

#include <cserde/writer.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mapper_plan_entry_s {
    char *stable_id;
    DataBind *codec;
    DataBindMessagePlan *plan;
    struct mapper_plan_entry_s *next;
} mapper_plan_entry_t;

typedef struct mapper_text_builder_s {
    char *data;
    size_t length;
    size_t capacity;
} mapper_text_builder_t;

typedef struct mapper_json_frame_s {
    json_value_t *container;
    int is_map;
    int expect_key;
    char *key;
    size_t key_len;
} mapper_json_frame_t;

typedef struct mapper_json_writer_s {
    json_value_t *root;
    mapper_json_frame_t frames[32];
    size_t depth;
} mapper_json_writer_t;

static void mapper_set_error(char *error, size_t error_len, const char *fmt, ...) {
    va_list args;
    if (!error || error_len == 0u) return;
    va_start(args, fmt);
    vsnprintf(error, error_len, fmt, args);
    va_end(args);
}

static int mapper_builder_reserve(mapper_text_builder_t *builder, size_t extra) {
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

static int mapper_builder_append_n(
    mapper_text_builder_t *builder, const char *text, size_t length) {
    if (!builder || (!text && length != 0u) ||
        !mapper_builder_reserve(builder, length))
        return 0;
    if (length != 0u) memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 1;
}

static int mapper_builder_append(mapper_text_builder_t *builder, const char *text) {
    return text ? mapper_builder_append_n(builder, text, strlen(text)) : 0;
}

static int mapper_builder_printf(mapper_text_builder_t *builder,
                                 const char *format, ...) {
    va_list args;
    va_list copy;
    int needed;
    if (!builder || !format) return 0;
    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !mapper_builder_reserve(builder, (size_t)needed)) {
        va_end(args);
        return 0;
    }
    vsnprintf(builder->data + builder->length,
              builder->capacity - builder->length, format, args);
    va_end(args);
    builder->length += (size_t)needed;
    return 1;
}

static int mapper_builder_quoted(mapper_text_builder_t *builder, vstr value) {
    if (!mapper_builder_append(builder, "\"")) return 0;
    for (size_t i = 0; i < value.len; ++i) {
        const unsigned char ch = (unsigned char)value.data[i];
        switch (ch) {
        case '\\':
            if (!mapper_builder_append(builder, "\\\\")) return 0;
            break;
        case '"':
            if (!mapper_builder_append(builder, "\\\"")) return 0;
            break;
        case '\n':
            if (!mapper_builder_append(builder, "\\n")) return 0;
            break;
        case '\r':
            if (!mapper_builder_append(builder, "\\r")) return 0;
            break;
        case '\t':
            if (!mapper_builder_append(builder, "\\t")) return 0;
            break;
        default:
            if (ch < 0x20u) return 0;
            if (!mapper_builder_append_n(builder, (const char *)&value.data[i], 1u))
                return 0;
            break;
        }
    }
    return mapper_builder_append(builder, "\"");
}

static const char *mapper_databind_type(const cmeta_data_desc *data) {
    if (!cmeta_data_desc_valid(data)) return NULL;
    if (cmeta_data_desc_equal(data, &cmeta_data_int64)) return "int64";
    if (cmeta_data_desc_equal(data, &cmeta_data_double)) return "double";
    if (cmeta_data_desc_equal(data, &cmeta_data_bool)) return "bool";
    if (data->kind == CMETA_DATA_STRING && data->storage_type != NULL)
        return "string";
    return NULL;
}

static int mapper_schema_default(
    mapper_text_builder_t *builder, const cmeta_data_desc *data,
    const exprtk_value_t *value) {
    if (!builder || !data || !value) return 0;
    if (cmeta_data_desc_equal(data, &cmeta_data_int64) &&
        value->type == EXPRTK_VAL_INTEGER)
        return mapper_builder_printf(builder, " default %lld",
                                     (long long)value->data.integer);
    if (cmeta_data_desc_equal(data, &cmeta_data_double) &&
        value->type == EXPRTK_VAL_NUMBER)
        return mapper_builder_printf(builder, " default %.17g",
                                     value->data.number);
    if (cmeta_data_desc_equal(data, &cmeta_data_bool) &&
        value->type == EXPRTK_VAL_BOOL)
        return mapper_builder_append(
            builder, value->data.boolean ? " default true" : " default false");
    if (data->kind == CMETA_DATA_STRING &&
        value->type == EXPRTK_VAL_STRING) {
        if (!mapper_builder_append(builder, " default ")) return 0;
        return mapper_builder_quoted(builder, value->data.string);
    }
    return 0;
}

static int mapper_build_schema(
    exprtk_class_t *klass, char **out_schema, size_t *out_length,
    char *error, size_t error_len) {
    mapper_text_builder_t builder = {0};
    const cmeta_data_desc *object_data;

    if (out_schema) *out_schema = NULL;
    if (out_length) *out_length = 0u;
    if (!klass || !out_schema || !out_length ||
        !exprtk_class_finalize_cmeta_data(klass)) {
        mapper_set_error(error, error_len, "mapper: class reflection is unavailable");
        return 0;
    }

    object_data = exprtk_class_cmeta_data(klass);
    if (!object_data || object_data->kind != CMETA_DATA_STRUCT) {
        mapper_set_error(error, error_len, "mapper: invalid class CMeta data surface");
        return 0;
    }

    if (!mapper_builder_append(
            &builder, "schema TurboScriptMapper [version(1)]; message ") ||
        !mapper_builder_append(&builder, klass->name) ||
        !mapper_builder_append(&builder, " {")) {
        free(builder.data);
        mapper_set_error(error, error_len, "mapper: out of memory building DataBind contract");
        return 0;
    }

    /* DataBind/TBE requires canonical record ordering: fixed-width scalar
     * fields precede variable data. MessagePlan binds object fields by name, so
     * this projection order never changes TurboScript class/slot semantics. */
    for (size_t i = 0; i < klass->instance_field_count; ++i) {
        const cmeta_data_desc *data =
            klass->cmeta_data_fields ? klass->cmeta_data_fields[i].value : NULL;
        if (!mapper_databind_type(data) || !klass->instance_field_names[i]) {
            free(builder.data);
            return -1; /* TurboScript dynamic-value domain, not a typed plan. */
        }
    }

    for (unsigned pass = 0u; pass < 2u; ++pass) {
        for (size_t i = 0; i < klass->instance_field_count; ++i) {
            const cmeta_data_desc *data = klass->cmeta_data_fields[i].value;
            const char *type = mapper_databind_type(data);
            const char *name = klass->instance_field_names[i];
            const unsigned variable = data->kind == CMETA_DATA_STRING ? 1u : 0u;

            if (variable != pass) continue;
            if (!mapper_builder_append(&builder, " ") ||
                !mapper_builder_append(&builder, type) ||
                !mapper_builder_append(&builder, " ") ||
                !mapper_builder_append(&builder, name)) {
                free(builder.data);
                mapper_set_error(error, error_len,
                                 "mapper: out of memory building DataBind fields");
                return 0;
            }

            if (klass->instance_field_has_default &&
                klass->instance_field_has_default[i]) {
                if (!klass->instance_field_defaults ||
                    !mapper_schema_default(
                        &builder, data, &klass->instance_field_defaults[i])) {
                    free(builder.data);
                    mapper_set_error(
                        error, error_len,
                        "mapper: unsupported typed class default for '%s'", name);
                    return 0;
                }
            }

            if (!mapper_builder_append(&builder, ";")) {
                free(builder.data);
                mapper_set_error(error, error_len,
                                 "mapper: out of memory building DataBind contract");
                return 0;
            }
        }
    }

    if (!mapper_builder_append(&builder, " }")) {
        free(builder.data);
        mapper_set_error(error, error_len,
                         "mapper: out of memory finalizing DataBind contract");
        return 0;
    }

    *out_schema = builder.data;
    *out_length = builder.length;
    return 1;
}

static mapper_plan_entry_t *mapper_plan_find(
    mapper_ctx_t *ctx, const cmeta_data_desc *data) {
    mapper_plan_entry_t *entry = ctx ? (mapper_plan_entry_t *)ctx->plans : NULL;
    while (entry) {
        if (entry->plan && entry->codec && entry->stable_id &&
            data && data->stable_id &&
            strcmp(entry->stable_id, data->stable_id) == 0 &&
            cmeta_data_desc_equal(
                data_bind_message_plan_object_data(entry->plan), data))
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static int mapper_plan_get(
    mapper_ctx_t *ctx, exprtk_class_t *klass, mapper_plan_entry_t **out,
    char *error, size_t error_len) {
    const cmeta_data_desc *data;
    mapper_plan_entry_t *entry;
    DataBindError bind_error = DATA_BIND_ERROR_INIT;
    DataBindMessagePlanDiagnostic diagnostic =
        DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;
    char *schema = NULL;
    size_t schema_len = 0u;
    int schema_status;
    DataBindStatus status;

    if (out) *out = NULL;
    if (!ctx || !klass || !out || !exprtk_class_finalize_cmeta_data(klass)) {
        mapper_set_error(error, error_len, "mapper: invalid reflected class");
        return 0;
    }

    data = exprtk_class_cmeta_data(klass);
    entry = mapper_plan_find(ctx, data);
    if (entry) {
        *out = entry;
        return 1;
    }

    schema_status = mapper_build_schema(
        klass, &schema, &schema_len, error, error_len);
    if (schema_status <= 0) return schema_status;

    entry = (mapper_plan_entry_t *)calloc(1u, sizeof(*entry));
    if (!entry) {
        free(schema);
        mapper_set_error(error, error_len, "mapper: out of memory caching MessagePlan");
        return 0;
    }

    status = data_bind_create_from_text(
        schema, schema_len, &entry->codec, &bind_error);
    free(schema);
    if (status != DATA_BIND_OK) {
        mapper_set_error(
            error, error_len, "mapper: DataBind contract compile failed: %s",
            bind_error.message[0] ? bind_error.message : "invalid contract");
        free(entry);
        return 0;
    }

    status = data_bind_message_plan_compile_object(
        entry->codec, klass->name, data, &entry->plan, &diagnostic);
    if (status != DATA_BIND_OK) {
        mapper_set_error(
            error, error_len, "mapper: MessagePlan compile failed: %s",
            diagnostic.message[0] ? diagnostic.message : "invalid object binding");
        data_bind_free(entry->codec);
        free(entry);
        return 0;
    }

    if (data->stable_id) {
        const size_t stable_len = strlen(data->stable_id);
        entry->stable_id = (char *)malloc(stable_len + 1u);
        if (entry->stable_id)
            memcpy(entry->stable_id, data->stable_id, stable_len + 1u);
    }
    if (!entry->stable_id) {
        data_bind_message_plan_free(entry->plan);
        data_bind_free(entry->codec);
        free(entry);
        mapper_set_error(error, error_len, "mapper: out of memory caching class identity");
        return 0;
    }

    entry->next = (mapper_plan_entry_t *)ctx->plans;
    ctx->plans = entry;
    *out = entry;
    return 1;
}

static const DataBindFormatProvider *mapper_format_provider(DataBindFormat format) {
    switch (format) {
    case DATA_BIND_FORMAT_JSON: return data_bind_json_format_provider();
    case DATA_BIND_FORMAT_YAML: return data_bind_yaml_format_provider();
    case DATA_BIND_FORMAT_XML: return data_bind_xml_format_provider();
    default: return NULL;
    }
}

static int mapper_xml_root_matches(
    exprtk_class_t *klass, const char *data, size_t len) {
    salts_xml_document document = {0};
    salts_xml_node root;
    salts_xml_string_view name;
    int matches = 0;
    if (!klass || !klass->name || !data) return 0;
    if (salts_xml_parse(&document, data, len, NULL, NULL) != SALTS_XML_OK)
        return 0;
    root = salts_xml_document_root(&document);
    name = salts_xml_node_qualified_name(root);
    matches = root.impl && name.data &&
              strlen(klass->name) == name.size &&
              memcmp(klass->name, name.data, name.size) == 0;
    salts_xml_document_destroy(&document);
    return matches;
}

static void mapper_native_options(
    mapper_ctx_t *ctx, size_t field_count, size_t payload_bytes,
    void *workspace, size_t workspace_bytes, DataBindNativeOptions *out) {
    size_t external = ctx && ctx->env ? ctx->env->max_external_value_bytes : 0u;
    *out = (DataBindNativeOptions)DATA_BIND_NATIVE_OPTIONS_INIT;
    out->workspace = workspace;
    out->workspace_bytes = workspace_bytes;
    out->max_depth = 32u;
    out->max_items = field_count > (SIZE_MAX - 64u) / 8u
                         ? SIZE_MAX
                         : field_count * 8u + 64u;
    out->max_owned_bytes = external != 0u
                               ? external
                               : payload_bytes > (1024u * 1024u)
                                     ? payload_bytes
                                     : 1024u * 1024u;
}

int mapper_databind_decode(
    mapper_ctx_t *ctx, exprtk_class_t *klass, DataBindFormat format,
    const char *data, size_t len, exprtk_value_t *out,
    char *error, size_t error_len) {
    mapper_plan_entry_t *entry = NULL;
    const DataBindFormatProvider *provider;
    DataBindFormatReader lease = DATA_BIND_FORMAT_READER_INIT;
    DataBindError format_error = DATA_BIND_ERROR_INIT;
    DataBindMessagePlanDiagnostic diagnostic =
        DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;
    DataBindNativeOptions options;
    cmeta_object_ref object = CMETA_OBJECT_REF_INIT;
    exprtk_value_t instance_value;
    unsigned char *workspace = NULL;
    size_t workspace_bytes;
    DataBindStatus status;
    int admitted;

    if (out) memset(out, 0, sizeof(*out));
    if (!ctx || !ctx->env || !klass || !data || !out) return 0;

    admitted = mapper_plan_get(ctx, klass, &entry, error, error_len);
    if (admitted <= 0) return admitted;

    provider = mapper_format_provider(format);
    if (!provider) {
        mapper_set_error(error, error_len, "mapper: unsupported DataBind format");
        return 0;
    }
    if (format == DATA_BIND_FORMAT_XML &&
        !mapper_xml_root_matches(klass, data, len)) {
        mapper_set_error(error, error_len,
                         "mapper.read_xml: root does not match class");
        return 0;
    }

    status = data_bind_format_reader_open(
        provider, data, len, 32u, &lease, &format_error);
    if (status != DATA_BIND_OK) {
        mapper_set_error(
            error, error_len, "mapper: format reader failed: %s",
            format_error.message[0] ? format_error.message : "invalid input");
        return 0;
    }

    instance_value = exprtk_oop_instantiate_class_value(
        exprtk_val_class(klass), klass->name, 0u, NULL, ctx->env);
    if (instance_value.type != EXPRTK_VAL_INSTANCE ||
        !instance_value.data.instance_val.instance) {
        data_bind_format_reader_close(&lease);
        mapper_set_error(error, error_len, "mapper: class instantiation failed");
        return 0;
    }

    workspace_bytes = 8192u +
        data_bind_message_plan_field_count(entry->plan) * 256u;
    workspace = (unsigned char *)calloc(1u, workspace_bytes);
    if (!workspace) {
        data_bind_format_reader_close(&lease);
        exprtk_instance_destroy(instance_value.data.instance_val.instance);
        mapper_set_error(error, error_len, "mapper: out of memory for DataBind workspace");
        return 0;
    }
    mapper_native_options(
        ctx, data_bind_message_plan_field_count(entry->plan), len,
        workspace, workspace_bytes, &options);

    if (exprtk_instance_borrow_cmeta_object(
            instance_value.data.instance_val.instance, &object) != CMETA_OK) {
        free(workspace);
        data_bind_format_reader_close(&lease);
        exprtk_instance_destroy(instance_value.data.instance_val.instance);
        mapper_set_error(error, error_len, "mapper: CMeta object borrow failed");
        return 0;
    }

    status = data_bind_message_plan_decode_object(
        entry->plan, &options, lease.reader, &object, NULL, &diagnostic);
    cmeta_object_release(&object);
    free(workspace);
    data_bind_format_reader_close(&lease);

    if (status != DATA_BIND_OK) {
        exprtk_instance_destroy(instance_value.data.instance_val.instance);
        mapper_set_error(
            error, error_len, "mapper: %s",
            diagnostic.message[0] ? diagnostic.message : "typed decode failed");
        return 0;
    }

    *out = instance_value;
    return 1;
}

static void mapper_json_writer_clear(mapper_json_writer_t *sink) {
    if (!sink) return;
    for (size_t i = 0; i < sink->depth; ++i) {
        free(sink->frames[i].key);
        sink->frames[i].key = NULL;
    }
    if (sink->root) json_free(sink->root);
    memset(sink, 0, sizeof(*sink));
}

static int mapper_json_attach(mapper_json_writer_t *sink, json_value_t *value) {
    mapper_json_frame_t *frame;
    if (!sink || !value) return 0;
    if (sink->depth == 0u) {
        if (sink->root) return 0;
        sink->root = value;
        return 1;
    }
    frame = &sink->frames[sink->depth - 1u];
    if (frame->is_map) {
        if (frame->expect_key || !frame->key) return 0;
        if (!json_object_add_n(
                frame->container, frame->key, frame->key_len, value))
            return 0;
        free(frame->key);
        frame->key = NULL;
        frame->key_len = 0u;
        frame->expect_key = 1;
        return 1;
    }
    return json_array_add_checked(frame->container, value) ? 1 : 0;
}

static cserde_status mapper_json_writer_write(
    void *context, const cserde_token *token) {
    mapper_json_writer_t *sink = (mapper_json_writer_t *)context;
    mapper_json_frame_t *frame;
    json_value_t *value = NULL;

    if (!sink || !token || !cserde_token_valid(token))
        return CSERDE_INVALID_ARGUMENT;

    if (sink->depth != 0u) {
        frame = &sink->frames[sink->depth - 1u];
        if (frame->is_map && frame->expect_key) {
            if (token->kind == CSERDE_MAP_END) {
                if (frame->key) return CSERDE_INVALID_STATE;
                --sink->depth;
                return CSERDE_OK;
            }
            if (token->kind != CSERDE_STRING)
                return CSERDE_INVALID_TOKEN;
            frame->key = (char *)malloc(token->value.slice.size + 1u);
            if (!frame->key) return CSERDE_SINK_ERROR;
            memcpy(frame->key, token->value.slice.data, token->value.slice.size);
            frame->key[token->value.slice.size] = '\0';
            frame->key_len = token->value.slice.size;
            frame->expect_key = 0;
            return CSERDE_OK;
        }
        if (!frame->is_map && token->kind == CSERDE_ARRAY_END) {
            --sink->depth;
            return CSERDE_OK;
        }
    }

    switch (token->kind) {
    case CSERDE_MAP_BEGIN:
        value = json_create_object();
        break;
    case CSERDE_ARRAY_BEGIN:
        value = json_create_array();
        break;
    case CSERDE_NULL:
        value = json_create_null();
        break;
    case CSERDE_BOOL:
        value = json_create_bool(token->value.boolean);
        break;
    case CSERDE_SINT:
        value = json_create_int64(token->value.sint);
        break;
    case CSERDE_UINT:
        value = json_create_uint64(token->value.uint);
        break;
    case CSERDE_FLOAT:
        value = json_create_number(token->value.floating);
        break;
    case CSERDE_STRING:
        value = json_create_string_n(
            (const char *)token->value.slice.data, token->value.slice.size);
        break;
    case CSERDE_BYTES:
    case CSERDE_MAP_END:
    case CSERDE_ARRAY_END:
    default:
        return CSERDE_UNSUPPORTED;
    }
    if (!value) return CSERDE_SINK_ERROR;
    if (!mapper_json_attach(sink, value)) {
        if (sink->root != value) json_free(value);
        return CSERDE_SINK_ERROR;
    }

    if (token->kind == CSERDE_MAP_BEGIN ||
        token->kind == CSERDE_ARRAY_BEGIN) {
        if (sink->depth >= sizeof(sink->frames) / sizeof(sink->frames[0]))
            return CSERDE_LIMIT_EXCEEDED;
        frame = &sink->frames[sink->depth++];
        memset(frame, 0, sizeof(*frame));
        frame->container = value;
        frame->is_map = token->kind == CSERDE_MAP_BEGIN;
        frame->expect_key = frame->is_map ? 1 : 0;
    }

    return CSERDE_OK;
}

static cserde_status mapper_json_writer_finish(void *context) {
    mapper_json_writer_t *sink = (mapper_json_writer_t *)context;
    return sink && sink->root && sink->depth == 0u
               ? CSERDE_OK
               : CSERDE_INVALID_STATE;
}

static const cserde_writer_ops MAPPER_JSON_WRITER_OPS = {
    sizeof(cserde_writer_ops),
    CSERDE_WRITER_OPS_ABI_VERSION,
    mapper_json_writer_write,
    mapper_json_writer_finish
};

static int mapper_copy_owned_text(
    const char *source, size_t length, char **out, size_t *out_length) {
    char *copy;
    if (!source || !out || !out_length || length == SIZE_MAX) return 0;
    copy = (char *)malloc(length + 1u);
    if (!copy) return 0;
    memcpy(copy, source, length);
    copy[length] = '\0';
    *out = copy;
    *out_length = length;
    return 1;
}

static int mapper_json_scalar_text(
    const json_value_t *value, char **out, size_t *out_len) {
    char buffer[64];
    const char *text = NULL;
    size_t length = 0u;
    int n;

    if (!value || !out || !out_len) return 0;
    switch (json_type(value)) {
    case JSON_STRING:
        return mapper_copy_owned_text(
            json_string(value), json_string_len(value), out, out_len);
    case JSON_BOOL:
        text = json_bool(value) ? "true" : "false";
        return mapper_copy_owned_text(text, strlen(text), out, out_len);
    case JSON_NULL:
        return mapper_copy_owned_text("", 0u, out, out_len);
    case JSON_NUMBER:
        text = json_number_text(value, &length);
        if (text) return mapper_copy_owned_text(text, length, out, out_len);
        n = snprintf(buffer, sizeof(buffer), "%.17g", json_number(value));
        return n >= 0 && (size_t)n < sizeof(buffer) &&
               mapper_copy_owned_text(buffer, (size_t)n, out, out_len);
    default:
        return 0;
    }
}

static int mapper_json_to_xml_value(
    salts_xml_node parent, const char *name, const json_value_t *value) {
    json_type_t type;
    salts_xml_node child = {0};
    if (!parent.impl || !name || !value) return 0;
    type = json_type(value);

    if (type == JSON_ARRAY) {
        for (size_t i = 0; i < json_array_size(value); ++i)
            if (!mapper_json_to_xml_value(
                    parent, name, json_array_get(value, i)))
                return 0;
        return 1;
    }

    if (salts_xml_node_add_element(parent, name, &child) != SALTS_XML_OK)
        return 0;

    if (type == JSON_OBJECT) {
        for (size_t i = 0; i < json_object_size(value); ++i) {
            const char *key = json_object_key(value, i);
            if (!key || !mapper_json_to_xml_value(
                            child, key, json_object_value(value, i)))
                return 0;
        }
        return 1;
    }

    {
        char *text = NULL;
        size_t text_len = 0u;
        int ok;
        if (!mapper_json_scalar_text(value, &text, &text_len)) return 0;
        if (memchr(text, '\0', text_len) != NULL) {
            free(text);
            return 0;
        }
        ok = salts_xml_node_set_text(child, text) == SALTS_XML_OK;
        free(text);
        return ok;
    }
}

static int mapper_render_json(
    json_value_t *json, DataBindFormat format, const char *root_name,
    char **out_data, size_t *out_len) {
    if (format == DATA_BIND_FORMAT_JSON) {
        char *text = json_serialize(json, out_len);
        int ok = text && mapper_copy_owned_text(text, *out_len, out_data, out_len);
        if (text) json_serialize_free(text);
        return ok;
    }

    if (format == DATA_BIND_FORMAT_YAML) {
        cyaml_doc_t *yaml = cyaml_doc_from_json_value(json);
        char *text;
        size_t length = 0u;
        int ok;
        if (!yaml) return 0;
        text = cyaml_emit(yaml, NULL, &length);
        cyaml_free(yaml);
        ok = text && mapper_copy_owned_text(text, length, out_data, out_len);
        free(text);
        return ok;
    }

    if (format == DATA_BIND_FORMAT_XML) {
        salts_xml_document document = {0};
        salts_xml_node root;
        char *text;
        size_t length = 0u;
        int ok = 1;
        if (!root_name || json_type(json) != JSON_OBJECT ||
            salts_xml_document_create(&document, root_name) != SALTS_XML_OK)
            return 0;
        root = salts_xml_document_root(&document);
        for (size_t i = 0; ok && i < json_object_size(json); ++i) {
            const char *key = json_object_key(json, i);
            ok = key && mapper_json_to_xml_value(
                           root, key, json_object_value(json, i));
        }
        text = ok ? salts_xml_document_serialize(&document, &length) : NULL;
        salts_xml_document_destroy(&document);
        ok = text && mapper_copy_owned_text(text, length, out_data, out_len);
        if (text) salts_xml_owned_string_free(text);
        return ok;
    }

    return 0;
}

int mapper_databind_encode(
    mapper_ctx_t *ctx, exprtk_instance_t *instance, DataBindFormat format,
    char **out_data, size_t *out_len, char *error, size_t error_len) {
    mapper_plan_entry_t *entry = NULL;
    DataBindMessagePlanDiagnostic diagnostic =
        DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;
    DataBindNativeOptions options;
    mapper_json_writer_t sink = {0};
    cserde_writer writer = {0};
    cmeta_object_ref object = CMETA_OBJECT_REF_INIT;
    unsigned char *workspace = NULL;
    size_t workspace_bytes;
    DataBindStatus status;
    int admitted;

    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0u;
    if (!ctx || !ctx->env || !instance || !instance->klass ||
        !out_data || !out_len)
        return 0;

    admitted = mapper_plan_get(
        ctx, instance->klass, &entry, error, error_len);
    if (admitted <= 0) return admitted;

    workspace_bytes = 8192u +
        data_bind_message_plan_field_count(entry->plan) * 256u;
    workspace = (unsigned char *)calloc(1u, workspace_bytes);
    if (!workspace) {
        mapper_set_error(error, error_len, "mapper: out of memory for encode workspace");
        return 0;
    }
    mapper_native_options(
        ctx, data_bind_message_plan_field_count(entry->plan), 0u,
        workspace, workspace_bytes, &options);

    if (exprtk_instance_borrow_cmeta_object(instance, &object) != CMETA_OK ||
        cserde_writer_init(&writer, &MAPPER_JSON_WRITER_OPS, &sink) != CSERDE_OK) {
        free(workspace);
        mapper_set_error(error, error_len, "mapper: failed to initialize typed encode");
        return 0;
    }

    status = data_bind_message_plan_encode_object(
        entry->plan, &options, &object, NULL, &writer, &diagnostic);
    if (status == DATA_BIND_OK && cserde_writer_finish(&writer) != CSERDE_OK)
        status = DATA_BIND_ERR_RUNTIME;
    cmeta_object_release(&object);
    free(workspace);

    if (status != DATA_BIND_OK || !sink.root) {
        mapper_set_error(
            error, error_len, "mapper: %s",
            diagnostic.message[0] ? diagnostic.message : "typed encode failed");
        mapper_json_writer_clear(&sink);
        return 0;
    }

    if (!mapper_render_json(
            sink.root, format, instance->klass->name, out_data, out_len)) {
        mapper_set_error(error, error_len, "mapper: format serialization failed");
        mapper_json_writer_clear(&sink);
        return 0;
    }

    mapper_json_writer_clear(&sink);
    return 1;
}

void mapper_databind_clear(mapper_ctx_t *ctx) {
    mapper_plan_entry_t *entry;
    mapper_plan_entry_t *next;
    if (!ctx) return;
    entry = (mapper_plan_entry_t *)ctx->plans;
    while (entry) {
        next = entry->next;
        free(entry->stable_id);
        data_bind_message_plan_free(entry->plan);
        data_bind_free(entry->codec);
        free(entry);
        entry = next;
    }
    ctx->plans = NULL;
}
