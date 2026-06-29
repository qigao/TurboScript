/**
 * @file exprtk_mod_data_bind.c
 * @brief data_bind module for TurboScript.
 *
 * Exposes the following functions to scripts:
 *
 *   data_bind.create(schema_path)         -> number (codec handle)
 *   data_bind.parse(handle, type, bytes)  -> object (parsed message object)
 *   data_bind.json(handle, type, json)    -> object (schema-bound JSON object)
 *   data_bind.csv(handle, type, csv, row) -> object (schema-bound CSV row)
 *   data_bind.xml(handle, type, xml)      -> object (schema-bound XML document)
 *   data_bind.close(handle)               -> number (0)
 *
 * `bytes` is a TurboScript string carrying raw bytes.  The plugin passes
 * the string buffer directly to the JIT-compiled parser.
 *
 * The core data_bind library owns dynamic parsing and returns a DataBindValue
 * tree.  This module only adapts that tree into native exprtk containers at
 * the scripting boundary.
 */
#include "data_bind_ctx.h"

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void *db_ctx_create(void) {
    return calloc(1, sizeof(db_ctx_t));
}

void db_ctx_destroy(void *p) {
    db_ctx_t *ctx = (db_ctx_t *)p;
    if (!ctx) return;
    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        if (ctx->handles[i].codec) {
            data_bind_free(ctx->handles[i].codec);
            ctx->handles[i].codec = NULL;
        }
    }
    free(ctx);
}

/* ── Handle management ────────────────────────────────────────────────────── */

static int db_handle_alloc(db_ctx_t *ctx, DataBind *codec) {
    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        if (!ctx->handles[i].codec) {
            ctx->handles[i].codec = codec;
            return i;
        }
    }
    return -1;
}

static DataBind *db_handle_get(db_ctx_t *ctx, int h) {
    if (h < 0 || h >= DB_MAX_HANDLES) return NULL;
    return ctx->handles[h].codec;
}

static void db_handle_free(db_ctx_t *ctx, int h) {
    if (h < 0 || h >= DB_MAX_HANDLES) return;
    if (ctx->handles[h].codec) {
        data_bind_free(ctx->handles[h].codec);
        ctx->handles[h].codec = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * DataBindValue -> exprtk_value_t conversion
 * ══════════════════════════════════════════════════════════════════════════ */

static exprtk_value_t db_null_value(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static exprtk_value_t db_string_value(exprtk_env_t *env, const char *data) {
    size_t len = data ? strlen(data) : 0;
    char *copy = db_arena_cstr(&env->arena, data ? data : "", len);
    if (!copy) return DB_ZERO;
    return exprtk_val_str(tstr_v_from_buf(copy, len));
}

static exprtk_value_t db_bytes_value(exprtk_env_t *env, const DataBindValue *value) {
    size_t len = 0;
    const uint8_t *bytes = data_bind_value_as_bytes(value, &len);
    char *copy;

    if (!env || (!bytes && len > 0)) return DB_ZERO;
    copy = (char *)mem_alloc(&env->arena, len + 1);
    if (!copy) return DB_ZERO;
    if (len > 0) memcpy(copy, bytes, len);
    copy[len] = '\0';
    return exprtk_val_bytes(tstr_v_from_buf(copy, len));
}

static exprtk_value_t db_uuid_value(const DataBindValue *value) {
    uuid_t uuid;
    if (!data_bind_value_as_uuid(value, &uuid)) return DB_ZERO;
    return exprtk_val_uuid(uuid);
}

static exprtk_value_t db_datetime_value(const DataBindValue *value) {
    turbo_datetime_t dt;
    if (!data_bind_value_as_datetime(value, &dt)) return DB_ZERO;
    return exprtk_val_datetime(dt);
}

static exprtk_value_t db_value_to_exprtk(db_ud_t *ud, const DataBindValue *value) {
    size_t i;

    if (!ud || !value) return db_null_value();

    switch (data_bind_value_kind(value)) {
    case DATA_BIND_VALUE_OBJECT: {
        exprtk_value_t map = exprtk_val_object();
        size_t count = data_bind_value_field_count(value);
        for (i = 0; i < count; i++) {
            const char *name = data_bind_value_field_name(value, i);
            const DataBindValue *child = data_bind_value_field_at(value, i);
            if (name) exprtk_map_set(&map, name, db_value_to_exprtk(ud, child));
        }
        return map;
    }
    case DATA_BIND_VALUE_LIST:
    case DATA_BIND_VALUE_SET: {
        exprtk_value_t list = exprtk_val_list_empty();
        size_t count = data_bind_value_count(value);
        for (i = 0; i < count; i++)
            exprtk_list_push(&list, db_value_to_exprtk(ud, data_bind_value_at(value, i)));
        return list;
    }
    case DATA_BIND_VALUE_MAP: {
        exprtk_value_t map = exprtk_val_map();
        size_t count = data_bind_value_count(value);
        for (i = 0; i < count; i++) {
            DataBindMapEntry entry = data_bind_value_map_entry_at(value, i);
            if (entry.key) exprtk_map_set(&map, entry.key, db_value_to_exprtk(ud, entry.value));
        }
        return map;
    }
    case DATA_BIND_VALUE_INT:
        return exprtk_val_num((double)data_bind_value_as_int(value));
    case DATA_BIND_VALUE_INT64:
        return exprtk_val_int(data_bind_value_as_int64(value));
    case DATA_BIND_VALUE_DOUBLE:
        return exprtk_val_num(data_bind_value_as_double(value));
    case DATA_BIND_VALUE_BOOL:
        return exprtk_val_bool(data_bind_value_as_bool(value));
    case DATA_BIND_VALUE_STRING:
        return db_string_value(ud->env, data_bind_value_as_string(value));
    case DATA_BIND_VALUE_BYTES:
        return db_bytes_value(ud->env, value);
    case DATA_BIND_VALUE_UUID:
        return db_uuid_value(value);
    case DATA_BIND_VALUE_DATETIME:
        return db_datetime_value(value);
    case DATA_BIND_VALUE_NULL:
    default:
        return db_null_value();
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Script-visible functions
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * data_bind.create(schema_path) -> number (handle >= 0, or -1 on error)
 */
static exprtk_value_t fn_db_create(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.create: expected string schema_path");
        return DB_ZERO;
    }

    /* NUL-terminate the schema path in scratch memory */
    char *path = db_arena_cstr(ud->scratch, args[0].data.string.data,
                               args[0].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.create: OOM"); return DB_ZERO; }

    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    if (data_bind_create(path, &codec, &err) != DATA_BIND_OK) {
        DB_ERROR(ud, err.message[0] ? err.message : "data_bind.create: failed to create codec");
        return exprtk_val_num(-1.0);
    }

    int h = db_handle_alloc(ud->ctx, codec);
    if (h < 0) {
        data_bind_free(codec);
        DB_ERROR(ud, "data_bind.create: too many open codecs");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num((double)h);
}

/**
 * data_bind.parse(handle, type_name, bytes) -> object | 0
 *
 * bytes is a TurboScript string whose data is the raw binary payload.
 */
static exprtk_value_t fn_db_parse(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    if (argc != 3
     || args[0].type != EXPRTK_VAL_NUMBER
     || args[1].type != EXPRTK_VAL_STRING
     || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.parse: expected (number, string, string)");
        return DB_ZERO;
    }

    int h = (int)args[0].data.number;
    DataBind *codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.parse: invalid handle");
        return DB_ZERO;
    }

    /* NUL-terminate type name */
    char *type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                                    args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.parse: OOM"); return DB_ZERO; }

    const uint8_t *buf = (const uint8_t *)args[2].data.string.data;
    size_t buf_len = args[2].data.string.len;

    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status = data_bind_parse(codec, type_name, buf, buf_len, &result, &err);
    exprtk_value_t out;

    if (status != DATA_BIND_OK) {
        if (err.message[0]) DB_ERROR(ud, err.message);
        return DB_ZERO;
    }

    out = db_value_to_exprtk(ud, result);
    data_bind_value_free(result);
    return out;
}

static exprtk_value_t db_convert_result(db_ud_t *ud, DataBindValue *result) {
    exprtk_value_t out;
    if (!result) return DB_ZERO;
    out = db_value_to_exprtk(ud, result);
    data_bind_value_free(result);
    return out;
}

static exprtk_value_t db_convert_status_result(db_ud_t *ud, DataBindStatus status,
                                               DataBindValue *result,
                                               const DataBindError *err) {
    if (status != DATA_BIND_OK) {
        if (err && err->message[0]) DB_ERROR(ud, err->message);
        return DB_ZERO;
    }
    return db_convert_result(ud, result);
}

static exprtk_value_t fn_db_json(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name;
    DataBind *codec;
    int h;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.json: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.json: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.json: OOM"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_json(
            codec, type_name, args[2].data.string.data, args[2].data.string.len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_json_all(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name;
    DataBind *codec;
    int h;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.json_all: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.json_all: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.json_all: OOM"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_json_all(
            codec, type_name, args[2].data.string.data, args[2].data.string.len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_csv(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name;
    DataBind *codec;
    int h;
    int row;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_NUMBER) {
        DB_ERROR(ud, "data_bind.csv: expected (number, string, string, number)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    row = (int)args[3].data.number;
    if (row < 0) return DB_ZERO;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.csv: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.csv: OOM"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_csv(codec, type_name, args[2].data.string.data,
                                                    args[2].data.string.len, (size_t)row,
                                                    &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_csv_all(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name;
    DataBind *codec;
    int h;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.csv_all: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.csv_all: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.csv_all: OOM"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_csv_all(
            codec, type_name, args[2].data.string.data, args[2].data.string.len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_xml(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name;
    DataBind *codec;
    int h;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.xml: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.xml: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.xml: OOM"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_xml(
            codec, type_name, args[2].data.string.data, args[2].data.string.len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_xml_all(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name;
    char *xpath;
    DataBind *codec;
    int h;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.xml_all: expected (number, string, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.xml_all: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    xpath = db_arena_cstr(ud->scratch, args[3].data.string.data, args[3].data.string.len);
    if (!type_name || !xpath) { DB_ERROR(ud, "data_bind.xml_all: OOM"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_xml_all(codec, type_name, args[2].data.string.data,
                                                        args[2].data.string.len, xpath,
                                                        &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

/**
 * data_bind.close(handle) -> 0
 */
static exprtk_value_t fn_db_close(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
        DB_ERROR(ud, "data_bind.close: expected number handle");
        return DB_ZERO;
    }
    db_handle_free(ud->ctx, (int)args[0].data.number);
    return DB_ZERO;
}

/* ── Loader ──────────────────────────────────────────────────────────────── */

void db_plugin_load(void *p, void *e, void *s) {
    db_ctx_t     *ctx     = (db_ctx_t *)p;
    exprtk_env_t *env     = (exprtk_env_t *)e;
    mem_pool_t   *scratch = (mem_pool_t *)s;
    if (!ctx || !env) return;

    db_ud_t *ud = (db_ud_t *)mem_alloc(&env->arena, sizeof(*ud));
    if (!ud) return;
    ud->ctx     = ctx;
    ud->env     = env;
    ud->scratch = scratch;

    exprtk_env_register_func(env, "data_bind.create", fn_db_create, ud);
    exprtk_env_register_func(env, "data_bind.parse",  fn_db_parse,  ud);
    exprtk_env_register_func(env, "data_bind.json",   fn_db_json,   ud);
    exprtk_env_register_func(env, "data_bind.json_all", fn_db_json_all, ud);
    exprtk_env_register_func(env, "data_bind.csv",    fn_db_csv,    ud);
    exprtk_env_register_func(env, "data_bind.csv_all", fn_db_csv_all, ud);
    exprtk_env_register_func(env, "data_bind.xml",    fn_db_xml,    ud);
    exprtk_env_register_func(env, "data_bind.xml_all", fn_db_xml_all, ud);
    exprtk_env_register_func(env, "data_bind.close",  fn_db_close,  ud);
}
