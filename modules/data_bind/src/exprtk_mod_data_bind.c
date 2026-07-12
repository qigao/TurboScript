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
 */
#include "data_bind_ctx.h"

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
    if (turbo_hash_map_reserve(&ctx->handles.raw, DB_MAX_HANDLES) != 0 ||
        turbo_hash_map_reserve(&ctx->streams.raw, DB_MAX_HANDLES) != 0) {
        db_stream_map_t_destroy(&ctx->streams);
        db_handle_map_t_destroy(&ctx->handles);
        free(ctx);
        return NULL;
    }
    ctx->next_handle = 0;
    ctx->next_stream_handle = 0;
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
        DataBind **codec_ptr = db_handle_map_t_get(&ctx->handles, i);
        if (codec_ptr && *codec_ptr) {
            data_bind_free(*codec_ptr);
        }
    }
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

static void db_handle_free(db_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= DB_MAX_HANDLES) return;

    DataBind *codec = NULL;
    if (db_handle_map_t_remove(&ctx->handles, h, &codec)) {
        db_stream_close_for_codec(ctx, codec);
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
 * data_bind.create_from_text(schema_text) -> number (handle >= 0, or -1 on error)
 */
static exprtk_value_t fn_db_create_from_text(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_validate_json(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_validate_text(argc, args, ud_,
                            "data_bind.validate_json: expected (number, string, string)",
                            data_bind_validate_json);
}

static exprtk_value_t fn_db_validate_csv(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_validate_text(argc, args, ud_,
                            "data_bind.validate_csv: expected (number, string, string)",
                            data_bind_validate_csv);
}

static exprtk_value_t fn_db_validate_xml(size_t argc, exprtk_value_t *args, void *ud_) {
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
        if (result) data_bind_value_free(result);
        if (err && err->message[0]) DB_ERROR(ud, err->message);
        return DB_ZERO;
    }
    return db_convert_result(ud, result);
}

static char *db_read_file_text(db_ud_t *ud, const char *path, size_t *out_len) {
    FILE *f;
    long size;
    size_t nread;
    mem_pool_t *pool;
    char *buf;

    if (!ud || !ud->env || !path) return NULL;
    if (out_len) *out_len = 0;
    pool = ud->scratch ? ud->scratch : &ud->env->arena;

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

static exprtk_value_t fn_db_json_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_json_all_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_csv_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_csv_all_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_xml_path(size_t argc, exprtk_value_t *args, void *ud_) {
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
        DataBindStatus status = data_bind_parse_xml_path_all(codec, type_name, args[2].data.string.data,
                                                        args[2].data.string.len, xpath,
                                                        &result, &err);
        return db_convert_status_result(ud, status, result, &err);
    }
}

static exprtk_value_t fn_db_xml_all_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_json_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.json_stream: expected (number, string, string)",
        DB_STREAM_JSON);
}

static exprtk_value_t fn_db_json_all_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.json_all_stream: expected (number, string, string)",
        DB_STREAM_JSON_ALL);
}

static exprtk_value_t fn_db_json_path_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.json_path_stream: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH);
}

static exprtk_value_t fn_db_json_path_all_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.json_path_all_stream: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH_ALL);
}

static exprtk_value_t fn_db_csv_all_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.csv_all_stream: expected (number, string, string)",
        DB_STREAM_CSV_ALL);
}

static exprtk_value_t fn_db_csv_path_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.csv_path_stream: expected (number, string, string, string)",
        DB_STREAM_CSV_PATH);
}

static exprtk_value_t fn_db_xml_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_no_expr(argc, args, ud_,
        "data_bind.xml_stream: expected (number, string, string)",
        DB_STREAM_XML);
}

static exprtk_value_t fn_db_xml_path_all_stream(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_text_with_expr(argc, args, ud_,
        "data_bind.xml_path_all_stream: expected (number, string, string, string)",
        DB_STREAM_XML_PATH_ALL);
}

static exprtk_value_t fn_db_json_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.json_stream_path: expected (number, string, string)",
        DB_STREAM_JSON);
}

static exprtk_value_t fn_db_json_all_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.json_all_stream_path: expected (number, string, string)",
        DB_STREAM_JSON_ALL);
}

static exprtk_value_t fn_db_json_path_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.json_path_stream_path: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH);
}

static exprtk_value_t fn_db_json_path_all_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.json_path_all_stream_path: expected (number, string, string, string)",
        DB_STREAM_JSON_PATH_ALL);
}

static exprtk_value_t fn_db_csv_all_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.csv_all_stream_path: expected (number, string, string)",
        DB_STREAM_CSV_ALL);
}

static exprtk_value_t fn_db_csv_path_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_with_expr(argc, args, ud_,
        "data_bind.csv_path_stream_path: expected (number, string, string, string)",
        DB_STREAM_CSV_PATH);
}

static exprtk_value_t fn_db_xml_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_file_no_expr(argc, args, ud_,
        "data_bind.xml_stream_path: expected (number, string, string)",
        DB_STREAM_XML);
}

static exprtk_value_t fn_db_xml_path_all_stream_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_sax_json_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.json_create: expected (number, string)",
        DB_STREAM_JSON);
}

static exprtk_value_t fn_db_sax_json_all_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.json_all_create: expected (number, string)",
        DB_STREAM_JSON_ALL);
}

static exprtk_value_t fn_db_sax_json_path_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.json_path_create: expected (number, string, string)",
        DB_STREAM_JSON_PATH);
}

static exprtk_value_t fn_db_sax_json_path_all_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.json_path_all_create: expected (number, string, string)",
        DB_STREAM_JSON_PATH_ALL);
}

static exprtk_value_t fn_db_sax_csv_all_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.csv_all_create: expected (number, string)",
        DB_STREAM_CSV_ALL);
}

static exprtk_value_t fn_db_sax_csv_path_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.csv_path_create: expected (number, string, string)",
        DB_STREAM_CSV_PATH);
}

static exprtk_value_t fn_db_sax_xml_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_no_expr(argc, args, ud_,
        "data_bind.sax.xml_create: expected (number, string)",
        DB_STREAM_XML);
}

static exprtk_value_t fn_db_sax_xml_path_all_create(size_t argc, exprtk_value_t *args, void *ud_) {
    return db_stream_create_with_expr(argc, args, ud_,
        "data_bind.sax.xml_path_all_create: expected (number, string, string)",
        DB_STREAM_XML_PATH_ALL);
}

static exprtk_value_t fn_db_stream_feed(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_stream_feed_path(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_stream_finish(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_db_stream_close(size_t argc, exprtk_value_t *args, void *ud_) {
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
    exprtk_env_register_func(env, "data_bind.create_from_text", fn_db_create_from_text, ud);
    exprtk_env_register_func(env, "data_bind.parse",  fn_db_parse,  ud);
    exprtk_env_register_func(env, "data_bind.validate_json", fn_db_validate_json, ud);
    exprtk_env_register_func(env, "data_bind.validate_csv", fn_db_validate_csv, ud);
    exprtk_env_register_func(env, "data_bind.validate_xml", fn_db_validate_xml, ud);
    exprtk_env_register_func(env, "data_bind.json",   fn_db_json,   ud);
    exprtk_env_register_func(env, "data_bind.json_all", fn_db_json_all, ud);
    exprtk_env_register_func(env, "data_bind.json_path",   fn_db_json_path,   ud);
    exprtk_env_register_func(env, "data_bind.json_all_path", fn_db_json_all_path, ud);
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
    exprtk_env_register_func(env, "data_bind.csv_all_stream", fn_db_csv_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.csv_path_stream", fn_db_csv_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.xml_stream", fn_db_xml_stream, ud);
    exprtk_env_register_func(env, "data_bind.xml_path_all_stream", fn_db_xml_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.json_stream_path", fn_db_json_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.json_all_stream_path", fn_db_json_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.json_path_stream_path", fn_db_json_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.json_path_all_stream_path", fn_db_json_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.csv_all_stream_path", fn_db_csv_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.csv_path_stream_path", fn_db_csv_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.xml_stream_path", fn_db_xml_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.xml_path_all_stream_path", fn_db_xml_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json", fn_db_json_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_all", fn_db_json_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path", fn_db_json_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_all", fn_db_json_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_all", fn_db_csv_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_path", fn_db_csv_path_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml", fn_db_xml_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_path_all", fn_db_xml_path_all_stream, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_file", fn_db_json_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_all_file", fn_db_json_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_file", fn_db_json_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_all_file", fn_db_json_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_all_file", fn_db_csv_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_path_file", fn_db_csv_path_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_file", fn_db_xml_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_path_all_file", fn_db_xml_path_all_stream_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_create", fn_db_sax_json_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_all_create", fn_db_sax_json_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_create", fn_db_sax_json_path_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.json_path_all_create", fn_db_sax_json_path_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_all_create", fn_db_sax_csv_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.csv_path_create", fn_db_sax_csv_path_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_create", fn_db_sax_xml_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.xml_path_all_create", fn_db_sax_xml_path_all_create, ud);
    exprtk_env_register_func(env, "data_bind.sax.feed", fn_db_stream_feed, ud);
    exprtk_env_register_func(env, "data_bind.sax.feed_path", fn_db_stream_feed_path, ud);
    exprtk_env_register_func(env, "data_bind.sax.finish", fn_db_stream_finish, ud);
    exprtk_env_register_func(env, "data_bind.sax.close", fn_db_stream_close, ud);
    exprtk_env_register_func(env, "data_bind.close",  fn_db_close,  ud);
}
