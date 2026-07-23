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
 *   data_bind.json_path(handle, type, path)   -> object (schema-bound JSON object from file)
 *   data_bind.json_all_path(handle, type, path)-> object (all JSON objects from file)
 *   data_bind.csv_path(handle, type, path, row)-> object (schema-bound CSV row from file)
 *   data_bind.csv_all_path(handle, type, path)-> object (all CSV objects from file)
 *   data_bind.xml_path(handle, type, path)    -> object (schema-bound XML document from file)
 *   data_bind.xml_all_path(handle, type, path, xpath) -> object (all XML objects from file)
 *   data_bind.yaml(handle, type, yaml) -> object (schema-bound YAML root)
 *   data_bind.yaml_all(handle, type, yaml) -> object (schema-bound YAML root sequence)
 *   data_bind.yaml_ypath(handle, type, yaml, ypath) -> object (first YPATH match)
 *   data_bind.yaml_ypath_all(handle, type, yaml, ypath) -> object (all YPATH matches)
 *   data_bind.object_from_<format>(handle, type, input[, row]) -> object handle
 *   data_bind.object_clone(object_handle) -> object handle
 *   data_bind.object_value(object_handle) -> object
 *   data_bind.object_serialize_<text-format>(object_handle) -> string
 *   data_bind.object_serialize_binary(object_handle) -> bytes
 *   data_bind.object_close(object_handle) -> number (0)
 *   data_bind.sax.yaml*_create(...) -> number (buffered YAML stream handle)
 *   data_bind.sax.set_callback(stream, fn) -> bool
 *   data_bind.json_stream(handle, type, json) -> object (stream-bound JSON object)
 *   data_bind.json_all_stream(handle, type, json) -> object (stream-bound JSON array)
 *   data_bind.json_path_stream(handle, type, json, jsonpath) -> object
 *   data_bind.json_path_all_stream(handle, type, json, jsonpath) -> object
 *   data_bind.csv_all_stream(handle, type, csv) -> object (stream-bound CSV rows)
 *   data_bind.csv_path_stream(handle, type, csv, csvpath) -> object
 *   data_bind.xml_stream(handle, type, xml) -> object (stream-bound XML document)
 *   data_bind.xml_path_all_stream(handle, type, xml, xpath) -> object
 *   data_bind.json_stream_path(handle, type, path) -> object (stream-bound JSON file)
 *   data_bind.json_all_stream_path(handle, type, path) -> object
 *   data_bind.json_path_stream_path(handle, type, path, jsonpath) -> object
 *   data_bind.json_path_all_stream_path(handle, type, path, jsonpath) -> object
 *   data_bind.csv_all_stream_path(handle, type, path) -> object
 *   data_bind.csv_path_stream_path(handle, type, path, csvpath) -> object
 *   data_bind.xml_stream_path(handle, type, path) -> object
 *   data_bind.xml_path_all_stream_path(handle, type, path, xpath) -> object
 *   data_bind.sax.json_create(handle, type) -> number (stream handle)
 *   data_bind.sax.json_all_create(handle, type) -> number (stream handle)
 *   data_bind.sax.json_path_create(handle, type, jsonpath) -> number (stream handle)
 *   data_bind.sax.json_path_all_create(handle, type, jsonpath) -> number (stream handle)
 *   data_bind.sax.csv_all_create(handle, type) -> number (stream handle)
 *   data_bind.sax.csv_path_create(handle, type, csvpath) -> number (stream handle)
 *   data_bind.sax.xml_create(handle, type) -> number (stream handle)
 *   data_bind.sax.xml_path_all_create(handle, type, xpath) -> number (stream handle)
 *   data_bind.sax.feed(stream_handle, chunk) -> number (0)
 *   data_bind.sax.feed_path(stream_handle, path) -> number (0)
 *   data_bind.sax.finish(stream_handle) -> object
 *   data_bind.sax.close(stream_handle) -> number (0)
 *   data_bind.dom.* aliases use the DOM parse APIs.
 *   data_bind.sax.* aliases use the SAX/streaming APIs.
 *   data_bind.close(handle)               -> number (0)
 *
 * `bytes` is a TurboScript string carrying raw bytes.  The plugin passes
 * the string buffer directly to the JIT-compiled parser.
 *
 * The core data_bind library owns both parse models. DOM APIs parse and
 * materialize a complete DataBindValue tree. SAX APIs accept chunks through a
 * stateful stream parser and materialize or collect bound values at finish.
 * This module only adapts DataBindValue trees into native exprtk containers at
 * the scripting boundary.
 *
 * Stream callbacks receive (record, index). Returning false/0 continues,
 * true/a positive number stops later callbacks, and a negative number fails
 * the stream. The final bound value remains available from sax.finish().
 */
#include "data_bind_ctx.h"

#define DB_REQUIRED_DATA_BIND_VERSION 11000

#if DATA_BIND_VERSION < DB_REQUIRED_DATA_BIND_VERSION
  #error "TurboScript data_bind requires DataBind 1.10.0 or newer"
#endif

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

void *db_ctx_create(void) {
    db_ctx_t *ctx = (db_ctx_t *)calloc(1, sizeof(db_ctx_t));
    if (!ctx) return NULL;

    if (db_handle_map_t_init(&ctx->handles) != 0) {
        free(ctx);
        return NULL;
    }
    if (db_stream_map_t_init(&ctx->streams) != 0) {
        db_handle_map_t_destroy(&ctx->handles);
        free(ctx);
        return NULL;
    }
    if (db_object_map_t_init(&ctx->objects) != 0) {
        db_stream_map_t_destroy(&ctx->streams);
        db_handle_map_t_destroy(&ctx->handles);
        free(ctx);
        return NULL;
    }
    if (turbo_hash_map_reserve(&ctx->handles.raw, DB_MAX_HANDLES) != 0 ||
        turbo_hash_map_reserve(&ctx->streams.raw, DB_MAX_HANDLES) != 0 ||
        turbo_hash_map_reserve(&ctx->objects.raw, DB_MAX_HANDLES) != 0) {
        db_object_map_t_destroy(&ctx->objects);
        db_stream_map_t_destroy(&ctx->streams);
        db_handle_map_t_destroy(&ctx->handles);
        free(ctx);
        return NULL;
    }
    ctx->next_handle = 0;
    ctx->next_stream_handle = 0;
    ctx->next_object_handle = 0;
    return ctx;
}

void db_ctx_destroy(void *p) {
    db_ctx_t *ctx = (db_ctx_t *)p;
    if (!ctx) return;
    for (int i = 0; i < DB_MAX_HANDLES; ++i) {
        db_stream_entry_t **entry_ptr = db_stream_map_t_get(&ctx->streams, i);
        if (entry_ptr && *entry_ptr) {
            if ((*entry_ptr)->stream) data_bind_stream_destroy((*entry_ptr)->stream);
            if ((*entry_ptr)->result) data_bind_value_free((*entry_ptr)->result);
            free(*entry_ptr);
        }
    }
    for (int i = 0; i < DB_MAX_HANDLES; ++i) {
        db_object_entry_t *entry = db_object_map_t_get(&ctx->objects, i);
        if (entry && entry->object) data_bind_object_free(entry->object);
    }
    for (int i = 0; i < DB_MAX_HANDLES; ++i) {
        DataBind **codec_ptr = db_handle_map_t_get(&ctx->handles, i);
        if (codec_ptr && *codec_ptr) {
            data_bind_free(*codec_ptr);
        }
    }
    db_object_map_t_destroy(&ctx->objects);
    db_stream_map_t_destroy(&ctx->streams);
    db_handle_map_t_destroy(&ctx->handles);
    free(ctx);
}

/* ── Handle management ────────────────────────────────────────────────────── */

static int db_handle_alloc(db_ctx_t *ctx, DataBind *codec) {
    if (!ctx || !codec) return -1;

    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        int handle = (ctx->next_handle + i) % DB_MAX_HANDLES;
        if (db_handle_map_t_contains(&ctx->handles, handle)) continue;
        if (db_handle_map_t_put(&ctx->handles, handle, codec) != TURBO_OK) return -1;
        ctx->next_handle = (handle + 1) % DB_MAX_HANDLES;
        return handle;
    }
    return -1; /* no free slot */
}

static DataBind *db_handle_get(db_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return NULL;
    DataBind **codec = db_handle_map_t_get(&ctx->handles, h);
    return codec ? *codec : NULL;
}

static void db_stream_close_for_codec(db_ctx_t *ctx, DataBind *codec);

static int db_object_handle_alloc(db_ctx_t *ctx, DataBind *codec, DataBindObject *object) {
    db_object_entry_t entry;
    if (!ctx || !codec || !object) return -1;
    entry.codec = codec;
    entry.object = object;
    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        int handle = (ctx->next_object_handle + i) % DB_MAX_HANDLES;
        if (db_object_map_t_contains(&ctx->objects, handle)) continue;
        if (db_object_map_t_put(&ctx->objects, handle, entry) != TURBO_OK) return -1;
        ctx->next_object_handle = (handle + 1) % DB_MAX_HANDLES;
        return handle;
    }
    return -1;
}

static db_object_entry_t *db_object_handle_get(db_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return NULL;
    return db_object_map_t_get(&ctx->objects, h);
}

static int db_object_handle_free(db_ctx_t *ctx, int h) {
    db_object_entry_t entry = {0};
    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return 0;
    if (!db_object_map_t_remove(&ctx->objects, h, &entry) || !entry.object) return 0;
    data_bind_object_free(entry.object);
    return 1;
}

static void db_object_close_for_codec(db_ctx_t *ctx, DataBind *codec) {
    if (!ctx || !codec) return;
    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        db_object_entry_t *entry = db_object_map_t_get(&ctx->objects, i);
        if (entry && entry->codec == codec) db_object_handle_free(ctx, i);
    }
}

static void db_handle_free(db_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return;

    DataBind *codec = NULL;
    if (db_handle_map_t_remove(&ctx->handles, h, &codec)) {
        db_stream_close_for_codec(ctx, codec);
        db_object_close_for_codec(ctx, codec);
        data_bind_free(codec);
    }
}

static int db_stream_handle_alloc(db_ctx_t *ctx, db_stream_entry_t *entry) {
    if (!ctx || !entry || !entry->codec || !entry->stream) return -1;

    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        int handle = (ctx->next_stream_handle + i) % DB_MAX_HANDLES;
        if (db_stream_map_t_contains(&ctx->streams, handle)) continue;
        if (db_stream_map_t_put(&ctx->streams, handle, entry) != TURBO_OK) return -1;
        ctx->next_stream_handle = (handle + 1) % DB_MAX_HANDLES;
        return handle;
    }
    return -1;
}

static db_stream_entry_t *db_stream_handle_get(db_ctx_t *ctx, int h) {
    db_stream_entry_t **entry;

    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return NULL;
    entry = db_stream_map_t_get(&ctx->streams, h);
    return entry ? *entry : NULL;
}

static db_stream_entry_t *db_stream_handle_take(db_ctx_t *ctx, int h) {
    db_stream_entry_t *entry = NULL;

    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return NULL;
    if (db_stream_map_t_remove(&ctx->streams, h, &entry) && entry) {
        return entry;
    }
    return NULL;
}

static void db_stream_handle_free(db_ctx_t *ctx, int h) {
    db_stream_entry_t *entry = db_stream_handle_take(ctx, h);
    if (!entry) return;
    data_bind_stream_destroy(entry->stream);
    if (entry->result) data_bind_value_free(entry->result);
    free(entry);
}

static void db_stream_close_for_codec(db_ctx_t *ctx, DataBind *codec) {
    if (!ctx || !codec) return;

    for (int i = 0; i < DB_MAX_HANDLES; i++) {
        db_stream_entry_t **entry_ptr = db_stream_map_t_get(&ctx->streams, i);
        if (entry_ptr && *entry_ptr && (*entry_ptr)->codec == codec) {
            db_stream_handle_free(ctx, i);
        }
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

static exprtk_value_t db_string_value_n(exprtk_env_t *env, const char *data, size_t len) {
    exprtk_value_t value = DB_ZERO;
    if (!env || (!data && len != 0) ||
        exprtk_value_copy_to_env(
            exprtk_val_str(tstr_v_from_buf((char *)(data ? data : ""), len)), env,
            &value) != 0)
        return DB_ZERO;
    return value;
}

static exprtk_value_t db_string_value(exprtk_env_t *env, const char *data) {
    return db_string_value_n(env, data, data ? strlen(data) : 0);
}

static exprtk_value_t db_bytes_value(exprtk_env_t *env, const DataBindValue *value) {
    size_t len = 0;
    const uint8_t *bytes = data_bind_value_as_bytes(value, &len);
    exprtk_value_t result = DB_ZERO;

    if (!env || (!bytes && len > 0)) return DB_ZERO;
    if (exprtk_value_copy_to_env(
            exprtk_val_bytes(tstr_v_from_buf((char *)bytes, len)), env, &result) != 0)
        return DB_ZERO;
    return result;
}

static exprtk_value_t db_uuid_value(const DataBindValue *value) {
    turbo_uuid_t uuid;
    if (!data_bind_value_as_uuid(value, uuid.bytes)) return DB_ZERO;
    return exprtk_val_uuid(uuid);
}

static exprtk_value_t db_datetime_value(const DataBindValue *value) {
    turbo_datetime_t dt;
    if (!data_bind_value_as_datetime(value, &dt)) return DB_ZERO;
    return exprtk_val_datetime(dt);
}

static exprtk_value_t db_date_value(const DataBindValue *value) {
    DataBindDate source;
    exprtk_date_t date;
    if (!data_bind_value_as_date(value, &source)) return DB_ZERO;
    date.year = source.year;
    date.month = source.month;
    date.day = source.day;
    return exprtk_val_date(date);
}

static exprtk_value_t db_time_value(const DataBindValue *value) {
    DataBindTime source;
    exprtk_time_t time;
    if (!data_bind_value_as_time(value, &source)) return DB_ZERO;
    time.hour = source.hour;
    time.minute = source.minute;
    time.second = source.second;
    time.millisecond = source.millisecond;
    return exprtk_val_time(time);
}

static exprtk_value_t db_decimal_value(const DataBindValue *value) {
    DataBindDecimal source;
    exprtk_decimal_t decimal;
    if (!data_bind_value_as_decimal(value, &source)) return DB_ZERO;
    decimal.mantissa = source.mantissa;
    decimal.scale = source.scale;
    return exprtk_val_decimal(decimal);
}

static exprtk_value_t db_bigint_value(exprtk_env_t *env, const DataBindValue *value) {
    const char *text = NULL;
    size_t len = 0;
    exprtk_value_t result = DB_ZERO;
    if (!env || data_bind_value_get_bigint(value, &text, &len) != DATA_BIND_OK ||
        (!text && len > 0))
        return DB_ZERO;
    if (exprtk_value_copy_to_env(
            exprtk_val_bigint(tstr_v_from_buf((char *)(text ? text : ""), len)), env,
            &result) != 0)
        return DB_ZERO;
    return result;
}

static exprtk_value_t db_money_value(const DataBindValue *value) {
    DataBindMoney source;
    exprtk_money_t money;
    if (!data_bind_value_as_money(value, &source)) return DB_ZERO;
    memset(&money, 0, sizeof(money));
    money.amount.mantissa = source.amount.mantissa;
    money.amount.scale = source.amount.scale;
    memcpy(money.currency, source.currency, sizeof(money.currency));
    return exprtk_val_money(money);
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
            if (name) {
                exprtk_value_t converted = db_value_to_exprtk(ud, child);
                if (exprtk_map_set(&map, name, converted) != 0) {
                    exprtk_value_destroy(&converted);
                    exprtk_value_destroy(&map);
                    return db_null_value();
                }
                exprtk_value_destroy(&converted);
            }
        }
        return map;
    }
    case DATA_BIND_VALUE_LIST:
    case DATA_BIND_VALUE_SET: {
        exprtk_value_t list = exprtk_val_list_empty();
        size_t count = data_bind_value_count(value);
        for (i = 0; i < count; i++) {
            exprtk_value_t converted = db_value_to_exprtk(ud, data_bind_value_at(value, i));
            if (exprtk_list_push(&list, converted) != 0) {
                exprtk_value_destroy(&converted);
                exprtk_value_destroy(&list);
                return db_null_value();
            }
            exprtk_value_destroy(&converted);
        }
        return list;
    }
    case DATA_BIND_VALUE_MAP: {
        exprtk_value_t map = exprtk_val_map();
        size_t count = data_bind_value_count(value);
        for (i = 0; i < count; i++) {
            DataBindMapEntry entry = data_bind_value_map_entry_at(value, i);
            if (entry.key) {
                exprtk_value_t converted = db_value_to_exprtk(ud, entry.value);
                if (exprtk_map_set(&map, entry.key, converted) != 0) {
                    exprtk_value_destroy(&converted);
                    exprtk_value_destroy(&map);
                    return db_null_value();
                }
                exprtk_value_destroy(&converted);
            }
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
    case DATA_BIND_VALUE_DATE:
        return db_date_value(value);
    case DATA_BIND_VALUE_TIME:
        return db_time_value(value);
    case DATA_BIND_VALUE_DURATION:
        return exprtk_val_duration(data_bind_value_as_duration_milliseconds(value));
    case DATA_BIND_VALUE_DECIMAL:
        return db_decimal_value(value);
    case DATA_BIND_VALUE_BIGINT:
        return db_bigint_value(ud->env, value);
    case DATA_BIND_VALUE_MONEY:
        return db_money_value(value);
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
static exprtk_value_t fn_db_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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
 * data_bind.create_from_text(schema_text) -> number (handle >= 0, or -1 on error)
 */
static exprtk_value_t fn_db_create_from_text(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    int handle;

    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.create_from_text: expected schema string");
        return exprtk_val_num(-1.0);
    }

    status = data_bind_create_from_text(args[0].data.string.data,
                                        args[0].data.string.len, &codec, &err);
    if (status != DATA_BIND_OK) {
        DB_ERROR(ud, err.message[0] ? err.message : "data_bind.create_from_text: failed");
        return exprtk_val_num(-1.0);
    }

    handle = db_handle_alloc(ud->ctx, codec);
    if (handle < 0) {
        data_bind_free(codec);
        DB_ERROR(ud, "data_bind.create_from_text: too many open codecs");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num((double)handle);
}

typedef DataBindStatus (*db_validate_fn)(DataBind *, const char *,
                                         const char *, size_t, DataBindError *);
typedef DataBindStatus (*db_validate_path_fn)(DataBind *, const char *,
                                              const char *, size_t, const char *,
                                              DataBindError *);

static exprtk_value_t db_validate_text(size_t argc, exprtk_value_t *args, void *ud_,
                                       const char *fn_name, db_validate_fn validate) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindError err = DATA_BIND_ERROR_INIT;
    char *type_name;
    int handle;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return exprtk_val_bool(0);
    }

    handle = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, handle);
    if (!codec) {
        DB_ERROR(ud, "data_bind validate: invalid handle");
        return exprtk_val_bool(0);
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                              args[1].data.string.len);
    if (!type_name) {
        DB_ERROR(ud, "data_bind validate: OOM");
        return exprtk_val_bool(0);
    }

    return exprtk_val_bool(validate(codec, type_name, args[2].data.string.data,
                                    args[2].data.string.len, &err) == DATA_BIND_OK);
}

static exprtk_value_t fn_db_validate_json(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_validate_text(argc, args, ud_,
                            "data_bind.validate_json: expected (number, string, string)",
                            data_bind_validate_json);
}

static exprtk_value_t fn_db_validate_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_validate_text(argc, args, ud_,
                            "data_bind.validate_csv: expected (number, string, string)",
                            data_bind_validate_csv);
}

static exprtk_value_t fn_db_validate_yaml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_validate_text(argc, args, ud_,
                            "data_bind.validate_yaml: expected (number, string, string)",
                            data_bind_validate_yaml);
}

static exprtk_value_t db_validate_text_path(size_t argc, exprtk_value_t *args, void *ud_,
                                            const char *fn_name,
                                            db_validate_path_fn validate) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindError err = DATA_BIND_ERROR_INIT;
    char *type_name;
    char *path;
    int handle;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return exprtk_val_bool(0);
    }

    handle = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, handle);
    if (!codec) {
        DB_ERROR(ud, "data_bind validate path: invalid handle");
        return exprtk_val_bool(0);
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                              args[1].data.string.len);
    path = db_arena_cstr(ud->scratch, args[3].data.string.data,
                         args[3].data.string.len);
    if (!type_name || !path) {
        DB_ERROR(ud, "data_bind validate path: OOM");
        return exprtk_val_bool(0);
    }

    return exprtk_val_bool(validate(codec, type_name, args[2].data.string.data,
                                    args[2].data.string.len, path, &err) == DATA_BIND_OK);
}

static exprtk_value_t fn_db_validate_yaml_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_validate_text_path(argc, args, ud_,
        "data_bind.validate_yaml_path: expected (number, string, string, string)",
        data_bind_validate_yaml_path);
}

static exprtk_value_t fn_db_validate_xml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindError err = DATA_BIND_ERROR_INIT;
    char *type_name;
    int handle;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.validate_xml: expected (number, string, string)");
        return exprtk_val_bool(0);
    }

    handle = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, handle);
    if (!codec) {
        DB_ERROR(ud, "data_bind.validate_xml: invalid handle");
        return exprtk_val_bool(0);
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                              args[1].data.string.len);
    if (!type_name) {
        DB_ERROR(ud, "data_bind.validate_xml: OOM");
        return exprtk_val_bool(0);
    }

    return exprtk_val_bool(data_bind_validate_xml_path(
        codec, type_name, args[2].data.string.data, args[2].data.string.len,
        NULL, &err) == DATA_BIND_OK);
}

/**
 * data_bind.parse(handle, type_name, bytes) -> object | 0
 *
 * bytes is a TurboScript string whose data is the raw binary payload.
 */
static exprtk_value_t fn_db_parse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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
        if (result) data_bind_value_free(result);
        if (err && err->message[0]) DB_ERROR(ud, err->message);
        return DB_ZERO;
    }
    return db_convert_result(ud, result);
}

typedef DataBindStatus (*db_object_text_fn)(DataBind *, const char *, const char *, size_t,
                                            DataBindObject **, DataBindError *);
typedef DataBindStatus (*db_object_serialize_fn)(const DataBindObject *, char **, size_t *,
                                                 DataBindError *);

static exprtk_value_t db_object_publish(db_ud_t *ud, DataBind *codec, DataBindObject *object,
                                        const DataBindError *error) {
    int handle;
    if (!object) {
        if (error && error->message[0]) DB_ERROR(ud, error->message);
        return exprtk_val_num(-1.0);
    }
    handle = db_object_handle_alloc(ud->ctx, codec, object);
    if (handle < 0) {
        data_bind_object_free(object);
        DB_ERROR(ud, "data_bind.object: too many open objects");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num((double)handle);
}

static exprtk_value_t db_object_from_text(size_t argc, exprtk_value_t *args, void *ud_,
                                          const char *error_text,
                                          db_object_text_fn parse) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindObject *object = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, error_text);
        return exprtk_val_num(-1.0);
    }
    codec = db_handle_get(ud->ctx, (int)args[0].data.number);
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                              args[1].data.string.len);
    if (!codec || !type_name) {
        DB_ERROR(ud, !codec ? "data_bind.object: invalid codec handle" :
                             "data_bind.object: OOM");
        return exprtk_val_num(-1.0);
    }
    status = parse(codec, type_name, args[2].data.string.data,
                   args[2].data.string.len, &object, &error);
    if (status != DATA_BIND_OK) {
        if (object) data_bind_object_free(object);
        DB_ERROR(ud, error.message[0] ? error.message : error_text);
        return exprtk_val_num(-1.0);
    }
    return db_object_publish(ud, codec, object, &error);
}

static exprtk_value_t fn_db_object_from_json(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_from_text(argc, args, ud_,
        "data_bind.object_from_json: expected (codec, type, json)",
        data_bind_object_from_json);
}

static exprtk_value_t fn_db_object_from_yaml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_from_text(argc, args, ud_,
        "data_bind.object_from_yaml: expected (codec, type, yaml)",
        data_bind_object_from_yaml);
}

static exprtk_value_t fn_db_object_from_xml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_from_text(argc, args, ud_,
        "data_bind.object_from_xml: expected (codec, type, xml)",
        data_bind_object_from_xml);
}

static exprtk_value_t fn_db_object_from_binary(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindObject *object = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;
    const char *data;
    size_t len;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING ||
        (args[2].type != EXPRTK_VAL_STRING && args[2].type != EXPRTK_VAL_BYTES)) {
        DB_ERROR(ud, "data_bind.object_from_binary: expected (codec, type, bytes)");
        return exprtk_val_num(-1.0);
    }
    codec = db_handle_get(ud->ctx, (int)args[0].data.number);
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                              args[1].data.string.len);
    if (args[2].type == EXPRTK_VAL_BYTES) {
        data = args[2].data.bytes.data;
        len = args[2].data.bytes.len;
    } else {
        data = args[2].data.string.data;
        len = args[2].data.string.len;
    }
    if (!codec || !type_name) {
        DB_ERROR(ud, !codec ? "data_bind.object_from_binary: invalid codec handle" :
                             "data_bind.object_from_binary: OOM");
        return exprtk_val_num(-1.0);
    }
    status = data_bind_object_from_bin(codec, type_name, (const uint8_t *)data, len,
                                       &object, &error);
    if (status != DATA_BIND_OK) {
        if (object) data_bind_object_free(object);
        DB_ERROR(ud, error.message[0] ? error.message :
                     "data_bind.object_from_binary: failed");
        return exprtk_val_num(-1.0);
    }
    return db_object_publish(ud, codec, object, &error);
}

static exprtk_value_t fn_db_object_from_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindObject *object = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;
    int64_t row;

    if (!ud || argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        (args[3].type != EXPRTK_VAL_INTEGER && args[3].type != EXPRTK_VAL_NUMBER)) {
        DB_ERROR(ud, "data_bind.object_from_csv: expected (codec, type, csv, row)");
        return exprtk_val_num(-1.0);
    }
    row = args[3].type == EXPRTK_VAL_INTEGER ? args[3].data.integer :
                                               (int64_t)args[3].data.number;
    if (row < 0) {
        DB_ERROR(ud, "data_bind.object_from_csv: row must be non-negative");
        return exprtk_val_num(-1.0);
    }
    codec = db_handle_get(ud->ctx, (int)args[0].data.number);
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data,
                              args[1].data.string.len);
    if (!codec || !type_name) {
        DB_ERROR(ud, !codec ? "data_bind.object_from_csv: invalid codec handle" :
                             "data_bind.object_from_csv: OOM");
        return exprtk_val_num(-1.0);
    }
    status = data_bind_object_from_csv(codec, type_name, args[2].data.string.data,
                                       args[2].data.string.len, (size_t)row, &object, &error);
    if (status != DATA_BIND_OK) {
        if (object) data_bind_object_free(object);
        DB_ERROR(ud, error.message[0] ? error.message : "data_bind.object_from_csv: failed");
        return exprtk_val_num(-1.0);
    }
    return db_object_publish(ud, codec, object, &error);
}

static exprtk_value_t fn_db_object_clone(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_object_entry_t *source;
    DataBindObject *clone = NULL;
    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_NUMBER ||
        !(source = db_object_handle_get(ud->ctx, (int)args[0].data.number))) {
        DB_ERROR(ud, "data_bind.object_clone: expected valid object handle");
        return exprtk_val_num(-1.0);
    }
    if (data_bind_object_clone(source->object, &clone) != DATA_BIND_OK || !clone) {
        DB_ERROR(ud, "data_bind.object_clone: failed");
        return exprtk_val_num(-1.0);
    }
    return db_object_publish(ud, source->codec, clone, NULL);
}

static exprtk_value_t fn_db_object_type(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_object_entry_t *entry;
    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_NUMBER ||
        !(entry = db_object_handle_get(ud->ctx, (int)args[0].data.number))) {
        DB_ERROR(ud, "data_bind.object_type: expected valid object handle");
        return db_string_value(ud->env, "");
    }
    return db_string_value(ud->env, data_bind_object_type_name(entry->object));
}

static exprtk_value_t fn_db_object_value(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_object_entry_t *entry;
    const DataBindValue *value;
    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_NUMBER ||
        !(entry = db_object_handle_get(ud->ctx, (int)args[0].data.number)) ||
        !(value = data_bind_object_value(entry->object))) {
        DB_ERROR(ud, "data_bind.object_value: expected valid object handle");
        return DB_ZERO;
    }
    return db_value_to_exprtk(ud, value);
}

static exprtk_value_t db_object_serialize(size_t argc, exprtk_value_t *args, void *ud_,
                                          const char *error_text,
                                          db_object_serialize_fn serialize) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_object_entry_t *entry;
    DataBindError error = DATA_BIND_ERROR_INIT;
    char *serialized = NULL;
    size_t len = 0;
    DataBindStatus status;

    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_NUMBER ||
        !(entry = db_object_handle_get(ud->ctx, (int)args[0].data.number))) {
        DB_ERROR(ud, error_text);
        return db_string_value(ud ? ud->env : NULL, "");
    }
    status = serialize(entry->object, &serialized, &len, &error);
    if (status != DATA_BIND_OK || !serialized) {
        if (serialized) data_bind_serialized_free(serialized);
        DB_ERROR(ud, error.message[0] ? error.message : error_text);
        return db_string_value(ud->env, "");
    }
    exprtk_value_t result = db_string_value_n(ud->env, serialized, len);
    data_bind_serialized_free(serialized);
    if (result.type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.object_serialize: OOM");
        return db_string_value(ud->env, "");
    }
    return result;
}

static exprtk_value_t fn_db_object_serialize_json(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_serialize(argc, args, ud_,
        "data_bind.object_serialize_json: expected valid object handle",
        data_bind_object_serialize_json);
}

static exprtk_value_t fn_db_object_serialize_yaml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_serialize(argc, args, ud_,
        "data_bind.object_serialize_yaml: expected valid object handle",
        data_bind_object_serialize_yaml);
}

static exprtk_value_t fn_db_object_serialize_xml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_serialize(argc, args, ud_,
        "data_bind.object_serialize_xml: expected valid object handle",
        data_bind_object_serialize_xml);
}

static exprtk_value_t fn_db_object_serialize_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_object_serialize(argc, args, ud_,
        "data_bind.object_serialize_csv: expected valid object handle",
        data_bind_object_serialize_csv);
}

static exprtk_value_t fn_db_object_serialize_binary(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_object_entry_t *entry;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *serialized = NULL;
    size_t len = 0;
    DataBindStatus status;

    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_NUMBER ||
        !(entry = db_object_handle_get(ud->ctx, (int)args[0].data.number))) {
        DB_ERROR(ud, "data_bind.object_serialize_binary: expected valid object handle");
        return exprtk_val_bytes(tstr_v_from_buf("", 0));
    }
    status = data_bind_object_serialize_bin(entry->codec, entry->object, &serialized, &len,
                                             &error);
    if (status != DATA_BIND_OK || (!serialized && len != 0)) {
        if (serialized) data_bind_binary_free(serialized);
        DB_ERROR(ud, error.message[0] ? error.message :
                     "data_bind.object_serialize_binary: failed");
        return exprtk_val_bytes(tstr_v_from_buf("", 0));
    }
    exprtk_value_t result = DB_ZERO;
    if (exprtk_value_copy_to_env(
            exprtk_val_bytes(tstr_v_from_buf((char *)serialized, len)), ud->env,
            &result) != 0) {
        data_bind_binary_free(serialized);
        DB_ERROR(ud, "data_bind.object_serialize_binary: OOM");
        return exprtk_val_bytes(tstr_v_from_buf("", 0));
    }
    data_bind_binary_free(serialized);
    return result;
}

static exprtk_value_t fn_db_object_close(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    if (!ud || argc != 1 || args[0].type != EXPRTK_VAL_NUMBER ||
        !db_object_handle_free(ud->ctx, (int)args[0].data.number)) {
        DB_ERROR(ud, "data_bind.object_close: expected valid object handle");
        return exprtk_val_num(-1.0);
    }
    return DB_ZERO;
}

static char *db_read_file_text(db_ud_t *ud, const char *path, size_t *out_len) {
    FILE *f;
    long size;
    size_t nread;
    mem_pool_t *pool;
    char *buf;

    if (!ud || !ud->scratch || !path) return NULL;
    if (out_len) *out_len = 0;
    pool = ud->scratch;

    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (char *)mem_alloc(pool, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (out_len) *out_len = nread;
    buf[nread] = '\0';
    return buf;
}

static int db_get_codec_and_type(db_ud_t *ud, exprtk_value_t *args,
                                 const char *fn_name, DataBind **out_codec,
                                 char **out_type_name) {
    DataBind *codec;
    char *type_name;
    int h;

    if (!ud || !args || !fn_name || !out_codec || !out_type_name) return 0;

    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind stream: invalid handle");
        return 0;
    }

    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) {
        DB_ERROR(ud, "data_bind stream: OOM");
        return 0;
    }

    (void)fn_name;
    *out_codec = codec;
    *out_type_name = type_name;
    return 1;
}

static char *db_arg_cstr(db_ud_t *ud, const exprtk_value_t *arg, const char *fn_name) {
    char *text;

    if (!ud || !arg || arg->type != EXPRTK_VAL_STRING) return NULL;
    text = db_arena_cstr(ud->scratch, arg->data.string.data, arg->data.string.len);
    if (!text) DB_ERROR(ud, fn_name ? fn_name : "data_bind stream: OOM");
    return text;
}

typedef enum db_stream_mode {
    DB_STREAM_JSON,
    DB_STREAM_JSON_ALL,
    DB_STREAM_JSON_PATH,
    DB_STREAM_JSON_PATH_ALL,
    DB_STREAM_YAML,
    DB_STREAM_YAML_ALL,
    DB_STREAM_YAML_PATH,
    DB_STREAM_YAML_PATH_ALL,
    DB_STREAM_CSV_ALL,
    DB_STREAM_CSV_PATH,
    DB_STREAM_XML,
    DB_STREAM_XML_PATH_ALL
} db_stream_mode_t;

static data_bind_stream_t *db_stream_open(DataBind *codec, const char *type_name,
                                          db_stream_mode_t mode, const char *expr,
                                          DataBindValue **out_result,
                                          DataBindError *error) {
    switch (mode) {
    case DB_STREAM_JSON:
        return data_bind_stream_json_create(codec, type_name, out_result, error);
    case DB_STREAM_JSON_ALL:
        return data_bind_stream_json_all_create(codec, type_name, out_result, error);
    case DB_STREAM_JSON_PATH:
        return data_bind_stream_json_path_create(codec, type_name, expr, out_result, error);
    case DB_STREAM_JSON_PATH_ALL:
        return data_bind_stream_json_path_all_create(codec, type_name, expr, out_result, error);
    case DB_STREAM_YAML:
        return data_bind_stream_yaml_create(codec, type_name, out_result, error);
    case DB_STREAM_YAML_ALL:
        return data_bind_stream_yaml_all_create(codec, type_name, out_result, error);
    case DB_STREAM_YAML_PATH:
        return data_bind_stream_yaml_path_create(codec, type_name, expr, out_result, error);
    case DB_STREAM_YAML_PATH_ALL:
        return data_bind_stream_yaml_path_all_create(codec, type_name, expr, out_result, error);
    case DB_STREAM_CSV_ALL:
        return data_bind_stream_csv_all_create(codec, type_name, out_result, error);
    case DB_STREAM_CSV_PATH:
        return data_bind_stream_csv_path_create(codec, type_name, expr, out_result, error);
    case DB_STREAM_XML:
        return data_bind_stream_xml_create(codec, type_name, out_result, error);
    case DB_STREAM_XML_PATH_ALL:
        return data_bind_stream_xml_path_all_create(codec, type_name, expr, out_result, error);
    }
    return NULL;
}

static exprtk_value_t db_stream_text(db_ud_t *ud, DataBind *codec, const char *type_name,
                                     db_stream_mode_t mode, const char *path_or_expr,
                                     const char *data, size_t len) {
    data_bind_stream_t *stream;
    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    int status = DATA_BIND_ERR_RUNTIME;

    stream = db_stream_open(codec, type_name, mode, path_or_expr, &result, &err);
    if (stream) {
        status = data_bind_stream_feed(stream, data, len);
        if (status == DATA_BIND_OK) status = data_bind_stream_finish(stream);
        data_bind_stream_destroy(stream);
    }

    return db_convert_status_result(ud, (DataBindStatus)status, result, &err);
}

static exprtk_value_t db_stream_file(db_ud_t *ud, DataBind *codec, const char *type_name,
                                     db_stream_mode_t mode, const char *path_or_expr,
                                     const char *file_path) {
    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    data_bind_stream_t *stream;
    int status = DATA_BIND_ERR_RUNTIME;

    stream = db_stream_open(codec, type_name, mode, path_or_expr, &result, &err);
    if (stream) {
        status = data_bind_stream_feed_file(stream, file_path);
        if (status == DATA_BIND_OK) status = data_bind_stream_finish(stream);
        data_bind_stream_destroy(stream);
    }

    return db_convert_status_result(ud, (DataBindStatus)status, result, &err);
}

static exprtk_value_t db_stream_text_no_expr(size_t argc, exprtk_value_t *args, void *ud_,
                                             const char *fn_name, db_stream_mode_t mode) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    char *type_name;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    return db_stream_text(ud, codec, type_name, mode, NULL,
                          args[2].data.string.data, args[2].data.string.len);
}

static exprtk_value_t db_stream_text_with_expr(size_t argc, exprtk_value_t *args, void *ud_,
                                               const char *fn_name, db_stream_mode_t mode) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    char *type_name;
    char *expr;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    expr = db_arg_cstr(ud, &args[3], fn_name);
    if (!expr) return DB_ZERO;
    return db_stream_text(ud, codec, type_name, mode, expr,
                          args[2].data.string.data, args[2].data.string.len);
}

static exprtk_value_t db_stream_file_no_expr(size_t argc, exprtk_value_t *args, void *ud_,
                                             const char *fn_name, db_stream_mode_t mode) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    char *type_name;
    char *file_path;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    file_path = db_arg_cstr(ud, &args[2], fn_name);
    if (!file_path) return DB_ZERO;
    return db_stream_file(ud, codec, type_name, mode, NULL, file_path);
}

static exprtk_value_t db_stream_file_with_expr(size_t argc, exprtk_value_t *args, void *ud_,
                                               const char *fn_name, db_stream_mode_t mode) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    char *type_name;
    char *file_path;
    char *expr;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    file_path = db_arg_cstr(ud, &args[2], fn_name);
    expr = db_arg_cstr(ud, &args[3], fn_name);
    if (!file_path || !expr) return DB_ZERO;
    return db_stream_file(ud, codec, type_name, mode, expr, file_path);
}

static exprtk_value_t fn_db_json(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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

static exprtk_value_t fn_db_json_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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

static exprtk_value_t fn_db_json_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name, *path;
    DataBind *codec;
    int h;
    size_t len = 0;
    char *json_text;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.json_path: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.json_path: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.json_path: OOM"); return DB_ZERO; }
    path = db_arena_cstr(ud->scratch, args[2].data.string.data, args[2].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.json_path: OOM"); return DB_ZERO; }

    json_text = db_read_file_text(ud, path, &len);
    if (!json_text) { DB_ERROR(ud, "data_bind.json_path: failed to read file"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_json(codec, type_name, json_text, len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_json_all_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name, *path;
    DataBind *codec;
    int h;
    size_t len = 0;
    char *json_text;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.json_all_path: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.json_all_path: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.json_all_path: OOM"); return DB_ZERO; }
    path = db_arena_cstr(ud->scratch, args[2].data.string.data, args[2].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.json_all_path: OOM"); return DB_ZERO; }
    json_text = db_read_file_text(ud, path, &len);
    if (!json_text) { DB_ERROR(ud, "data_bind.json_all_path: failed to read file"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_json_all(
            codec, type_name, json_text, len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

typedef DataBindStatus (*db_parse_text_fn)(DataBind *, const char *, const char *, size_t,
                                            DataBindValue **, DataBindError *);
typedef DataBindStatus (*db_parse_text_path_fn)(DataBind *, const char *, const char *, size_t,
                                                 const char *, DataBindValue **, DataBindError *);

static exprtk_value_t db_parse_yaml_text(size_t argc, exprtk_value_t *args, void *ud_,
                                         const char *fn_name, db_parse_text_fn parse) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    status = parse(codec, type_name, args[2].data.string.data,
                   args[2].data.string.len, &result, &err);
    return db_convert_status_result(ud, status, result, &err);
}

static exprtk_value_t db_parse_yaml_file(size_t argc, exprtk_value_t *args, void *ud_,
                                         const char *fn_name, db_parse_text_fn parse) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;
    char *file_path;
    char *text;
    size_t len = 0;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    file_path = db_arg_cstr(ud, &args[2], fn_name);
    if (!file_path) return DB_ZERO;
    text = db_read_file_text(ud, file_path, &len);
    if (!text) {
        DB_ERROR(ud, "data_bind YAML: failed to read file");
        return DB_ZERO;
    }
    status = parse(codec, type_name, text, len, &result, &err);
    return db_convert_status_result(ud, status, result, &err);
}

static exprtk_value_t db_parse_yaml_text_path(size_t argc, exprtk_value_t *args, void *ud_,
                                              const char *fn_name,
                                              db_parse_text_path_fn parse) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;
    char *yaml_path;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    yaml_path = db_arg_cstr(ud, &args[3], fn_name);
    if (!yaml_path) return DB_ZERO;
    status = parse(codec, type_name, args[2].data.string.data,
                   args[2].data.string.len, yaml_path, &result, &err);
    return db_convert_status_result(ud, status, result, &err);
}

static exprtk_value_t db_parse_yaml_file_path(size_t argc, exprtk_value_t *args, void *ud_,
                                              const char *fn_name,
                                              db_parse_text_path_fn parse) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    DataBindValue *result = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    char *type_name;
    char *file_path;
    char *yaml_path;
    char *text;
    size_t len = 0;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return DB_ZERO;
    }
    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) return DB_ZERO;
    file_path = db_arg_cstr(ud, &args[2], fn_name);
    yaml_path = db_arg_cstr(ud, &args[3], fn_name);
    if (!file_path || !yaml_path) return DB_ZERO;
    text = db_read_file_text(ud, file_path, &len);
    if (!text) {
        DB_ERROR(ud, "data_bind YAML path: failed to read file");
        return DB_ZERO;
    }
    status = parse(codec, type_name, text, len, yaml_path, &result, &err);
    return db_convert_status_result(ud, status, result, &err);
}

static exprtk_value_t fn_db_yaml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_text(argc, args, ud_,
        "data_bind.yaml: expected (number, string, string)", data_bind_parse_yaml);
}

static exprtk_value_t fn_db_yaml_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_text(argc, args, ud_,
        "data_bind.yaml_all: expected (number, string, string)", data_bind_parse_yaml_all);
}

static exprtk_value_t fn_db_yaml_ypath(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_text_path(argc, args, ud_,
        "data_bind.yaml_ypath: expected (number, string, string, string)",
        data_bind_parse_yaml_path);
}

static exprtk_value_t fn_db_yaml_ypath_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_text_path(argc, args, ud_,
        "data_bind.yaml_ypath_all: expected (number, string, string, string)",
        data_bind_parse_yaml_path_all);
}

static exprtk_value_t fn_db_yaml_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_file(argc, args, ud_,
        "data_bind.yaml_path: expected (number, string, string)", data_bind_parse_yaml);
}

static exprtk_value_t fn_db_yaml_all_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_file(argc, args, ud_,
        "data_bind.yaml_all_path: expected (number, string, string)", data_bind_parse_yaml_all);
}

static exprtk_value_t fn_db_yaml_ypath_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_file_path(argc, args, ud_,
        "data_bind.yaml_ypath_path: expected (number, string, string, string)",
        data_bind_parse_yaml_path);
}

static exprtk_value_t fn_db_yaml_ypath_all_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_parse_yaml_file_path(argc, args, ud_,
        "data_bind.yaml_ypath_all_path: expected (number, string, string, string)",
        data_bind_parse_yaml_path_all);
}

static exprtk_value_t fn_db_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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

static exprtk_value_t fn_db_csv_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name, *path;
    DataBind *codec;
    int h;
    int row;
    size_t len = 0;
    char *csv_text;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_NUMBER) {
        DB_ERROR(ud, "data_bind.csv_path: expected (number, string, string, number)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    row = (int)args[3].data.number;
    if (row < 0) return DB_ZERO;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.csv_path: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.csv_path: OOM"); return DB_ZERO; }
    path = db_arena_cstr(ud->scratch, args[2].data.string.data, args[2].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.csv_path: OOM"); return DB_ZERO; }
    csv_text = db_read_file_text(ud, path, &len);
    if (!csv_text) { DB_ERROR(ud, "data_bind.csv_path: failed to read file"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_csv(
            codec, type_name, csv_text, len, (size_t)row, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_csv_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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

static exprtk_value_t fn_db_csv_all_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name, *path;
    DataBind *codec;
    int h;
    size_t len = 0;
    char *csv_text;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.csv_all_path: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.csv_all_path: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.csv_all_path: OOM"); return DB_ZERO; }
    path = db_arena_cstr(ud->scratch, args[2].data.string.data, args[2].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.csv_all_path: OOM"); return DB_ZERO; }
    csv_text = db_read_file_text(ud, path, &len);
    if (!csv_text) { DB_ERROR(ud, "data_bind.csv_all_path: failed to read file"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_csv_all(
            codec, type_name, csv_text, len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_xml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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

static exprtk_value_t fn_db_xml_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name, *path;
    DataBind *codec;
    int h;
    size_t len = 0;
    char *xml_text;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.xml_path: expected (number, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.xml_path: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    if (!type_name) { DB_ERROR(ud, "data_bind.xml_path: OOM"); return DB_ZERO; }
    path = db_arena_cstr(ud->scratch, args[2].data.string.data, args[2].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.xml_path: OOM"); return DB_ZERO; }
    xml_text = db_read_file_text(ud, path, &len);
    if (!xml_text) { DB_ERROR(ud, "data_bind.xml_path: failed to read file"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_xml(codec, type_name, xml_text, len, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_xml_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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
        DataBindStatus status = data_bind_parse_xml_path_all(codec, type_name, args[2].data.string.data,
                                                        args[2].data.string.len, xpath,
                                                        &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_xml_all_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    char *type_name, *path, *xpath;
    DataBind *codec;
    int h;
    size_t len = 0;
    char *xml_text;

    if (argc != 4 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        args[3].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.xml_all_path: expected (number, string, string, string)");
        return DB_ZERO;
    }
    h = (int)args[0].data.number;
    codec = db_handle_get(ud->ctx, h);
    if (!codec) {
        DB_ERROR(ud, "data_bind.xml_all_path: invalid handle");
        return DB_ZERO;
    }
    type_name = db_arena_cstr(ud->scratch, args[1].data.string.data, args[1].data.string.len);
    xpath = db_arena_cstr(ud->scratch, args[3].data.string.data, args[3].data.string.len);
    if (!type_name || !xpath) { DB_ERROR(ud, "data_bind.xml_all_path: OOM"); return DB_ZERO; }
    path = db_arena_cstr(ud->scratch, args[2].data.string.data, args[2].data.string.len);
    if (!path) { DB_ERROR(ud, "data_bind.xml_all_path: OOM"); return DB_ZERO; }
    xml_text = db_read_file_text(ud, path, &len);
    if (!xml_text) { DB_ERROR(ud, "data_bind.xml_all_path: failed to read file"); return DB_ZERO; }
    {
        DataBindValue *result = NULL;
        DataBindError err = DATA_BIND_ERROR_INIT;
        DataBindStatus status = data_bind_parse_xml_path_all(
            codec, type_name, xml_text, len, xpath, &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_json_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.json_stream: expected (number, string, string)",
        DB_STREAM_JSON);
}

static exprtk_value_t fn_db_json_all_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.json_all_stream: expected (number, string, string)",
        DB_STREAM_JSON_ALL);
}

static exprtk_value_t fn_db_json_path_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.json_path_stream: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH);
}

static exprtk_value_t fn_db_json_path_all_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.json_path_all_stream: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH_ALL);
}

static exprtk_value_t fn_db_yaml_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.yaml_stream: expected (number, string, string)",
        DB_STREAM_YAML);
}

static exprtk_value_t fn_db_yaml_all_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.yaml_all_stream: expected (number, string, string)",
        DB_STREAM_YAML_ALL);
}

static exprtk_value_t fn_db_yaml_path_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.yaml_path_stream: expected (number, string, string, string)",
        DB_STREAM_YAML_PATH);
}

static exprtk_value_t fn_db_yaml_path_all_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.yaml_path_all_stream: expected (number, string, string, string)",
        DB_STREAM_YAML_PATH_ALL);
}

static exprtk_value_t fn_db_csv_all_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.csv_all_stream: expected (number, string, string)",
        DB_STREAM_CSV_ALL);
}

static exprtk_value_t fn_db_csv_path_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.csv_path_stream: expected (number, string, string, string)",
        DB_STREAM_CSV_PATH);
}

static exprtk_value_t fn_db_xml_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.xml_stream: expected (number, string, string)",
        DB_STREAM_XML);
}

static exprtk_value_t fn_db_xml_path_all_stream(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.xml_path_all_stream: expected (number, string, string, string)",
        DB_STREAM_XML_PATH_ALL);
}

static exprtk_value_t fn_db_json_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.json_stream_path: expected (number, string, string)",
        DB_STREAM_JSON);
}

static exprtk_value_t fn_db_json_all_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.json_all_stream_path: expected (number, string, string)",
        DB_STREAM_JSON_ALL);
}

static exprtk_value_t fn_db_json_path_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.json_path_stream_path: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH);
}

static exprtk_value_t fn_db_json_path_all_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.json_path_all_stream_path: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH_ALL);
}

static exprtk_value_t fn_db_yaml_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.yaml_stream_path: expected (number, string, string)",
        DB_STREAM_YAML);
}

static exprtk_value_t fn_db_yaml_all_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.yaml_all_stream_path: expected (number, string, string)",
        DB_STREAM_YAML_ALL);
}

static exprtk_value_t fn_db_yaml_path_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.yaml_path_stream_path: expected (number, string, string, string)",
        DB_STREAM_YAML_PATH);
}

static exprtk_value_t fn_db_yaml_path_all_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.yaml_path_all_stream_path: expected (number, string, string, string)",
        DB_STREAM_YAML_PATH_ALL);
}

static exprtk_value_t fn_db_csv_all_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.csv_all_stream_path: expected (number, string, string)",
        DB_STREAM_CSV_ALL);
}

static exprtk_value_t fn_db_csv_path_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.csv_path_stream_path: expected (number, string, string, string)",
        DB_STREAM_CSV_PATH);
}

static exprtk_value_t fn_db_xml_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.xml_stream_path: expected (number, string, string)",
        DB_STREAM_XML);
}

static exprtk_value_t fn_db_xml_path_all_stream_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.xml_path_all_stream_path: expected (number, string, string, string)",
        DB_STREAM_XML_PATH_ALL);
}

static exprtk_value_t db_stream_create_no_expr(size_t argc, exprtk_value_t *args, void *ud_,
                                               const char *fn_name, db_stream_mode_t mode) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    db_stream_entry_t *entry;
    char *type_name;
    int stream_handle;

    if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return exprtk_val_num(-1.0);
    }

    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) {
        return exprtk_val_num(-1.0);
    }

    entry = (db_stream_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) {
        DB_ERROR(ud, "data_bind sax create: OOM");
        return exprtk_val_num(-1.0);
    }
    entry->codec = codec;
    entry->env = ud->env;
    entry->error = (DataBindError)DATA_BIND_ERROR_INIT;
    entry->stream = db_stream_open(codec, type_name, mode, NULL,
                                   &entry->result, &entry->error);
    if (!entry->stream) {
        DB_ERROR(ud, entry->error.message[0] ? entry->error.message : "data_bind sax create: failed");
        free(entry);
        return exprtk_val_num(-1.0);
    }

    stream_handle = db_stream_handle_alloc(ud->ctx, entry);
    if (stream_handle < 0) {
        data_bind_stream_destroy(entry->stream);
        free(entry);
        DB_ERROR(ud, "data_bind sax create: too many open streams");
        return exprtk_val_num(-1.0);
    }

    return exprtk_val_num((double)stream_handle);
}

static exprtk_value_t db_stream_create_with_expr(size_t argc, exprtk_value_t *args, void *ud_,
                                                 const char *fn_name, db_stream_mode_t mode) {
    db_ud_t *ud = (db_ud_t *)ud_;
    DataBind *codec;
    db_stream_entry_t *entry;
    char *type_name;
    char *expr;
    int stream_handle;

    if (argc != 3 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, fn_name);
        return exprtk_val_num(-1.0);
    }

    if (!db_get_codec_and_type(ud, args, fn_name, &codec, &type_name)) {
        return exprtk_val_num(-1.0);
    }

    expr = db_arg_cstr(ud, &args[2], fn_name);
    if (!expr) return exprtk_val_num(-1.0);

    entry = (db_stream_entry_t *)calloc(1, sizeof(*entry));
    if (!entry) {
        DB_ERROR(ud, "data_bind sax create: OOM");
        return exprtk_val_num(-1.0);
    }
    entry->codec = codec;
    entry->env = ud->env;
    entry->error = (DataBindError)DATA_BIND_ERROR_INIT;
    entry->stream = db_stream_open(codec, type_name, mode, expr,
                                   &entry->result, &entry->error);
    if (!entry->stream) {
        DB_ERROR(ud, entry->error.message[0] ? entry->error.message : "data_bind sax create: failed");
        free(entry);
        return exprtk_val_num(-1.0);
    }

    stream_handle = db_stream_handle_alloc(ud->ctx, entry);
    if (stream_handle < 0) {
        data_bind_stream_destroy(entry->stream);
        free(entry);
        DB_ERROR(ud, "data_bind sax create: too many open streams");
        return exprtk_val_num(-1.0);
    }

    return exprtk_val_num((double)stream_handle);
}

static exprtk_value_t fn_db_sax_json_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.json_create: expected (number, string)",
        DB_STREAM_JSON);
}

static exprtk_value_t fn_db_sax_json_all_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.json_all_create: expected (number, string)",
        DB_STREAM_JSON_ALL);
}

static exprtk_value_t fn_db_sax_json_path_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.json_path_create: expected (number, string, string)",
        DB_STREAM_JSON_PATH);
}

static exprtk_value_t fn_db_sax_json_path_all_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.json_path_all_create: expected (number, string, string)",
        DB_STREAM_JSON_PATH_ALL);
}

static exprtk_value_t fn_db_sax_yaml_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.yaml_create: expected (number, string)",
        DB_STREAM_YAML);
}

static exprtk_value_t fn_db_sax_yaml_all_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.yaml_all_create: expected (number, string)",
        DB_STREAM_YAML_ALL);
}

static exprtk_value_t fn_db_sax_yaml_path_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.yaml_path_create: expected (number, string, string)",
        DB_STREAM_YAML_PATH);
}

static exprtk_value_t fn_db_sax_yaml_path_all_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.yaml_path_all_create: expected (number, string, string)",
        DB_STREAM_YAML_PATH_ALL);
}

static exprtk_value_t fn_db_sax_csv_all_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.csv_all_create: expected (number, string)",
        DB_STREAM_CSV_ALL);
}

static exprtk_value_t fn_db_sax_csv_path_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.csv_path_create: expected (number, string, string)",
        DB_STREAM_CSV_PATH);
}

static exprtk_value_t fn_db_sax_xml_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.xml_create: expected (number, string)",
        DB_STREAM_XML);
}

static exprtk_value_t fn_db_sax_xml_path_all_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.xml_path_all_create: expected (number, string, string)",
        DB_STREAM_XML_PATH_ALL);
}

static void db_callback_value_free(exprtk_value_t *value) {
    size_t i;

    if (!value) return;
    if (value->type == EXPRTK_VAL_LIST || value->type == EXPRTK_VAL_SET) {
        for (i = 0; i < value->data.list.count; ++i) {
            db_callback_value_free(&value->data.list.items[i]);
        }
        if (value->data.list.heap_owned) {
            turbo_vec_t vec = {
                value->data.list.items,
                value->data.list.count,
                value->data.list.capacity,
                sizeof(exprtk_value_t)
            };
            turbo_vec_destroy(&vec);
        }
        value->data.list.items = NULL;
        value->data.list.count = 0;
        value->data.list.capacity = 0;
        value->data.list.heap_owned = 0;
    } else if (exprtk_value_is_object_like(value)) {
        exprtk_map_iter_t it = exprtk_map_iter_begin(value);
        exprtk_value_t child;
        while (exprtk_map_iter_next(&it, NULL, &child)) {
            db_callback_value_free(&child);
        }
        exprtk_map_free(value);
    }
}

static DataBindRecordAction db_record_callback(void *user_data,
                                               const DataBindValue *record,
                                               uint64_t record_index) {
    db_stream_entry_t *entry = (db_stream_entry_t *)user_data;
    exprtk_value_t args[2];
    exprtk_value_t result;
    DataBindRecordAction action;

    if (!entry || !entry->env || !record ||
        entry->record_callback.type != EXPRTK_VAL_FUNCTION) {
        return DATA_BIND_RECORD_ERROR;
    }

    args[0] = db_value_to_exprtk(&(db_ud_t){.env = entry->env}, record);
    args[1] = exprtk_val_int((int64_t)record_index);
    result = exprtk_call_function_value(entry->record_callback, 2, args, entry->env);
    if (entry->env->aborted || entry->env->flow == exprtk_FLOW_THROW) {
        action = DATA_BIND_RECORD_ERROR;
    } else if (result.type == EXPRTK_VAL_BOOL) {
        action = result.data.boolean ? DATA_BIND_RECORD_STOP : DATA_BIND_RECORD_CONTINUE;
    } else if (result.type == EXPRTK_VAL_NUMBER) {
        action = result.data.number < 0.0 ? DATA_BIND_RECORD_ERROR :
                 result.data.number > 0.0 ? DATA_BIND_RECORD_STOP : DATA_BIND_RECORD_CONTINUE;
    } else if (result.type == EXPRTK_VAL_INTEGER) {
        action = result.data.integer < 0 ? DATA_BIND_RECORD_ERROR :
                 result.data.integer > 0 ? DATA_BIND_RECORD_STOP : DATA_BIND_RECORD_CONTINUE;
    } else {
        action = DATA_BIND_RECORD_ERROR;
    }

    db_callback_value_free(&args[0]);
    return action;
}

static exprtk_value_t fn_db_stream_set_callback(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_stream_entry_t *entry;
    DataBindStatus status;

    if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER ||
        args[1].type != EXPRTK_VAL_FUNCTION) {
        DB_ERROR(ud, "data_bind.sax.set_callback: expected (number, function)");
        return exprtk_val_bool(0);
    }

    entry = db_stream_handle_get(ud->ctx, (int)args[0].data.number);
    if (!entry) {
        DB_ERROR(ud, "data_bind.sax.set_callback: invalid stream handle");
        return exprtk_val_bool(0);
    }

    entry->record_callback = args[1];
    status = data_bind_stream_set_record_callback(entry->stream, db_record_callback, entry);
    if (status != DATA_BIND_OK) {
        memset(&entry->record_callback, 0, sizeof(entry->record_callback));
        DB_ERROR(ud, entry->error.message[0] ? entry->error.message :
                     "data_bind.sax.set_callback: failed");
        return exprtk_val_bool(0);
    }
    return exprtk_val_bool(1);
}

static exprtk_value_t fn_db_stream_feed(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_stream_entry_t *entry;
    int h;

    if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.stream_feed: expected (number, string)");
        return DB_ZERO;
    }

    h = (int)args[0].data.number;
    entry = db_stream_handle_get(ud->ctx, h);
    if (!entry) {
        DB_ERROR(ud, "data_bind.stream_feed: invalid stream handle");
        return DB_ZERO;
    }

    if (data_bind_stream_feed(entry->stream, args[1].data.string.data,
                              args[1].data.string.len) != DATA_BIND_OK) {
        DB_ERROR(ud, entry->error.message[0] ? entry->error.message : "data_bind.stream_feed: failed");
        return DB_ZERO;
    }

    return DB_ZERO;
}

static exprtk_value_t fn_db_stream_feed_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_stream_entry_t *entry;
    char *file_path;
    int h;

    if (argc != 2 || args[0].type != EXPRTK_VAL_NUMBER || args[1].type != EXPRTK_VAL_STRING) {
        DB_ERROR(ud, "data_bind.stream_feed_path: expected (number, string)");
        return DB_ZERO;
    }

    h = (int)args[0].data.number;
    entry = db_stream_handle_get(ud->ctx, h);
    if (!entry) {
        DB_ERROR(ud, "data_bind.stream_feed_path: invalid stream handle");
        return DB_ZERO;
    }

    file_path = db_arg_cstr(ud, &args[1], "data_bind.stream_feed_path: OOM");
    if (!file_path) return DB_ZERO;

    if (data_bind_stream_feed_file(entry->stream, file_path) != DATA_BIND_OK) {
        DB_ERROR(ud, entry->error.message[0] ? entry->error.message : "data_bind.stream_feed_path: failed");
        return DB_ZERO;
    }

    return DB_ZERO;
}

static exprtk_value_t fn_db_stream_finish(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    db_stream_entry_t *entry;
    int status;
    int h;

    if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
        DB_ERROR(ud, "data_bind.stream_finish: expected number stream_handle");
        return DB_ZERO;
    }

    h = (int)args[0].data.number;
    entry = db_stream_handle_take(ud->ctx, h);
    if (!entry) {
        DB_ERROR(ud, "data_bind.stream_finish: invalid stream handle");
        return DB_ZERO;
    }

    status = data_bind_stream_finish(entry->stream);
    data_bind_stream_destroy(entry->stream);
    {
        DataBindValue *result = entry->result;
        DataBindError error = entry->error;
        free(entry);
        return db_convert_status_result(ud, (DataBindStatus)status, result, &error);
    }
}

static exprtk_value_t fn_db_stream_close(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;

    if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
        DB_ERROR(ud, "data_bind.stream_close: expected number stream_handle");
        return DB_ZERO;
    }

    db_stream_handle_free(ud->ctx, (int)args[0].data.number);
    return DB_ZERO;
}

/**
 * data_bind.close(handle) -> 0
 */
static exprtk_value_t fn_db_close(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
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
    if (!ctx || !env || !scratch) return;

    db_ud_t *ud = (db_ud_t *)mem_alloc(&env->arena, sizeof(*ud));
    if (!ud) return;
    ud->ctx     = ctx;
    ud->env     = env;
    ud->scratch = scratch;

    exprtk_env_register_func(env, "data_bind.create", fn_db_create, ud);
    exprtk_env_register_func(env, "data_bind.create_from_text", fn_db_create_from_text, ud);
    exprtk_env_register_func(env, "data_bind.object_from_binary", fn_db_object_from_binary, ud);
    exprtk_env_register_func(env, "data_bind.object_from_json", fn_db_object_from_json, ud);
    exprtk_env_register_func(env, "data_bind.object_from_yaml", fn_db_object_from_yaml, ud);
    exprtk_env_register_func(env, "data_bind.object_from_xml", fn_db_object_from_xml, ud);
    exprtk_env_register_func(env, "data_bind.object_from_csv", fn_db_object_from_csv, ud);
    exprtk_env_register_func(env, "data_bind.object_clone", fn_db_object_clone, ud);
    exprtk_env_register_func(env, "data_bind.object_type", fn_db_object_type, ud);
    exprtk_env_register_func(env, "data_bind.object_value", fn_db_object_value, ud);
    exprtk_env_register_func(env, "data_bind.object_serialize_json", fn_db_object_serialize_json, ud);
    exprtk_env_register_func(env, "data_bind.object_serialize_yaml", fn_db_object_serialize_yaml, ud);
    exprtk_env_register_func(env, "data_bind.object_serialize_xml", fn_db_object_serialize_xml, ud);
    exprtk_env_register_func(env, "data_bind.object_serialize_csv", fn_db_object_serialize_csv, ud);
    exprtk_env_register_func(env, "data_bind.object_serialize_binary", fn_db_object_serialize_binary, ud);
    exprtk_env_register_func(env, "data_bind.object_close", fn_db_object_close, ud);
    exprtk_env_register_func(env, "data_bind.parse",  fn_db_parse,  ud);
    exprtk_env_register_func(env, "data_bind.validate_json", fn_db_validate_json, ud);
    exprtk_env_register_func(env, "data_bind.validate_yaml", fn_db_validate_yaml, ud);
    exprtk_env_register_func(env, "data_bind.validate_yaml_path", fn_db_validate_yaml_path, ud);
    exprtk_env_register_func(env, "data_bind.validate_csv", fn_db_validate_csv, ud);
    exprtk_env_register_func(env, "data_bind.validate_xml", fn_db_validate_xml, ud);
    exprtk_env_register_func(env, "data_bind.json",   fn_db_json,   ud);
    exprtk_env_register_func(env, "data_bind.json_all", fn_db_json_all, ud);
    exprtk_env_register_func(env, "data_bind.json_path",   fn_db_json_path,   ud);
    exprtk_env_register_func(env, "data_bind.json_all_path", fn_db_json_all_path, ud);
    exprtk_env_register_func(env, "data_bind.yaml", fn_db_yaml, ud);
    exprtk_env_register_func(env, "data_bind.yaml_all", fn_db_yaml_all, ud);
    exprtk_env_register_func(env, "data_bind.yaml_ypath", fn_db_yaml_ypath, ud);
    exprtk_env_register_func(env, "data_bind.yaml_ypath_all", fn_db_yaml_ypath_all, ud);
    exprtk_env_register_func(env, "data_bind.yaml_path", fn_db_yaml_file, ud);
    exprtk_env_register_func(env, "data_bind.yaml_all_path", fn_db_yaml_all_file, ud);
    exprtk_env_register_func(env, "data_bind.yaml_ypath_path", fn_db_yaml_ypath_file, ud);
    exprtk_env_register_func(env, "data_bind.yaml_ypath_all_path", fn_db_yaml_ypath_all_file, ud);
    exprtk_env_register_func(env, "data_bind.csv",    fn_db_csv,    ud);
    exprtk_env_register_func(env, "data_bind.csv_all", fn_db_csv_all, ud);
    exprtk_env_register_func(env, "data_bind.csv_path",    fn_db_csv_path,    ud);
    exprtk_env_register_func(env, "data_bind.csv_all_path", fn_db_csv_all_path, ud);
    exprtk_env_register_func(env, "data_bind.xml",    fn_db_xml,    ud);
    exprtk_env_register_func(env, "data_bind.xml_all", fn_db_xml_all, ud);
    exprtk_env_register_func(env, "data_bind.xml_path",    fn_db_xml_path,    ud);
    exprtk_env_register_func(env, "data_bind.xml_all_path", fn_db_xml_all_path, ud);
    exprtk_env_register_func(env, "data_bind.dom.json",   fn_db_json,   ud);
    exprtk_env_register_func(env, "data_bind.dom.json_all", fn_db_json_all, ud);
    exprtk_env_register_func(env, "data_bind.dom.json_path",   fn_db_json_path,   ud);
    exprtk_env_register_func(env, "data_bind.dom.json_all_path", fn_db_json_all_path, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml", fn_db_yaml, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_all", fn_db_yaml_all, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_ypath", fn_db_yaml_ypath, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_ypath_all", fn_db_yaml_ypath_all, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_path", fn_db_yaml_file, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_all_path", fn_db_yaml_all_file, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_ypath_path", fn_db_yaml_ypath_file, ud);
    exprtk_env_register_func(env, "data_bind.dom.yaml_ypath_all_path", fn_db_yaml_ypath_all_file, ud);
    exprtk_env_register_func(env, "data_bind.dom.csv",    fn_db_csv,    ud);
    exprtk_env_register_func(env, "data_bind.dom.csv_all", fn_db_csv_all, ud);
    exprtk_env_register_func(env, "data_bind.dom.csv_path",    fn_db_csv_path,    ud);
    exprtk_env_register_func(env, "data_bind.dom.csv_all_path", fn_db_csv_all_path, ud);
    exprtk_env_register_func(env, "data_bind.dom.xml",    fn_db_xml,    ud);
    exprtk_env_register_func(env, "data_bind.dom.xml_all", fn_db_xml_all, ud);
    exprtk_env_register_func(env, "data_bind.dom.xml_path",    fn_db_xml_path,    ud);
    exprtk_env_register_func(env, "data_bind.dom.xml_all_path", fn_db_xml_all_path, ud);
    exprtk_env_register_func(env, "data_bind.json_stream", fn_db_json_stream, ud);
    exprtk_env_register_func(env, "data_bind.json_all_stream", fn_db_json_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.json_path_stream", fn_db_json_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.json_path_all_stream", fn_db_json_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.yaml_stream", fn_db_yaml_stream, ud);
    exprtk_env_register_func(env, "data_bind.yaml_all_stream", fn_db_yaml_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.yaml_path_stream", fn_db_yaml_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.yaml_path_all_stream", fn_db_yaml_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.csv_all_stream", fn_db_csv_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.csv_path_stream", fn_db_csv_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.xml_stream", fn_db_xml_stream, ud);
    exprtk_env_register_func(env, "data_bind.xml_path_all_stream", fn_db_xml_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.json_stream_path", fn_db_json_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.json_all_stream_path", fn_db_json_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.json_path_stream_path", fn_db_json_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.json_path_all_stream_path", fn_db_json_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.yaml_stream_path", fn_db_yaml_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.yaml_all_stream_path", fn_db_yaml_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.yaml_path_stream_path", fn_db_yaml_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.yaml_path_all_stream_path", fn_db_yaml_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.csv_all_stream_path", fn_db_csv_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.csv_path_stream_path", fn_db_csv_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.xml_stream_path", fn_db_xml_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.xml_path_all_stream_path", fn_db_xml_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json", fn_db_json_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_all", fn_db_json_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path", fn_db_json_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_all", fn_db_json_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml", fn_db_yaml_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_all", fn_db_yaml_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_path", fn_db_yaml_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_path_all", fn_db_yaml_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_all", fn_db_csv_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_path", fn_db_csv_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml", fn_db_xml_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_path_all", fn_db_xml_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_file", fn_db_json_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_all_file", fn_db_json_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_file", fn_db_json_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_all_file", fn_db_json_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_file", fn_db_yaml_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_all_file", fn_db_yaml_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_path_file", fn_db_yaml_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_path_all_file", fn_db_yaml_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_all_file", fn_db_csv_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_path_file", fn_db_csv_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_file", fn_db_xml_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_path_all_file", fn_db_xml_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_create", fn_db_sax_json_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_all_create", fn_db_sax_json_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_create", fn_db_sax_json_path_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_all_create", fn_db_sax_json_path_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_create", fn_db_sax_yaml_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_all_create", fn_db_sax_yaml_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_path_create", fn_db_sax_yaml_path_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.yaml_path_all_create", fn_db_sax_yaml_path_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_all_create", fn_db_sax_csv_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_path_create", fn_db_sax_csv_path_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_create", fn_db_sax_xml_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_path_all_create", fn_db_sax_xml_path_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.set_callback", fn_db_stream_set_callback, ud);
    exprtk_env_register_func(env, "data_bind.sax.feed", fn_db_stream_feed, ud);
    exprtk_env_register_func(env, "data_bind.sax.feed_path", fn_db_stream_feed_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.finish", fn_db_stream_finish, ud);
    exprtk_env_register_func(env, "data_bind.sax.close", fn_db_stream_close, ud);
    exprtk_env_register_func(env, "data_bind.close",  fn_db_close,  ud);
}
