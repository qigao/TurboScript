/**
 * @file exprtk_mod_data_bind.c
 * @brief data_bind module for TurboScript.
 *
 * Exposes the following functions to scripts:
 *
 *   data_bind.create(schema_path)         -> number (codec handle)
 *   data_bind.parse(handle, type, bytes)  -> map   (parsed message object)
 *   data_bind.close(handle)               -> number (0)
 *   data_bind.error(handle)               -> string (last error)
 *
 * `bytes` is a TurboScript vector (double[]) that carries raw byte values
 * in range [0, 255].  The plugin converts it to a uint8_t[] buffer before
 * passing to the JIT-compiled parser.
 *
 * The DataBindValueApi callbacks build a native exprtk_value_t tree
 * (maps, lists, numbers, strings) directly into the env arena so that
 * the returned object lives as long as the script env.
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
 * DataBindValueApi callbacks
 *
 * All "Value *" pointers here are actually exprtk_value_t * cast to Value *.
 * The macros in data_bind.h use `typedef struct Value Value;` with no
 * definition — the host (us) owns the layout.  We use exprtk_value_t.
 * ══════════════════════════════════════════════════════════════════════════ */

/* We store the build context in a thread-local-ish pointer set before every
 * parse call and cleared after.  Since DataBind has no user_data slot in
 * ValueApi callbacks we use a module-level pointer. This is safe as long as
 * parse calls are not concurrent on the same codec — which matches the
 * single-threaded TurboScript model. */
static db_build_ctx_t *g_build_ctx = NULL;

/* Helper: arena-dup a C string */
static char *bc_dup(const char *s) {
    if (!s || !g_build_ctx) return NULL;
    size_t len = strlen(s);
    return db_arena_cstr(&g_build_ctx->env->arena, s, len);
}

/* ── Object / scalar callbacks ─────────────────────────────────────────── */

static Value *bc_create_object(void) {
    exprtk_value_t *v = (exprtk_value_t *)mem_alloc(
        &g_build_ctx->env->arena, sizeof(exprtk_value_t));
    if (!v) return NULL;
    *v = exprtk_val_map();
    return (Value *)v;
}

static void bc_set_field_int(Value *obj, const char *name, int32_t val) {
    exprtk_value_t *map = (exprtk_value_t *)obj;
    if (!map || !name) return;
    char *k = bc_dup(name);
    if (!k) return;
    exprtk_map_set(map, k, exprtk_val_num((double)val));
}

static void bc_set_field_int64(Value *obj, const char *name, int64_t val) {
    exprtk_value_t *map = (exprtk_value_t *)obj;
    if (!map || !name) return;
    char *k = bc_dup(name);
    if (!k) return;
    exprtk_map_set(map, k, exprtk_val_int(val));
}

static void bc_set_field_double(Value *obj, const char *name, double val) {
    exprtk_value_t *map = (exprtk_value_t *)obj;
    if (!map || !name) return;
    char *k = bc_dup(name);
    if (!k) return;
    exprtk_map_set(map, k, exprtk_val_num(val));
}

static void bc_set_field_string(Value *obj, const char *name, const char *val) {
    exprtk_value_t *map = (exprtk_value_t *)obj;
    if (!map || !name) return;
    char *k = bc_dup(name);
    if (!k) return;
    size_t vlen = val ? strlen(val) : 0;
    char  *vcopy = val ? db_arena_cstr(&g_build_ctx->env->arena, val, vlen) : NULL;
    tstr_v sv = {vcopy ? vcopy : "", vlen};
    exprtk_map_set(map, k, exprtk_val_str(sv));
}

static void bc_set_field_bytes(Value *obj, const char *name,
                               const uint8_t *data, size_t len) {
    exprtk_value_t *map = (exprtk_value_t *)obj;
    if (!map || !name) return;
    char *k = bc_dup(name);
    if (!k) return;
    /* Expose bytes as a numeric vector (each element = byte value 0-255) */
    double *buf = (double *)mem_alloc(&g_build_ctx->env->arena,
                                      len * sizeof(double));
    if (!buf) return;
    for (size_t i = 0; i < len; i++) buf[i] = (double)data[i];
    exprtk_map_set(map, k, exprtk_val_vec(buf, len));
}

/* ── List callbacks ─────────────────────────────────────────────────────── */

static Value *bc_create_list(void) {
    exprtk_value_t *v = (exprtk_value_t *)mem_alloc(
        &g_build_ctx->env->arena, sizeof(exprtk_value_t));
    if (!v) return NULL;
    *v = exprtk_val_list_empty();
    return (Value *)v;
}

static void bc_add_list_item_int(Value *list, int32_t val) {
    exprtk_value_t *l = (exprtk_value_t *)list;
    if (!l) return;
    exprtk_list_push(l, exprtk_val_num((double)val));
}

static void bc_add_list_item_int64(Value *list, int64_t val) {
    exprtk_value_t *l = (exprtk_value_t *)list;
    if (!l) return;
    exprtk_list_push(l, exprtk_val_int(val));
}

static void bc_add_list_item_double(Value *list, double val) {
    exprtk_value_t *l = (exprtk_value_t *)list;
    if (!l) return;
    exprtk_list_push(l, exprtk_val_num(val));
}

static void bc_add_list_item_string(Value *list, const char *val) {
    exprtk_value_t *l = (exprtk_value_t *)list;
    if (!l) return;
    size_t vlen = val ? strlen(val) : 0;
    char  *vcopy = val ? db_arena_cstr(&g_build_ctx->env->arena, val, vlen) : NULL;
    tstr_v sv = {vcopy ? vcopy : "", vlen};
    exprtk_list_push(l, exprtk_val_str(sv));
}

static void bc_add_list_item_object(Value *list, Value *obj) {
    exprtk_value_t *l = (exprtk_value_t *)list;
    exprtk_value_t *o = (exprtk_value_t *)obj;
    if (!l || !o) return;
    exprtk_list_push(l, *o);
}

static void bc_set_field_list(Value *obj, const char *name, Value *list) {
    exprtk_value_t *map  = (exprtk_value_t *)obj;
    exprtk_value_t *lval = (exprtk_value_t *)list;
    if (!map || !name || !lval) return;
    char *k = bc_dup(name);
    if (!k) return;
    exprtk_map_set(map, k, *lval);
}

/* ── Set callbacks (exposed same as list) ────────────────────────────────── */

static Value *bc_create_set(void) { return bc_create_list(); }

static void bc_add_set_item_int(Value *set, int32_t val) {
    bc_add_list_item_int(set, val);
}

static void bc_add_set_item_double(Value *set, double val) {
    bc_add_list_item_double(set, val);
}

static void bc_add_set_item_string(Value *set, const char *val) {
    bc_add_list_item_string(set, val);
}

static void bc_set_field_set(Value *obj, const char *name, Value *set) {
    bc_set_field_list(obj, name, set);
}

/* ── Map callbacks ──────────────────────────────────────────────────────── */

static Value *bc_create_map(void) {
    /* A TBE map<K,V> becomes a nested exprtk map */
    return bc_create_object();
}

static void bc_add_map_entry_str_str(Value *map, const char *key, const char *val) {
    exprtk_value_t *m = (exprtk_value_t *)map;
    if (!m || !key) return;
    char *k = bc_dup(key);
    if (!k) return;
    size_t vlen = val ? strlen(val) : 0;
    char  *vcopy = val ? db_arena_cstr(&g_build_ctx->env->arena, val, vlen) : NULL;
    tstr_v sv = {vcopy ? vcopy : "", vlen};
    exprtk_map_set(m, k, exprtk_val_str(sv));
}

static void bc_add_map_entry_str_int(Value *map, const char *key, int32_t val) {
    exprtk_value_t *m = (exprtk_value_t *)map;
    if (!m || !key) return;
    char *k = bc_dup(key);
    if (!k) return;
    exprtk_map_set(m, k, exprtk_val_num((double)val));
}

static void bc_add_map_entry_str_dbl(Value *map, const char *key, double val) {
    exprtk_value_t *m = (exprtk_value_t *)map;
    if (!m || !key) return;
    char *k = bc_dup(key);
    if (!k) return;
    exprtk_map_set(m, k, exprtk_val_num(val));
}

static void bc_set_field_map(Value *obj, const char *name, Value *map) {
    exprtk_value_t *parent = (exprtk_value_t *)obj;
    exprtk_value_t *mval   = (exprtk_value_t *)map;
    if (!parent || !name || !mval) return;
    char *k = bc_dup(name);
    if (!k) return;
    exprtk_map_set(parent, k, *mval);
}

/* ── Shared api descriptor ───────────────────────────────────────────────── */

static const DataBindValueApi g_value_api = {
    .create_object              = bc_create_object,
    .set_field_int              = bc_set_field_int,
    .set_field_int64            = bc_set_field_int64,
    .set_field_double           = bc_set_field_double,
    .set_field_string           = bc_set_field_string,
    .set_field_bytes            = bc_set_field_bytes,
    .create_list                = bc_create_list,
    .add_list_item_int          = bc_add_list_item_int,
    .add_list_item_int64        = bc_add_list_item_int64,
    .add_list_item_double       = bc_add_list_item_double,
    .add_list_item_string       = bc_add_list_item_string,
    .add_list_item_object       = bc_add_list_item_object,
    .set_field_list             = bc_set_field_list,
    .create_set                 = bc_create_set,
    .add_set_item_int           = bc_add_set_item_int,
    .add_set_item_double        = bc_add_set_item_double,
    .add_set_item_string        = bc_add_set_item_string,
    .set_field_set              = bc_set_field_set,
    .create_map                 = bc_create_map,
    .add_map_entry_string_string = bc_add_map_entry_str_str,
    .add_map_entry_string_int   = bc_add_map_entry_str_int,
    .add_map_entry_string_double = bc_add_map_entry_str_dbl,
    .set_field_map              = bc_set_field_map,
};

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

    DataBind *codec = data_bind_create(path, &g_value_api);
    if (!codec) {
        DB_ERROR(ud, "data_bind.create: failed to create codec");
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
 * data_bind.parse(handle, type_name, bytes_vec) -> map | 0
 *
 * bytes_vec is a TurboScript vector where each element encodes one byte
 * (values 0-255).  The function converts it to uint8_t[] on the scratch
 * pool and passes it to the JIT-compiled parser.
 */
static exprtk_value_t fn_db_parse(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    if (argc != 3
     || args[0].type != EXPRTK_VAL_NUMBER
     || args[1].type != EXPRTK_VAL_STRING
     || args[2].type != EXPRTK_VAL_VECTOR) {
        DB_ERROR(ud, "data_bind.parse: expected (number, string, vector)");
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

    /* Convert double[] vector to uint8_t[] byte buffer */
    const exprtk_vector_t *vec = &args[2].data.vector;
    size_t buf_len = vec->size;
    uint8_t *buf = (uint8_t *)mem_alloc(ud->scratch, buf_len + 1);
    if (!buf) { DB_ERROR(ud, "data_bind.parse: OOM"); return DB_ZERO; }
    for (size_t i = 0; i < buf_len; i++)
        buf[i] = (uint8_t)(unsigned int)vec->data[i];

    /* Install build context so the ValueApi callbacks can reach the env */
    db_build_ctx_t bctx = { .env = ud->env, .scratch = ud->scratch };
    g_build_ctx = &bctx;

    Value *result = data_bind_parse(codec, type_name, buf, buf_len);

    g_build_ctx = NULL;

    if (!result) return DB_ZERO;

    /* result is an arena-allocated exprtk_value_t (a map) */
    return *(exprtk_value_t *)result;
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

/**
 * data_bind.error(handle) -> string
 */
static exprtk_value_t fn_db_error(size_t argc, exprtk_value_t *args, void *ud_) {
    db_ud_t *ud = (db_ud_t *)ud_;
    if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER) {
        DB_ERROR(ud, "data_bind.error: expected number handle");
        return DB_ZERO;
    }
    DataBind *codec = db_handle_get(ud->ctx, (int)args[0].data.number);
    if (!codec) return DB_ZERO;

    const char *err = data_bind_get_error(codec);
    if (!err || err[0] == '\0') return DB_ZERO;

    size_t len = strlen(err);
    char  *copy = db_arena_cstr(&ud->env->arena, err, len);
    if (!copy) return DB_ZERO;
    tstr_v sv = {copy, len};
    return exprtk_val_str(sv);
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
    exprtk_env_register_func(env, "data_bind.close",  fn_db_close,  ud);
    exprtk_env_register_func(env, "data_bind.error",  fn_db_error,  ud);
}
