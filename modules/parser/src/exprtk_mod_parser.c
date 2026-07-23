/**
 * @file exprtk_mod_parser.c
 * @brief Parser module functions for TurboScript
 */
#include "parser_ctx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>

#define PARSER_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})
#define PARSER_CMD_MAX_ARGS 64

typedef enum {
    PARSER_CMD_KIND_FLAG = 0,
    PARSER_CMD_KIND_STRING,
    PARSER_CMD_KIND_INTEGER,
    PARSER_CMD_KIND_FLOAT
} parser_cmd_kind_t;

typedef struct {
    parser_cmd_kind_t kind;
    const char *name;
    union {
        bool *flag;
        char **str;
        int64_t *integer;
        double *floating;
    } value;
} parser_cmd_arg_t;

#define PARSER_ERROR(ud, msg)                                             \
    do {                                                                  \
        if ((ud) && (ud)->ctx) {                                          \
            strncpy((ud)->ctx->error_msg, (msg),                          \
                    sizeof((ud)->ctx->error_msg) - 1);                    \
            (ud)->ctx->error_msg[sizeof((ud)->ctx->error_msg) - 1] = '\0'; \
        }                                                                 \
        if ((ud) && (ud)->env) {                                          \
            (ud)->env->aborted = 1;                                       \
        }                                                                 \
    } while (0)

static exprtk_value_t parser_null(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static exprtk_value_t parser_string_value(exprtk_env_t *env, const char *text) {
    exprtk_value_t value;
    exprtk_value_t borrowed;

    if (!env) return exprtk_val_str(tstr_v_from_buf("", 0));
    if (!text) text = "";
    borrowed = exprtk_val_str(tstr_v_from_buf(text, strlen(text)));
    if (exprtk_value_copy_to_env(borrowed, env, &value) != 0) return PARSER_ZERO;
    return value;
}

static inline char *parser_arena_cstr(mem_pool_t *arena, tstr_v sv) {
    char *buf = (char *)mem_alloc(arena, sv.len + 1);
    if (buf) {
        memcpy(buf, sv.data, sv.len);
        buf[sv.len] = '\0';
    }
    return buf;
}

static int parser_value_truthy(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_NUMBER) return value.data.number != 0.0;
    if (value.type == EXPRTK_VAL_INTEGER) return value.data.integer != 0;
    if (value.type == EXPRTK_VAL_BOOL) return value.data.boolean != 0;
    if (value.type == EXPRTK_VAL_STRING) return value.data.string.len > 0;
    if (value.type == EXPRTK_VAL_BYTES) return value.data.bytes.len > 0;
    if (value.type == EXPRTK_VAL_VECTOR) return value.data.vector.size > 0;
    if (value.type == EXPRTK_VAL_LIST) return value.data.list.count > 0;
    if (exprtk_value_is_object_like(&value)) return exprtk_map_count(&value) > 0;
    return 0;
}

static exprtk_value_t parser_read_file(parser_ud_t *ud, exprtk_value_t path_value) {
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    char *path;
    FILE *file;
    long size;
    char *buffer;
    size_t read_size;

    if (!ud || !ud->env || path_value.type != EXPRTK_VAL_STRING) return result;
    path = parser_arena_cstr(ud->scratch, path_value.data.string);
    if (!path) return result;
    file = fopen(path, "rb");
    if (!file) return result;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return result;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return result;
    }
    buffer = (char *)mem_alloc(ud->scratch, (size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return result;
    }
    read_size = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[read_size] = '\0';
    if (exprtk_value_copy_to_env(exprtk_val_str(tstr_v_from_buf(buffer, read_size)),
                                 ud->env, &result) != 0)
        return PARSER_ZERO;
    return result;
}

static exprtk_value_t fn_ini_get(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_ini_t *ini = NULL;
    char *section;
    char *key;
    const char *value;
    exprtk_value_t result = parser_null();

    if (!ud || !ud->env || argc < 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        return result;
    }
    if (turbo_parse_ini((const uint8_t *)args[0].data.string.data,
                        args[0].data.string.len, &ini) != 0 || !ini) {
        return result;
    }
    section = parser_arena_cstr(ud->scratch, args[1].data.string);
    key = parser_arena_cstr(ud->scratch, args[2].data.string);
    if (section && key) {
        value = turbo_ini_get(ini, section, key);
        if (value) result = parser_string_value(ud->env, value);
        else if (argc >= 4 && args[3].type == EXPRTK_VAL_STRING) result = args[3];
    }
    {
        void *ini_ptr = ini;
        turbo_free_ini(&ini_ptr);
    }
    return result;
}

static exprtk_value_t fn_dotenv_load(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    char *path;
    int overwrite = 1;

    if (!ud || !ud->env || argc < 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(-1.0);
    if (argc >= 2) overwrite = parser_value_truthy(args[1]);
    path = parser_arena_cstr(ud->scratch, args[0].data.string);
    if (!path) return exprtk_val_num(-1.0);
    return exprtk_val_num((double)turbo_dotenv_load(path, overwrite != 0));
}

static exprtk_value_t fn_dotenv_load_default(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int overwrite = 1;

    if (!ud || !ud->env) return exprtk_val_num(-1.0);
    if (argc >= 1) overwrite = parser_value_truthy(args[0]);
    return exprtk_val_num((double)turbo_dotenv_load_default(overwrite != 0));
}

/* =========================================================================
 * TOML
 * ========================================================================= */

static exprtk_value_t parser_toml_timestamp_to_expr(parser_ud_t *ud, turbo_toml_value_t value) {
    exprtk_value_t result = exprtk_val_object();
    turbo_toml_timestamp_t ts = value.u.ts;

    if (!ud) return result;

    exprtk_map_set(&result, "year", exprtk_val_int(ts.year));
    exprtk_map_set(&result, "month", exprtk_val_int(ts.month));
    exprtk_map_set(&result, "day", exprtk_val_int(ts.day));
    exprtk_map_set(&result, "hour", exprtk_val_int(ts.hour));
    exprtk_map_set(&result, "minute", exprtk_val_int(ts.minute));
    exprtk_map_set(&result, "second", exprtk_val_int(ts.second));
    exprtk_map_set(&result, "millisecond", exprtk_val_int(ts.millisec));
    exprtk_map_set(&result, "tz_offset", exprtk_val_int(ts.tz));
    return result;
}

static exprtk_value_t parser_toml_scalar_to_expr(parser_ud_t *ud, const turbo_toml_t *table,
                                                const char *key) {
    turbo_toml_value_t value;
    exprtk_value_t result = parser_null();

    if (!ud || !table || !key) return result;

    value = turbo_toml_string(table, key);
    if (value.ok) {
        char *text = value.u.s;
        result = parser_string_value(ud->env, text ? text : "");
        if (text) free(text);
        return result;
    }

    value = turbo_toml_bool(table, key);
    if (value.ok) return exprtk_val_bool(value.u.b);

    value = turbo_toml_int(table, key);
    if (value.ok) return exprtk_val_int((int64_t)value.u.i);

    value = turbo_toml_double(table, key);
    if (value.ok) return exprtk_val_num(value.u.d);

    value = turbo_toml_timestamp(table, key);
    if (value.ok) return parser_toml_timestamp_to_expr(ud, value);

    return result;
}

static exprtk_value_t parser_toml_array_to_expr(parser_ud_t *ud, const turbo_toml_array_t *array);

static exprtk_value_t parser_toml_table_to_expr(parser_ud_t *ud, const turbo_toml_t *table) {
    exprtk_value_t result = exprtk_val_object();
    int len;
    int i;

    if (!ud || !table) return result;
    len = turbo_toml_len(table);
    for (i = 0; i < len; ++i) {
        int key_len = 0;
        const char *raw_key = turbo_toml_key(table, i, &key_len);
        if (!raw_key || key_len < 0) continue;

        char *key = (char *)mem_alloc(ud->scratch, (size_t)key_len + 1);
        if (!key) continue;
        memcpy(key, raw_key, (size_t)key_len);
        key[key_len] = '\0';

        if (turbo_toml_table(table, key)) {
            turbo_toml_t *child = turbo_toml_table(table, key);
            exprtk_value_t converted = parser_toml_table_to_expr(ud, child);
            exprtk_map_set(&result, key, converted);
            exprtk_value_destroy(&converted);
            continue;
        }

        if (turbo_toml_array(table, key)) {
            turbo_toml_array_t *child = turbo_toml_array(table, key);
            exprtk_value_t converted = parser_toml_array_to_expr(ud, child);
            exprtk_map_set(&result, key, converted);
            exprtk_value_destroy(&converted);
            continue;
        }

        exprtk_value_t converted = parser_toml_scalar_to_expr(ud, table, key);
        exprtk_map_set(&result, key, converted);
        exprtk_value_destroy(&converted);
    }

    return result;
}

static exprtk_value_t parser_toml_array_to_expr(parser_ud_t *ud, const turbo_toml_array_t *array) {
    exprtk_value_t result = exprtk_val_list_empty();
    int len;
    int i;

    if (!ud || !array) return result;
    len = turbo_toml_array_len(array);

    for (i = 0; i < len; ++i) {
        turbo_toml_value_t value;

        if (turbo_toml_array_table(array, i)) {
            turbo_toml_t *table = turbo_toml_array_table(array, i);
            exprtk_value_t converted = parser_toml_table_to_expr(ud, table);
            exprtk_list_push(&result, converted);
            exprtk_value_destroy(&converted);
            continue;
        }

        if (turbo_toml_array_array(array, i)) {
            turbo_toml_array_t *sub = turbo_toml_array_array(array, i);
            exprtk_value_t converted = parser_toml_array_to_expr(ud, sub);
            exprtk_list_push(&result, converted);
            exprtk_value_destroy(&converted);
            continue;
        }

        value = turbo_toml_array_string(array, i);
        if (value.ok) {
            char *text = value.u.s;
            exprtk_list_push(&result, parser_string_value(ud->env, text ? text : ""));
            if (text) free(text);
            continue;
        }

        value = turbo_toml_array_bool(array, i);
        if (value.ok) {
            exprtk_list_push(&result, exprtk_val_bool(value.u.b));
            continue;
        }

        value = turbo_toml_array_int(array, i);
        if (value.ok) {
            exprtk_list_push(&result, exprtk_val_int((int64_t)value.u.i));
            continue;
        }

        value = turbo_toml_array_double(array, i);
        if (value.ok) {
            exprtk_list_push(&result, exprtk_val_num(value.u.d));
            continue;
        }

        value = turbo_toml_array_timestamp(array, i);
        if (value.ok) {
            exprtk_list_push(&result, parser_toml_timestamp_to_expr(ud, value));
            continue;
        }
    }

    return result;
}

static exprtk_value_t parser_toml_parse_text(parser_ud_t *ud, tstr_v text) {
    turbo_toml_t *root = NULL;
    exprtk_value_t result = parser_null();
    void *ptr;

    if (!ud) return result;
    if (turbo_parse_toml((const uint8_t *)text.data, text.len, &root) != 0 || !root) {
        return result;
    }
    result = parser_toml_table_to_expr(ud, root);
    ptr = root;
    turbo_free_toml(&ptr);
    return result;
}

static exprtk_value_t fn_toml_parse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        return parser_null();
    }
    return parser_toml_parse_text(ud, args[0].data.string);
}

static exprtk_value_t fn_toml_parse_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t text;

    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_STRING) return parser_null();
    text = parser_read_file(ud, args[0]);
    if (text.type != EXPRTK_VAL_STRING || text.data.string.len == 0) return parser_null();
    return parser_toml_parse_text(ud, text.data.string);
}

static int parser_cmd_add_section_args(parser_ud_t *ud, turbo_cmd_parser_t *parser,
                                      const char *field, parser_cmd_kind_t kind,
                                      exprtk_value_t spec, parser_cmd_arg_t *args,
                                      size_t *count, bool *bool_values,
                                      char **string_values, int64_t *integer_values,
                                      double *float_values) {
    exprtk_value_t list = exprtk_map_get(&spec, field);
    size_t i;
    if (list.type == EXPRTK_VAL_NULL) return 0;
    if (list.type != EXPRTK_VAL_LIST && list.type != EXPRTK_VAL_SET) return -1;

    for (i = 0; i < list.data.list.count; ++i) {
        exprtk_value_t entry;
        exprtk_value_t name_v;
        exprtk_value_t short_v;
        exprtk_value_t desc_v;
        exprtk_value_t required_v;
        const char *name;
        const char *short_name;
        const char *desc;

        if (*count >= PARSER_CMD_MAX_ARGS) return -1;

        entry = exprtk_list_get(&list, i);
        if (!exprtk_value_is_object_like(&entry)) continue;

        name_v = exprtk_map_get(&entry, "name");
        if (name_v.type != EXPRTK_VAL_STRING || name_v.data.string.len == 0) continue;
        name = parser_arena_cstr(&ud->env->arena, name_v.data.string);
        if (!name) return -1;

        short_v = exprtk_map_get(&entry, "short");
        desc_v = exprtk_map_get(&entry, "desc");
        required_v = exprtk_map_get(&entry, "required");
        short_name = short_v.type == EXPRTK_VAL_STRING ? short_v.data.string.data : "";
        desc = desc_v.type == EXPRTK_VAL_STRING ? desc_v.data.string.data : "";

        switch (kind) {
        case PARSER_CMD_KIND_FLAG:
            turbo_cmd_add_flag(parser, &bool_values[*count], name, short_name, desc);
            args[*count].kind = PARSER_CMD_KIND_FLAG;
            args[*count].name = name;
            args[*count].value.flag = &bool_values[*count];
            break;
        case PARSER_CMD_KIND_STRING:
            string_values[*count] = NULL;
            if (parser_value_truthy(required_v)) {
                turbo_cmd_add_required_string(parser, &string_values[*count], name, desc);
            } else {
                turbo_cmd_add_string(parser, &string_values[*count], name, short_name, desc);
            }
            args[*count].kind = PARSER_CMD_KIND_STRING;
            args[*count].name = name;
            args[*count].value.str = &string_values[*count];
            break;
        case PARSER_CMD_KIND_INTEGER:
            integer_values[*count] = 0;
            if (parser_value_truthy(required_v)) {
                turbo_cmd_add_required_integer(parser, &integer_values[*count], name, desc);
            } else {
                turbo_cmd_add_integer(parser, &integer_values[*count], name, short_name, desc);
            }
            args[*count].kind = PARSER_CMD_KIND_INTEGER;
            args[*count].name = name;
            args[*count].value.integer = &integer_values[*count];
            break;
        case PARSER_CMD_KIND_FLOAT:
            float_values[*count] = 0.0;
            turbo_cmd_add_float(parser, &float_values[*count], name, short_name, desc);
            args[*count].kind = PARSER_CMD_KIND_FLOAT;
            args[*count].name = name;
            args[*count].value.floating = &float_values[*count];
            break;
        default:
            return -1;
        }

        ++(*count);
    }

    return 0;
}

static exprtk_value_t fn_cmd_parse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_cmd_parser_t *parser = NULL;
    parser_cmd_arg_t arg_values[PARSER_CMD_MAX_ARGS];
    bool bool_values[PARSER_CMD_MAX_ARGS];
    char *string_values[PARSER_CMD_MAX_ARGS];
    int64_t int_values[PARSER_CMD_MAX_ARGS];
    double float_values[PARSER_CMD_MAX_ARGS];
    size_t arg_count = 0;
    exprtk_value_t out = exprtk_val_object();
    exprtk_value_t values = exprtk_val_object();
    exprtk_value_t argv_v;
    exprtk_value_t spec;
    const char *app_name = "app";
    const char *version = "1.0";
    bool colors = false;
    size_t i;
    int parse_argc = 0;
    char **parse_argv = NULL;

    if (!ud || argc < 2) return parser_null();
    if (!exprtk_value_is_object_like(&args[0])) return parser_null();
    if (args[1].type != EXPRTK_VAL_LIST && args[1].type != EXPRTK_VAL_SET) return parser_null();

    spec = args[0];
    argv_v = args[1];
    if (argc >= 3 && args[2].type == EXPRTK_VAL_STRING) {
        app_name = parser_arena_cstr(ud->scratch, args[2].data.string);
        if (!app_name) app_name = "app";
    }
    if (argc >= 4 && args[3].type == EXPRTK_VAL_STRING) {
        version = parser_arena_cstr(ud->scratch, args[3].data.string);
        if (!version) version = "1.0";
    }
    if (argc >= 5) colors = parser_value_truthy(args[4]);

    memset(arg_values, 0, sizeof(arg_values));
    memset(string_values, 0, sizeof(string_values));

    parse_argv = mem_alloc(ud->scratch,
                           sizeof(char *) * (argv_v.data.list.count ? argv_v.data.list.count : 1));
    if (!parse_argv) return parser_null();

    for (i = 0; i < argv_v.data.list.count; ++i) {
        exprtk_value_t arg_v = exprtk_list_get(&argv_v, i);
        if (arg_v.type != EXPRTK_VAL_STRING) return parser_null();
        parse_argv[i] = parser_arena_cstr(ud->scratch, arg_v.data.string);
        if (!parse_argv[i]) return parser_null();
        ++parse_argc;
    }

    parser = turbo_cmd_create(app_name, version);
    if (!parser) {
        exprtk_map_set(&out, "ok", exprtk_val_bool(false));
        exprtk_map_set(&out, "values", values);
        return out;
    }

    if (parser_cmd_add_section_args(ud, parser, "flags", PARSER_CMD_KIND_FLAG, spec,
                                   arg_values, &arg_count, bool_values,
                                   string_values, int_values, float_values) < 0 ||
        parser_cmd_add_section_args(ud, parser, "strings", PARSER_CMD_KIND_STRING, spec,
                                   arg_values, &arg_count, bool_values,
                                   string_values, int_values, float_values) < 0 ||
        parser_cmd_add_section_args(ud, parser, "integers", PARSER_CMD_KIND_INTEGER, spec,
                                   arg_values, &arg_count, bool_values,
                                   string_values, int_values, float_values) < 0 ||
        parser_cmd_add_section_args(ud, parser, "floats", PARSER_CMD_KIND_FLOAT, spec,
                                   arg_values, &arg_count, bool_values,
                                   string_values, int_values, float_values) < 0) {
        turbo_cmd_destroy(parser);
        return parser_null();
    }

    turbo_cmd_parse(parser, (int)parse_argc, parse_argv, colors);

    for (i = 0; i < arg_count; ++i) {
        switch (arg_values[i].kind) {
        case PARSER_CMD_KIND_FLAG:
            exprtk_map_set(&values, arg_values[i].name,
                           exprtk_val_bool(*arg_values[i].value.flag ? true : false));
            break;
        case PARSER_CMD_KIND_STRING:
            if (arg_values[i].value.str && *arg_values[i].value.str) {
                exprtk_map_set(&values, arg_values[i].name,
                               parser_string_value(ud->env, *arg_values[i].value.str));
            } else {
                exprtk_map_set(&values, arg_values[i].name, parser_null());
            }
            break;
        case PARSER_CMD_KIND_INTEGER:
            exprtk_map_set(&values, arg_values[i].name,
                           exprtk_val_int(*arg_values[i].value.integer));
            break;
        case PARSER_CMD_KIND_FLOAT:
            exprtk_map_set(&values, arg_values[i].name,
                           exprtk_val_num(*arg_values[i].value.floating));
            break;
        default:
            break;
        }
    }

    turbo_cmd_destroy(parser);
    exprtk_map_set(&out, "ok", exprtk_val_bool(true));
    exprtk_map_set(&out, "values", values);
    return out;
}

static exprtk_value_t parser_datetime_to_map(exprtk_env_t *env, const turbo_datetime_t *dt) {
    exprtk_value_t result = exprtk_val_object();
    time_t ts;

    if (!dt) return parser_null();
    exprtk_map_set(&result, "year", exprtk_val_int(dt->year));
    exprtk_map_set(&result, "month", exprtk_val_int(dt->month));
    exprtk_map_set(&result, "day", exprtk_val_int(dt->day));
    exprtk_map_set(&result, "hour", exprtk_val_int(dt->hour));
    exprtk_map_set(&result, "minute", exprtk_val_int(dt->minute));
    exprtk_map_set(&result, "second", exprtk_val_int(dt->second));
    exprtk_map_set(&result, "millisecond", exprtk_val_int(dt->millisecond));
    exprtk_map_set(&result, "tz_offset", exprtk_val_int(dt->tz_offset));
    exprtk_map_set(&result, "has_tz", exprtk_val_int(dt->has_tz ? 1 : 0));
    exprtk_map_set(&result, "day_of_week", exprtk_val_int(dt->day_of_week));

    ts = turbo_datetime_to_time(dt);
    if (ts != (time_t)-1) exprtk_map_set(&result, "timestamp", exprtk_val_num((double)ts));
    (void)env;
    return result;
}

static int parser_datetime_get_int(const exprtk_value_t *map, const char *key, int *out) {
    exprtk_value_t value;
    if (!exprtk_value_is_object_like(map) || !key || !out) return 0;
    value = exprtk_map_get(map, key);
    if (value.type == EXPRTK_VAL_INTEGER) {
        *out = (int)value.data.integer;
        return 1;
    }
    if (value.type == EXPRTK_VAL_NUMBER) {
        *out = (int)value.data.number;
        return 1;
    }
    return 0;
}

static int parser_datetime_from_map(const exprtk_value_t *map, turbo_datetime_t *dt) {
    if (!exprtk_value_is_object_like(map) || !dt) return 0;
    memset(dt, 0, sizeof(*dt));
    dt->day_of_week = -1;
    if (!parser_datetime_get_int(map, "year", &dt->year) ||
        !parser_datetime_get_int(map, "month", &dt->month) ||
        !parser_datetime_get_int(map, "day", &dt->day)) {
        return 0;
    }
    (void)parser_datetime_get_int(map, "hour", &dt->hour);
    (void)parser_datetime_get_int(map, "minute", &dt->minute);
    (void)parser_datetime_get_int(map, "second", &dt->second);
    (void)parser_datetime_get_int(map, "millisecond", &dt->millisecond);
    (void)parser_datetime_get_int(map, "tz_offset", &dt->tz_offset);
    (void)parser_datetime_get_int(map, "has_tz", &dt->has_tz);
    (void)parser_datetime_get_int(map, "day_of_week", &dt->day_of_week);
    return 1;
}

/**
 * datetime.parse(text: string) -> datetime|null
 * Parse common datetime strings into a native datetime value.
 */
static exprtk_value_t fn_datetime_parse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_datetime_t dt;

    if (!ud || !ud->env || argc != 1 || args[0].type != EXPRTK_VAL_STRING) return parser_null();
    if (turbo_parse_datetime(args[0].data.string.data, args[0].data.string.len, &dt) != 0)
        return parser_null();
    return exprtk_val_datetime(dt);
}

/**
 * datetime.to_time(value: string|datetime|object|map) -> number
 * Convert datetime text, native datetime, or compatible field data to Unix epoch seconds.
 */
static exprtk_value_t fn_datetime_to_time(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    turbo_datetime_t dt;
    time_t ts;
    (void)user_data;

    if (argc != 1) return exprtk_val_num(-1.0);
    if (args[0].type == EXPRTK_VAL_STRING) {
        if (turbo_parse_datetime(args[0].data.string.data, args[0].data.string.len, &dt) != 0)
            return exprtk_val_num(-1.0);
    } else if (args[0].type == EXPRTK_VAL_DATETIME) {
        dt = args[0].data.datetime;
    } else if (exprtk_value_is_object_like(&args[0])) {
        if (!parser_datetime_from_map(&args[0], &dt)) return exprtk_val_num(-1.0);
    } else {
        return exprtk_val_num(-1.0);
    }

    ts = turbo_datetime_to_time(&dt);
    return exprtk_val_num((double)ts);
}

/**
 * datetime.format_rfc822(timestamp: number) -> string
 * Format seconds since Unix epoch as an HTTP/RFC-822 date.
 */
static exprtk_value_t fn_datetime_format_rfc822(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    char buf[64];
    time_t ts;

    if (!ud || !ud->env || argc != 1) return exprtk_val_str(tstr_v_from_buf("", 0));
    if (args[0].type == EXPRTK_VAL_INTEGER)
        ts = (time_t)args[0].data.integer;
    else if (args[0].type == EXPRTK_VAL_NUMBER)
        ts = (time_t)args[0].data.number;
    else if (args[0].type == EXPRTK_VAL_DATETIME)
        ts = turbo_datetime_to_time(&args[0].data.datetime);
    else
        return exprtk_val_str(tstr_v_from_buf("", 0));

    if (turbo_datetime_format_rfc822(ts, buf, sizeof(buf)) < 0)
        return exprtk_val_str(tstr_v_from_buf("", 0));
    return parser_string_value(ud->env, buf);
}

/* =========================================================================
 * Module Registration
 * ========================================================================= */

void parser_load(void *p, void *e, void *s) {
    parser_ctx_t *ctx = (parser_ctx_t *)p;
    exprtk_env_t *env = (exprtk_env_t *)e;
    mem_pool_t *scratch = (mem_pool_t *)s;
    
    if (!ctx || !env || !scratch) return;
    
    /* Allocate user data */
    parser_ud_t *ud = (parser_ud_t *)mem_alloc(&env->arena, sizeof(parser_ud_t));
    if (!ud) return;
    
    ud->ctx = ctx;
    ud->env = env;
    ud->scratch = scratch;

    /* Register parser helper functions */
    exprtk_env_register_func(env, "parser.ini_get", fn_ini_get, ud);
    exprtk_env_register_func(env, "parser.dotenv_load", fn_dotenv_load, ud);
    exprtk_env_register_func(env, "parser.dotenv_load_default", fn_dotenv_load_default, ud);
    exprtk_env_register_func(env, "parser.toml_parse", fn_toml_parse, ud);
    exprtk_env_register_func(env, "parser.toml_parse_file", fn_toml_parse_file, ud);
    exprtk_env_register_func(env, "parser.cmd_parse", fn_cmd_parse, ud);
}
