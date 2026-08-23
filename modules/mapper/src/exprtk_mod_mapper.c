/**
 * @file exprtk_mod_mapper.c
 * @brief Jackson-style class-first JSON/YAML/XML mapping.
 */
#include "mapper.h"
#include "exprtk_class.h"
#include "exprtk_module.h"
#include "turbo_parser.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAPPER_ERROR_CAP 256

static exprtk_value_t mapper_null(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static exprtk_value_t mapper_error(exprtk_env_t *env, const char *format, ...) {
    va_list args;
    if (env) {
        va_start(args, format);
        vsnprintf(env->error_msg, sizeof(env->error_msg), format, args);
        va_end(args);
        env->aborted = 1;
    }
    return mapper_null();
}

static int mapper_type_is_builtin(const char *type) {
    static const char *const names[] = {
        "any", "number", "float", "int", "int64", "integer", "bool", "boolean",
        "string", "bytes", "map", "object", "list", "array", "class", "instance",
        "function", "null"
    };
    size_t i;
    if (!type) return 1;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strcmp(type, names[i]) == 0) return 1;
    return 0;
}

static exprtk_class_t *mapper_resolve_class(exprtk_env_t *env, const char *type) {
    exprtk_value_t value;
    if (!env || !type || mapper_type_is_builtin(type)) return NULL;
    value = exprtk_env_get(env, type);
    return value.type == EXPRTK_VAL_CLASS ? value.data.class_val.klass : NULL;
}

static int mapper_copy_string(exprtk_env_t *env, const char *data, size_t len,
                              exprtk_value_t *out) {
    exprtk_value_t borrowed;
    if (!env || !out || (!data && len != 0)) return 0;
    borrowed = exprtk_val_str(vstr_from_buf(data ? data : "", len));
    return exprtk_value_copy_to_env(borrowed, env, out) == 0;
}

static int mapper_parse_i64(const char *data, size_t len, int64_t *out) {
    char *buffer;
    char *end;
    long long value;
    if (!data || !out || len == 0 || len > 64) return 0;
    buffer = (char *)malloc(len + 1);
    if (!buffer) return 0;
    memcpy(buffer, data, len);
    buffer[len] = '\0';
    errno = 0;
    value = strtoll(buffer, &end, 10);
    if (errno == ERANGE || end != buffer + len) {
        free(buffer);
        return 0;
    }
    *out = (int64_t)value;
    free(buffer);
    return 1;
}

static int mapper_parse_bool(const char *data, size_t len, int *out) {
    if (!data || !out) return 0;
    if ((len == 4 && memcmp(data, "true", 4) == 0) ||
        (len == 1 && data[0] == '1')) {
        *out = 1;
        return 1;
    }
    if ((len == 5 && memcmp(data, "false", 5) == 0) ||
        (len == 1 && data[0] == '0')) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int mapper_parse_double(const char *data, size_t len, double *out) {
    char *buffer;
    char *end;
    if (!data || !out || len == 0 || len > 128) return 0;
    buffer = (char *)malloc(len + 1);
    if (!buffer) return 0;
    memcpy(buffer, data, len);
    buffer[len] = '\0';
    errno = 0;
    *out = strtod(buffer, &end);
    if (errno == ERANGE || end != buffer + len || !isfinite(*out)) {
        free(buffer);
        return 0;
    }
    free(buffer);
    return 1;
}

static int mapper_field_is_numeric(const char *type) {
    return type && (strcmp(type, "number") == 0 || strcmp(type, "float") == 0 ||
                    strcmp(type, "int") == 0 || strcmp(type, "int64") == 0 ||
                    strcmp(type, "integer") == 0);
}

static int mapper_json_to_value(const json_value_t *node, const char *declared_type,
                                mapper_ctx_t *ctx, exprtk_value_t *out);

static int mapper_json_to_instance(const json_value_t *node, exprtk_class_t *klass,
                                   mapper_ctx_t *ctx, exprtk_value_t *out) {
    exprtk_value_t instance_value;
    size_t i;
    if (!node || !klass || !ctx || !ctx->env ||
        turbo_json_type(node) != TURBO_JSON_OBJECT || !out) return 0;

    instance_value = exprtk_oop_instantiate_class_value(
        exprtk_val_class(klass), klass->name, 0, NULL, ctx->env);
    if (instance_value.type != EXPRTK_VAL_INSTANCE) return 0;

    for (i = 0; i < turbo_json_object_size(node); ++i) {
        const char *name = turbo_json_object_key(node, i);
        json_value_t *child = turbo_json_object_value(node, i);
        const char *field_type;
        exprtk_value_t converted;
        char field_error[MAPPER_ERROR_CAP];

        if (!name || !child || !exprtk_class_has_instance_field(klass, name)) {
            mapper_error(ctx->env, "mapper: unknown field '%s' in class '%s'",
                         name ? name : "<null>", klass->name ? klass->name : "<unknown>");
            exprtk_instance_destroy(instance_value.data.instance_val.instance);
            return 0;
        }
        field_type = exprtk_class_get_instance_field_type(klass, name);
        memset(&converted, 0, sizeof(converted));
        converted.type = EXPRTK_VAL_NULL;
        if (!mapper_json_to_value(child, field_type, ctx, &converted)) {
            exprtk_instance_destroy(instance_value.data.instance_val.instance);
            return 0;
        }
        memset(field_error, 0, sizeof(field_error));
        if (!exprtk_instance_set_field_checked(instance_value.data.instance_val.instance,
                                               name, converted, field_error,
                                               sizeof(field_error))) {
            exprtk_value_destroy(&converted);
            mapper_error(ctx->env, "mapper: %s", field_error);
            exprtk_instance_destroy(instance_value.data.instance_val.instance);
            return 0;
        }
        exprtk_value_destroy(&converted);
    }
    *out = instance_value;
    return 1;
}

static int mapper_json_to_object(const json_value_t *node, mapper_ctx_t *ctx,
                                 exprtk_value_t *out) {
    size_t i;
    exprtk_value_t object = exprtk_val_object();
    if (!object.data.map.htab) return 0;
    for (i = 0; i < turbo_json_object_size(node); ++i) {
        const char *key = turbo_json_object_key(node, i);
        json_value_t *child = turbo_json_object_value(node, i);
        exprtk_value_t converted = mapper_null();
        if (!key || !child || !mapper_json_to_value(child, NULL, ctx, &converted) ||
            exprtk_map_set(&object, key, converted) != 0) {
            exprtk_value_destroy(&converted);
            exprtk_value_destroy(&object);
            return 0;
        }
        exprtk_value_destroy(&converted);
    }
    *out = object;
    return 1;
}

static int mapper_json_to_list(const json_value_t *node, mapper_ctx_t *ctx,
                               exprtk_value_t *out) {
    size_t i;
    exprtk_value_t list = exprtk_val_list_empty();
    for (i = 0; i < turbo_json_array_size(node); ++i) {
        exprtk_value_t converted = mapper_null();
        if (!mapper_json_to_value(turbo_json_array_get(node, i), NULL, ctx, &converted) ||
            exprtk_list_push(&list, converted) != 0) {
            exprtk_value_destroy(&converted);
            exprtk_value_destroy(&list);
            return 0;
        }
        exprtk_value_destroy(&converted);
    }
    *out = list;
    return 1;
}

static int mapper_json_to_value(const json_value_t *node, const char *declared_type,
                                mapper_ctx_t *ctx, exprtk_value_t *out) {
    turbo_json_type_t kind;
    if (!node || !ctx || !ctx->env || !out) return 0;
    kind = turbo_json_type(node);
    if (kind == TURBO_JSON_NULL) {
        *out = mapper_null();
        return 1;
    }
    if (kind == TURBO_JSON_BOOL) {
        if (declared_type && strcmp(declared_type, "bool") != 0 &&
            strcmp(declared_type, "boolean") != 0 && strcmp(declared_type, "any") != 0)
            return (mapper_error(ctx->env, "mapper: expected '%s', received bool",
                                 declared_type), 0);
        *out = exprtk_val_bool(turbo_json_bool(node));
        return 1;
    }
    if (kind == TURBO_JSON_STRING) {
        if (declared_type && strcmp(declared_type, "string") != 0 &&
            strcmp(declared_type, "any") != 0)
            return (mapper_error(ctx->env, "mapper: expected '%s', received string",
                                 declared_type), 0);
        return mapper_copy_string(ctx->env, turbo_json_string(node),
                                  turbo_json_string_len(node), out);
    }
    if (kind == TURBO_JSON_NUMBER) {
        const char *text;
        size_t len = 0;
        int64_t integer;
        double number;
        if (declared_type && (strcmp(declared_type, "int") == 0 ||
                              strcmp(declared_type, "int64") == 0 ||
                              strcmp(declared_type, "integer") == 0)) {
            text = turbo_json_number_text(node, &len);
            if (!text || !mapper_parse_i64(text, len, &integer))
                return (mapper_error(ctx->env, "mapper: invalid integer"), 0);
            *out = exprtk_val_int(integer);
            return 1;
        }
        if (declared_type && !mapper_field_is_numeric(declared_type) &&
            strcmp(declared_type, "any") != 0)
            return (mapper_error(ctx->env, "mapper: expected '%s', received number",
                                 declared_type), 0);
        number = turbo_json_number(node);
        *out = exprtk_val_num(number);
        return 1;
    }
    if (kind == TURBO_JSON_ARRAY) {
        if (declared_type && strcmp(declared_type, "list") != 0 &&
            strcmp(declared_type, "array") != 0 && strcmp(declared_type, "any") != 0)
            return (mapper_error(ctx->env, "mapper: expected '%s', received array",
                                 declared_type), 0);
        return mapper_json_to_list(node, ctx, out);
    }
    if (kind == TURBO_JSON_OBJECT) {
        exprtk_class_t *klass = mapper_resolve_class(ctx->env, declared_type);
        if (klass) return mapper_json_to_instance(node, klass, ctx, out);
        if (declared_type && strcmp(declared_type, "map") != 0 &&
            strcmp(declared_type, "object") != 0 && strcmp(declared_type, "any") != 0)
            return (mapper_error(ctx->env, "mapper: expected '%s', received object",
                                 declared_type), 0);
        return mapper_json_to_object(node, ctx, out);
    }
    return (mapper_error(ctx->env, "mapper: unsupported JSON value"), 0);
}

static int mapper_json_from_value(const exprtk_value_t *value, json_value_t **out);

static int mapper_json_from_instance(const exprtk_instance_t *instance, json_value_t **out) {
    json_value_t *object;
    size_t i;
    if (!instance || !instance->klass || !out) return 0;
    object = turbo_json_create_object();
    if (!object) return 0;
    for (i = 0; i < instance->klass->instance_field_count; ++i) {
        json_value_t *child = NULL;
        exprtk_value_t value = mapper_null();
        const char *name = instance->klass->instance_field_names[i];
        if (!name || !exprtk_instance_get_field((exprtk_instance_t *)instance, name, &value) ||
            !mapper_json_from_value(&value, &child) ||
            !turbo_json_object_add_checked(object, name, child)) {
            if (child) turbo_free_json(&child);
            turbo_free_json(&object);
            return 0;
        }
    }
    *out = object;
    return 1;
}

static int mapper_json_from_value(const exprtk_value_t *value, json_value_t **out) {
    size_t i;
    if (!value || !out) return 0;
    switch (value->type) {
        case EXPRTK_VAL_NULL: *out = turbo_json_create_null(); return *out != NULL;
        case EXPRTK_VAL_BOOL: *out = turbo_json_create_bool(value->data.boolean != 0); return *out != NULL;
        case EXPRTK_VAL_NUMBER: *out = turbo_json_create_number(value->data.number); return *out != NULL;
        case EXPRTK_VAL_INTEGER: *out = turbo_json_create_int64(value->data.integer); return *out != NULL;
        case EXPRTK_VAL_STRING:
            *out = turbo_json_create_string_n(value->data.string.data, value->data.string.len);
            return *out != NULL;
        case EXPRTK_VAL_INSTANCE:
            return mapper_json_from_instance(value->data.instance_val.instance, out);
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT: {
            exprtk_map_iter_t iterator = exprtk_map_iter_begin(value);
            const char *key;
            exprtk_value_t child_value;
            json_value_t *object = turbo_json_create_object();
            if (!object) return 0;
            while (exprtk_map_iter_next(&iterator, &key, &child_value)) {
                json_value_t *child = NULL;
                if (!mapper_json_from_value(&child_value, &child) ||
                    !turbo_json_object_add_checked(object, key, child)) {
                    if (child) turbo_free_json(&child);
                    turbo_free_json(&object);
                    return 0;
                }
            }
            *out = object;
            return 1;
        }
        case EXPRTK_VAL_LIST:
        case EXPRTK_VAL_SET: {
            json_value_t *array = turbo_json_create_array();
            if (!array) return 0;
            for (i = 0; i < value->data.list.count; ++i) {
                json_value_t *child = NULL;
                if (!mapper_json_from_value(&value->data.list.items[i], &child) ||
                    !turbo_json_array_add_checked(array, child)) {
                    if (child) turbo_free_json(&child);
                    turbo_free_json(&array);
                    return 0;
                }
            }
            *out = array;
            return 1;
        }
        default:
            return 0;
    }
}

static exprtk_value_t mapper_return_text(exprtk_env_t *env, char *text, size_t len,
                                         void (*free_fn)(char *)) {
    exprtk_value_t result;
    if (!text) return mapper_error(env, "mapper: serialization failed");
    if (!mapper_copy_string(env, text, len, &result)) {
        free_fn(text);
        return mapper_error(env, "mapper: failed to retain serialized text");
    }
    free_fn(text);
    return result;
}

static exprtk_value_t fn_read_json(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, void *user_data) {
    mapper_ctx_t *ctx = (mapper_ctx_t *)user_data;
    turbo_json_doc_t *doc = NULL;
    exprtk_value_t result = mapper_null();
    exprtk_class_t *klass;
    if (!ctx || !env || argc != 2 || args[0].type != EXPRTK_VAL_CLASS ||
        args[1].type != EXPRTK_VAL_STRING || !args[0].data.class_val.klass)
        return mapper_error(env, "mapper.read_json expects (Class, string)");
    klass = args[0].data.class_val.klass;
    if (turbo_parse_json((const uint8_t *)args[1].data.string.data,
                         args[1].data.string.len, &doc) != 0 || !doc)
        return mapper_error(env, "mapper.read_json: invalid JSON");
    if (!mapper_json_to_instance(doc, klass, ctx, &result)) {
        turbo_free_json(&doc);
        return mapper_null();
    }
    turbo_free_json(&doc);
    return result;
}

static exprtk_value_t fn_read_yaml(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, void *user_data) {
    mapper_ctx_t *ctx = (mapper_ctx_t *)user_data;
    turbo_yaml_doc_t *yaml = NULL;
    json_value_t *json = NULL;
    exprtk_value_t result = mapper_null();
    exprtk_class_t *klass;
    if (!ctx || !env || argc != 2 || args[0].type != EXPRTK_VAL_CLASS ||
        args[1].type != EXPRTK_VAL_STRING || !args[0].data.class_val.klass)
        return mapper_error(env, "mapper.read_yaml expects (Class, string)");
    klass = args[0].data.class_val.klass;
    if (turbo_parse_yaml((const uint8_t *)args[1].data.string.data,
                         args[1].data.string.len, &yaml) != 0 || !yaml)
        return mapper_error(env, "mapper.read_yaml: invalid YAML");
    json = turbo_yaml_node_to_json(yaml, turbo_yaml_root(yaml));
    if (!json || !mapper_json_to_instance(json, klass, ctx, &result)) {
        if (json) turbo_free_json(&json);
        turbo_free_yaml(&yaml);
        return mapper_null();
    }
    turbo_free_json(&json);
    turbo_free_yaml(&yaml);
    return result;
}

static int mapper_xml_to_value(turbo_xml_node_t *node, const char *declared_type,
                               mapper_ctx_t *ctx, exprtk_value_t *out);

static int mapper_xml_add_instance(turbo_xml_node_t *parent,
                                   const exprtk_instance_t *instance);

static int mapper_xml_to_instance(turbo_xml_node_t *node, exprtk_class_t *klass,
                                  mapper_ctx_t *ctx, exprtk_value_t *out) {
    exprtk_value_t instance_value;
    size_t i;
    if (!node || !klass || !ctx || !out) return 0;
    instance_value = exprtk_oop_instantiate_class_value(
        exprtk_val_class(klass), klass->name, 0, NULL, ctx->env);
    if (instance_value.type != EXPRTK_VAL_INSTANCE) return 0;
    for (i = 0; i < klass->instance_field_count; ++i) {
        const char *name = klass->instance_field_names[i];
        const char *type = exprtk_class_get_instance_field_type(klass, name);
        char query[MAPPER_ERROR_CAP];
        int query_len;
        turbo_xml_node_t *child;
        exprtk_value_t value;
        char error[MAPPER_ERROR_CAP];
        query_len = name ? snprintf(query, sizeof(query), "<%s>/", name) : -1;
        if (!name || query_len < 0 || (size_t)query_len >= sizeof(query)) {
            mapper_error(ctx->env, "mapper: invalid XML field name");
            exprtk_instance_destroy(instance_value.data.instance_val.instance);
            return 0;
        }
        child = turbo_xml_find(node, query);
        if (!child) continue;
        memset(&value, 0, sizeof(value));
        value.type = EXPRTK_VAL_NULL;
        if (!mapper_xml_to_value(child, type, ctx, &value)) {
            exprtk_instance_destroy(instance_value.data.instance_val.instance);
            return 0;
        }
        if (!exprtk_instance_set_field_checked(instance_value.data.instance_val.instance,
                                               name, value, error, sizeof(error))) {
            exprtk_value_destroy(&value);
            mapper_error(ctx->env, "mapper: %s", error);
            exprtk_instance_destroy(instance_value.data.instance_val.instance);
            return 0;
        }
        exprtk_value_destroy(&value);
    }
    *out = instance_value;
    return 1;
}

static int mapper_xml_to_value(turbo_xml_node_t *node, const char *declared_type,
                               mapper_ctx_t *ctx, exprtk_value_t *out) {
    char *text;
    size_t len;
    exprtk_class_t *klass;
    int boolean;
    int boolean_ok;
    int64_t integer;
    double number;
    if (!node || !ctx || !out) return 0;
    klass = mapper_resolve_class(ctx->env, declared_type);
    if (klass) return mapper_xml_to_instance(node, klass, ctx, out);
    text = turbo_xml_text_dup(node);
    if (!text) return 0;
    len = strlen(text);
    if (!declared_type || strcmp(declared_type, "string") == 0 ||
        strcmp(declared_type, "any") == 0) {
        int ok = mapper_copy_string(ctx->env, text, len, out);
        free(text);
        return ok;
    }
    if (strcmp(declared_type, "bool") == 0 || strcmp(declared_type, "boolean") == 0) {
        boolean_ok = mapper_parse_bool(text, len, &boolean);
        free(text);
        if (!boolean_ok) return (mapper_error(ctx->env, "mapper: invalid boolean"), 0);
        *out = exprtk_val_bool(boolean);
        return 1;
    }
    if (strcmp(declared_type, "int") == 0 || strcmp(declared_type, "int64") == 0 ||
        strcmp(declared_type, "integer") == 0) {
        int ok = mapper_parse_i64(text, len, &integer);
        free(text);
        if (!ok) return (mapper_error(ctx->env, "mapper: invalid integer"), 0);
        *out = exprtk_val_int(integer);
        return 1;
    }
    if (strcmp(declared_type, "number") == 0 || strcmp(declared_type, "float") == 0) {
        int ok = mapper_parse_double(text, len, &number);
        free(text);
        if (!ok) return (mapper_error(ctx->env, "mapper: invalid number"), 0);
        *out = exprtk_val_num(number);
        return 1;
    }
    free(text);
    return (mapper_error(ctx->env, "mapper: XML field '%s' is not scalar", declared_type), 0);
}

static exprtk_value_t fn_read_xml(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, void *user_data) {
    mapper_ctx_t *ctx = (mapper_ctx_t *)user_data;
    turbo_xml_doc_t *doc = NULL;
    turbo_xml_node_t *root;
    exprtk_value_t result = mapper_null();
    exprtk_class_t *klass;
    if (!ctx || !env || argc != 2 || args[0].type != EXPRTK_VAL_CLASS ||
        args[1].type != EXPRTK_VAL_STRING || !args[0].data.class_val.klass)
        return mapper_error(env, "mapper.read_xml expects (Class, string)");
    klass = args[0].data.class_val.klass;
    if (turbo_parse_xml((const uint8_t *)args[1].data.string.data,
                        args[1].data.string.len, &doc) != 0 || !doc)
        return mapper_error(env, "mapper.read_xml: invalid XML");
    root = turbo_xml_root_element(doc);
    if (!root || (klass->name && turbo_xml_node_name(root) &&
                  strcmp(klass->name, turbo_xml_node_name(root)) != 0) ||
        !mapper_xml_to_instance(root, klass, ctx, &result)) {
        turbo_free_xml(&doc);
        if (!env->aborted) mapper_error(env, "mapper.read_xml: root does not match class");
        return mapper_null();
    }
    turbo_free_xml(&doc);
    return result;
}

static exprtk_value_t mapper_write_json_value(exprtk_value_t value, exprtk_env_t *env) {
    json_value_t *json = NULL;
    char *text;
    size_t len = 0;
    if (!mapper_json_from_value(&value, &json))
        return mapper_error(env, "mapper.write_json: unsupported value");
    text = turbo_json_serialize(json, &len);
    turbo_free_json(&json);
    return mapper_return_text(env, text, len, turbo_json_serialize_free);
}

static exprtk_value_t fn_write_json(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, void *user_data) {
    (void)user_data;
    if (!env || argc != 1 || args[0].type != EXPRTK_VAL_INSTANCE)
        return mapper_error(env, "mapper.write_json expects (instance)");
    return mapper_write_json_value(args[0], env);
}

static exprtk_value_t fn_write_yaml(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, void *user_data) {
    json_value_t *json = NULL;
    turbo_yaml_doc_t *yaml;
    char *text;
    size_t len = 0;
    (void)user_data;
    if (!env || argc != 1 || args[0].type != EXPRTK_VAL_INSTANCE)
        return mapper_error(env, "mapper.write_yaml expects (instance)");
    if (!mapper_json_from_value(&args[0], &json))
        return mapper_error(env, "mapper.write_yaml: unsupported value");
    yaml = turbo_yaml_from_json(json);
    turbo_free_json(&json);
    if (!yaml) return mapper_error(env, "mapper.write_yaml: conversion failed");
    text = turbo_yaml_emit(yaml, &len);
    turbo_free_yaml(&yaml);
    return mapper_return_text(env, text, len, turbo_yaml_serialize_free);
}

static int mapper_xml_add_value(turbo_xml_node_t *parent, const char *name,
                                const exprtk_value_t *value) {
    turbo_xml_node_t *child;
    char buffer[128];
    if (!parent || !name || !value) return 0;
    if (value->type == EXPRTK_VAL_LIST || value->type == EXPRTK_VAL_SET) {
        size_t i;
        for (i = 0; i < value->data.list.count; ++i)
            if (!mapper_xml_add_value(parent, name, &value->data.list.items[i])) return 0;
        return 1;
    }
    child = turbo_xml_add_element(parent, name);
    if (!child) return 0;
    if (value->type == EXPRTK_VAL_INSTANCE)
        return mapper_xml_add_instance(child, value->data.instance_val.instance);
    if (value->type == EXPRTK_VAL_STRING)
        return turbo_xml_set_text(child, value->data.string.data) == 0;
    if (value->type == EXPRTK_VAL_INTEGER)
        snprintf(buffer, sizeof(buffer), "%lld", (long long)value->data.integer);
    else if (value->type == EXPRTK_VAL_NUMBER)
        snprintf(buffer, sizeof(buffer), "%.17g", value->data.number);
    else if (value->type == EXPRTK_VAL_BOOL)
        snprintf(buffer, sizeof(buffer), "%s", value->data.boolean ? "true" : "false");
    else if (value->type == EXPRTK_VAL_NULL)
        buffer[0] = '\0';
    else
        return 0;
    return turbo_xml_set_text(child, buffer) == 0;
}

static int mapper_xml_add_instance(turbo_xml_node_t *parent,
                                   const exprtk_instance_t *instance) {
    size_t i;
    if (!parent || !instance || !instance->klass) return 0;
    for (i = 0; i < instance->klass->instance_field_count; ++i) {
        exprtk_value_t value;
        const char *name = instance->klass->instance_field_names[i];
        if (!name || !exprtk_instance_get_field((exprtk_instance_t *)instance, name, &value))
            continue;
        if (!mapper_xml_add_value(parent, name, &value)) return 0;
    }
    return 1;
}

static exprtk_value_t fn_write_xml(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, void *user_data) {
    turbo_xml_doc_t *doc;
    turbo_xml_node_t *root;
    char *text;
    size_t len = 0;
    (void)user_data;
    if (!env || argc != 1 || args[0].type != EXPRTK_VAL_INSTANCE ||
        !args[0].data.instance_val.instance ||
        !args[0].data.instance_val.instance->klass)
        return mapper_error(env, "mapper.write_xml expects (instance)");
    doc = turbo_xml_create_document(args[0].data.instance_val.instance->klass->name);
    if (!doc) return mapper_error(env, "mapper.write_xml: allocation failed");
    root = turbo_xml_root_element(doc);
    if (!root || !mapper_xml_add_instance(root, args[0].data.instance_val.instance)) {
        turbo_free_xml(&doc);
        return mapper_error(env, "mapper.write_xml: unsupported value");
    }
    text = turbo_xml_serialize(doc, &len);
    turbo_free_xml(&doc);
    return mapper_return_text(env, text, len, turbo_xml_serialize_free);
}

void *mapper_ctx_create(void) {
    return calloc(1, sizeof(mapper_ctx_t));
}

void mapper_ctx_destroy(void *ctx) {
    free(ctx);
}

void mapper_load(void *ctx_value, void *env_value, void *scratch) {
    mapper_ctx_t *ctx = (mapper_ctx_t *)ctx_value;
    exprtk_env_t *env = (exprtk_env_t *)env_value;
    (void)scratch;
    if (!ctx || !env) return;
    ctx->env = env;
    exprtk_env_register_func(env, "mapper.read_json", fn_read_json, ctx);
    exprtk_env_register_func(env, "mapper.read_yaml", fn_read_yaml, ctx);
    exprtk_env_register_func(env, "mapper.read_xml", fn_read_xml, ctx);
    exprtk_env_register_func(env, "mapper.write_json", fn_write_json, ctx);
    exprtk_env_register_func(env, "mapper.write_yaml", fn_write_yaml, ctx);
    exprtk_env_register_func(env, "mapper.write_xml", fn_write_xml, ctx);
}
