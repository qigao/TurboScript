#include "rules_forge_ctx.h"

#include <stdio.h>

#define RFG_OK RULES_FORGE_OK
#define RFG_INVALID RULES_FORGE_ERROR_INVALID_ARGUMENT

static exprtk_value_t rfg_zero(void) {
    return exprtk_val_num(0.0);
}

static exprtk_value_t rfg_null(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static int rfg_is_int(exprtk_value_t v) {
    return v.type == EXPRTK_VAL_NUMBER || v.type == EXPRTK_VAL_INTEGER;
}

static int rfg_arg_int(exprtk_value_t v, int *out) {
    if (!out || !rfg_is_int(v)) return 0;
    *out = v.type == EXPRTK_VAL_INTEGER ? (int)v.data.integer : (int)v.data.number;
    return 1;
}

static int rfg_arg_i64(exprtk_value_t v, int64_t *out) {
    if (!out || !rfg_is_int(v)) return 0;
    *out = v.type == EXPRTK_VAL_INTEGER ? v.data.integer : (int64_t)v.data.number;
    return 1;
}

static int rfg_arg_string(rfg_ud_t *ud, exprtk_value_t v, char **out) {
    char *buf;
    if (!ud || !out || v.type != EXPRTK_VAL_STRING) return 0;
    if (!ud->scratch) return 0;
    buf = (char *)mem_alloc(ud->scratch, v.data.string.len + 1);
    if (!buf) return 0;
    memcpy(buf, v.data.string.data, v.data.string.len);
    buf[v.data.string.len] = '\0';
    *out = buf;
    return 1;
}

static char *rfg_read_file_text(rfg_ud_t *ud, const char *path) {
    FILE *f;
    long size;
    size_t nread;
    mem_pool_t *pool;
    char *buf;

    if (!ud || !ud->scratch || !path) return NULL;
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
    buf[nread] = '\0';
    return buf;
}

static int rfg_arg_bytes(exprtk_value_t v, const uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return 0;
    if (v.type == EXPRTK_VAL_BYTES) {
        *out = (const uint8_t *)v.data.bytes.data;
        *out_len = v.data.bytes.len;
        return 1;
    }
    if (v.type == EXPRTK_VAL_STRING) {
        *out = (const uint8_t *)v.data.string.data;
        *out_len = v.data.string.len;
        return 1;
    }
    return 0;
}

static exprtk_value_t rfg_string_n(rfg_ud_t *ud, const char *s, size_t len) {
    exprtk_value_t result = rfg_zero();
    if (!ud || !ud->env) return exprtk_val_str(tstr_v_from_buf("", 0));
    if (exprtk_value_copy_to_env(
            exprtk_val_str(tstr_v_from_buf((char *)(s ? s : ""), len)), ud->env,
            &result) != 0)
        return rfg_zero();
    return result;
}

static exprtk_value_t rfg_string(rfg_ud_t *ud, const char *s) {
    return rfg_string_n(ud, s, s ? strlen(s) : 0);
}

static void rfg_set_error(rfg_ctx_t *ctx, const char *msg) {
    if (!ctx) return;
    if (!msg || !msg[0]) msg = ruleforge_get_last_error_message();
    if (!msg) msg = "";
    strncpy(ctx->error_msg, msg, sizeof(ctx->error_msg) - 1);
    ctx->error_msg[sizeof(ctx->error_msg) - 1] = '\0';
}

static ruleforge_status_t rfg_bad_args(rfg_ud_t *ud, const char *msg) {
    if (ud && ud->ctx) rfg_set_error(ud->ctx, msg);
    return RFG_INVALID;
}

static exprtk_value_t rfg_status(ruleforge_status_t status, rfg_ud_t *ud) {
    if (status != RFG_OK && ud && ud->ctx && ud->ctx->error_msg[0] == '\0')
        rfg_set_error(ud->ctx, NULL);
    return exprtk_val_num((double)status);
}

static int rfg_alloc_kb(rfg_ctx_t *ctx, ruleforge_knowledge_base_t kb) {
    int i;
    if (!ctx || !kb) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->kb[i].ptr) {
            ctx->kb[i].ptr = kb;
            return i;
        }
    }
    return -1;
}

static ruleforge_knowledge_base_t rfg_get_kb(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->kb[h].ptr;
}

static int rfg_alloc_session(rfg_ctx_t *ctx, ruleforge_stateful_session_t session, int kb_handle) {
    int i;
    if (!ctx || !session) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->sessions[i].ptr) {
            ctx->sessions[i].ptr = session;
            ctx->sessions[i].kb_handle = kb_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_stateful_session_t rfg_get_session(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->sessions[h].ptr;
}

static int rfg_alloc_stream(rfg_ctx_t *ctx, ruleforge_data_bind_stream_t stream,
                            int session_handle) {
    int i;
    if (!ctx || !stream) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->streams[i].ptr) {
            ctx->streams[i].ptr = stream;
            ctx->streams[i].session_handle = session_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_data_bind_stream_t rfg_get_stream(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->streams[h].ptr;
}

static int rfg_alloc_data_bind_object(rfg_ctx_t *ctx, ruleforge_data_bind_object_t object) {
    int i;
    if (!ctx || !object) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->data_bind_objects[i].ptr) {
            ctx->data_bind_objects[i].ptr = object;
            return i;
        }
    }
    return -1;
}

static ruleforge_data_bind_object_t rfg_get_data_bind_object(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->data_bind_objects[h].ptr;
}

static int rfg_alloc_continuous(rfg_ctx_t *ctx, ruleforge_continuous_session_t session,
                                int kb_handle) {
    int i;
    if (!ctx || !session) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->continuous[i].ptr) {
            ctx->continuous[i].ptr = session;
            ctx->continuous[i].kb_handle = kb_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_continuous_session_t rfg_get_continuous(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->continuous[h].ptr;
}

static int rfg_alloc_continuous_result(rfg_ctx_t *ctx, ruleforge_continuous_result_t result,
                                       int continuous_handle) {
    int i;
    if (!ctx || !result) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->continuous_results[i].ptr) {
            ctx->continuous_results[i].ptr = result;
            ctx->continuous_results[i].continuous_handle = continuous_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_continuous_result_t rfg_get_continuous_result(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->continuous_results[h].ptr;
}

static int rfg_alloc_continuous_stream(rfg_ctx_t *ctx,
                                       ruleforge_continuous_data_bind_stream_t stream,
                                       int continuous_handle) {
    int i;
    if (!ctx || !stream) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->continuous_streams[i].ptr) {
            ctx->continuous_streams[i].ptr = stream;
            ctx->continuous_streams[i].continuous_handle = continuous_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_continuous_data_bind_stream_t rfg_get_continuous_stream(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->continuous_streams[h].ptr;
}

static int rfg_alloc_query(rfg_ctx_t *ctx, ruleforge_query_result_t query, int session_handle) {
    int i;
    if (!ctx || !query) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->queries[i].ptr) {
            ctx->queries[i].ptr = query;
            ctx->queries[i].session_handle = session_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_query_result_t rfg_get_query(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->queries[h].ptr;
}

static int rfg_alloc_fact(rfg_ctx_t *ctx, ruleforge_fact_t fact, int owner_kind, int owner_handle) {
    int i;
    if (!ctx || !fact) return -1;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (!ctx->facts[i].ptr) {
            ctx->facts[i].ptr = fact;
            ctx->facts[i].owner_kind = owner_kind;
            ctx->facts[i].owner_handle = owner_handle;
            return i;
        }
    }
    return -1;
}

static ruleforge_fact_t rfg_get_fact(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES) return NULL;
    return ctx->facts[h].ptr;
}

static void rfg_invalidate_facts(rfg_ctx_t *ctx, int owner_kind, int owner_handle) {
    int i;
    if (!ctx) return;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (ctx->facts[i].ptr && ctx->facts[i].owner_kind == owner_kind &&
            ctx->facts[i].owner_handle == owner_handle) {
            ctx->facts[i].ptr = NULL;
            ctx->facts[i].owner_kind = 0;
            ctx->facts[i].owner_handle = 0;
        }
    }
}

static void rfg_destroy_query_handle(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->queries[h].ptr) return;
    rfg_invalidate_facts(ctx, 2, h);
    ruleforge_query_result_destroy(ctx->queries[h].ptr);
    ctx->queries[h].ptr = NULL;
    ctx->queries[h].session_handle = 0;
}

static void rfg_destroy_stream_handle(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->streams[h].ptr) return;
    ruleforge_data_bind_stream_destroy(ctx->streams[h].ptr);
    ctx->streams[h].ptr = NULL;
    ctx->streams[h].session_handle = 0;
}

static void rfg_destroy_data_bind_object_handle(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->data_bind_objects[h].ptr) return;
    (void)ruleforge_data_bind_object_destroy(ctx->data_bind_objects[h].ptr);
    ctx->data_bind_objects[h].ptr = NULL;
}

static void rfg_destroy_continuous_result_handle(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->continuous_results[h].ptr) return;
    rfg_invalidate_facts(ctx, 3, h);
    ruleforge_continuous_result_destroy(ctx->continuous_results[h].ptr);
    ctx->continuous_results[h].ptr = NULL;
    ctx->continuous_results[h].continuous_handle = 0;
}

static void rfg_destroy_continuous_stream_handle(rfg_ctx_t *ctx, int h) {
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->continuous_streams[h].ptr) return;
    ruleforge_continuous_data_bind_stream_destroy(ctx->continuous_streams[h].ptr);
    ctx->continuous_streams[h].ptr = NULL;
    ctx->continuous_streams[h].continuous_handle = 0;
}

static void rfg_destroy_continuous_handle(rfg_ctx_t *ctx, int h) {
    int i;
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->continuous[h].ptr) return;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (ctx->continuous_streams[i].ptr &&
            ctx->continuous_streams[i].continuous_handle == h)
            rfg_destroy_continuous_stream_handle(ctx, i);
        if (ctx->continuous_results[i].ptr &&
            ctx->continuous_results[i].continuous_handle == h)
            rfg_destroy_continuous_result_handle(ctx, i);
    }
    ruleforge_continuous_session_destroy(ctx->continuous[h].ptr);
    ctx->continuous[h].ptr = NULL;
    ctx->continuous[h].kb_handle = 0;
}

static void rfg_destroy_session_handle(rfg_ctx_t *ctx, int h) {
    int i;
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->sessions[h].ptr) return;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (ctx->streams[i].ptr && ctx->streams[i].session_handle == h)
            rfg_destroy_stream_handle(ctx, i);
        if (ctx->queries[i].ptr && ctx->queries[i].session_handle == h)
            rfg_destroy_query_handle(ctx, i);
    }
    rfg_invalidate_facts(ctx, 1, h);
    ruleforge_session_destroy(ctx->sessions[h].ptr);
    ctx->sessions[h].ptr = NULL;
    ctx->sessions[h].kb_handle = 0;
}

static void rfg_destroy_kb_handle(rfg_ctx_t *ctx, int h) {
    int i;
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->kb[h].ptr) return;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
        if (ctx->continuous[i].ptr && ctx->continuous[i].kb_handle == h)
            rfg_destroy_continuous_handle(ctx, i);
        if (ctx->sessions[i].ptr && ctx->sessions[i].kb_handle == h)
            rfg_destroy_session_handle(ctx, i);
    }
    ruleforge_kb_destroy(ctx->kb[h].ptr);
    ctx->kb[h].ptr = NULL;
}

static exprtk_value_t rfg_status_count(rfg_ud_t *ud, ruleforge_status_t status,
                                       const char *count_name, int count) {
    exprtk_value_t result = exprtk_val_object();
    exprtk_map_set(&result, "status", exprtk_val_int((int64_t)status));
    exprtk_map_set(&result, count_name, exprtk_val_int((int64_t)count));
    if (status != RFG_OK && ud && ud->ctx) rfg_set_error(ud->ctx, NULL);
    return result;
}

static exprtk_value_t rfg_status_facts_loaded(rfg_ud_t *ud, ruleforge_status_t status,
                                              ruleforge_fact_t *facts, int loaded,
                                              int session_handle) {
    exprtk_value_t result = exprtk_val_object();
    exprtk_value_t list = exprtk_val_list_empty();
    int i;
    if (status == RFG_OK && facts && loaded > 0) {
        for (i = 0; i < loaded; i++) {
            int h = rfg_alloc_fact(ud->ctx, facts[i], 1, session_handle);
            exprtk_list_push(&list, exprtk_val_int((int64_t)h));
        }
    }
    exprtk_map_set(&result, "status", exprtk_val_int((int64_t)status));
    exprtk_map_set(&result, "loaded", exprtk_val_int((int64_t)loaded));
    exprtk_map_set(&result, "facts", list);
    if (status != RFG_OK && ud && ud->ctx) rfg_set_error(ud->ctx, NULL);
    return result;
}

void *rfg_ctx_create(void) {
    rfg_ctx_t *ctx = (rfg_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    if (ruleforge_init() == RFG_OK) {
        ctx->initialized = 1;
    } else {
        rfg_set_error(ctx, NULL);
    }
    return ctx;
}

void rfg_ctx_destroy(void *p) {
    rfg_ctx_t *ctx = (rfg_ctx_t *)p;
    int i;
    if (!ctx) return;
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_stream_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_data_bind_object_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_continuous_stream_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_continuous_result_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_query_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_session_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_continuous_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_kb_handle(ctx, i);
    if (ctx->initialized) ruleforge_cleanup();
    free(ctx);
}

static exprtk_value_t fn_version(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    (void)args;
    if (argc != 0) return rfg_zero();
    return rfg_string((rfg_ud_t *)ud_, ruleforge_get_version());
}

static exprtk_value_t fn_error(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    (void)args;
    if (argc != 0 || !ud || !ud->ctx) return rfg_zero();
    return rfg_string(ud, ud->ctx->error_msg);
}

static exprtk_value_t fn_kb_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_knowledge_base_t kb = NULL;
    ruleforge_status_t st;
    int h;
    (void)args;
    if (!ud || argc != 0) return exprtk_val_num(-1.0);
    st = ruleforge_kb_create(&kb);
    if (st != RFG_OK || !kb) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    h = rfg_alloc_kb(ud->ctx, kb);
    if (h < 0) {
        ruleforge_kb_destroy(kb);
        rfg_set_error(ud->ctx, "rules_forge.kb_create: too many knowledge bases");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num((double)h);
}

static exprtk_value_t fn_kb_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_kb(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.kb_destroy: invalid handle"), ud);
    rfg_destroy_kb_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_kb_load(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_knowledge_base_t kb;
    char *source;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &source))
        return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load: expected (kb, rules_text)"), ud);
    kb = rfg_get_kb(ud->ctx, h);
    if (!kb) return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load: invalid kb handle"), ud);
    return rfg_status(ruleforge_kb_load_drl(kb, source), ud);
}

static exprtk_value_t fn_kb_load_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_knowledge_base_t kb;
    char *path;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &path))
        return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load_file: expected (kb, path)"), ud);
    kb = rfg_get_kb(ud->ctx, h);
    if (!kb) return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load_file: invalid kb handle"), ud);
    return rfg_status(ruleforge_kb_load_drl_file(kb, path, NULL, 0), ud);
}

static exprtk_value_t fn_kb_load_decision_table_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_knowledge_base_t kb;
    char *csv;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &csv))
        return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load_decision_table_csv: expected (kb, csv)"), ud);
    kb = rfg_get_kb(ud->ctx, h);
    if (!kb) return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load_decision_table_csv: invalid kb handle"), ud);
    return rfg_status(ruleforge_kb_load_decision_table_csv(kb, csv), ud);
}

static exprtk_value_t fn_session_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session = NULL;
    ruleforge_knowledge_base_t kb;
    ruleforge_status_t st;
    int kb_h, h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &kb_h)) return exprtk_val_num(-1.0);
    kb = rfg_get_kb(ud->ctx, kb_h);
    if (!kb) {
        rfg_bad_args(ud, "rules_forge.session_create: invalid kb handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_create(kb, &session);
    if (st != RFG_OK || !session) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    h = rfg_alloc_session(ud->ctx, session, kb_h);
    if (h < 0) {
        ruleforge_session_destroy(session);
        rfg_set_error(ud->ctx, "rules_forge.session_create: too many sessions");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num((double)h);
}

static exprtk_value_t fn_session_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_session(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_destroy: invalid handle"), ud);
    rfg_destroy_session_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_session_reset(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_status_t st;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_reset: expected session"), ud);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.session_reset: invalid session handle"), ud);
    st = ruleforge_session_reset(session);
    if (st == RFG_OK) rfg_invalidate_facts(ud->ctx, 1, h);
    return rfg_status(st, ud);
}

static exprtk_value_t fn_session_fire_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h, max_rules = -1, fired = 0;
    if (!ud || (argc != 1 && argc != 2) || !rfg_arg_int(args[0], &h) ||
        (argc == 2 && !rfg_arg_int(args[1], &max_rules)))
        return rfg_status_count(ud, rfg_bad_args(ud, "rules_forge.session_fire_all: expected (session, max_rules?)"), "fired", 0);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status_count(ud, rfg_bad_args(ud, "rules_forge.session_fire_all: invalid session handle"), "fired", 0);
    return rfg_status_count(ud, ruleforge_session_fire_all_rules(session, max_rules, &fired), "fired", fired);
}

static exprtk_value_t fn_session_fact_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_num(-1.0);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return exprtk_val_num(-1.0);
    return exprtk_val_int((int64_t)ruleforge_session_get_fact_count(session));
}

static exprtk_value_t fn_session_set_validation_mode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h, mode;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_int(args[1], &mode))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_set_validation_mode: expected (session, mode)"), ud);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.session_set_validation_mode: invalid session handle"), ud);
    return rfg_status(ruleforge_session_set_validation_mode(session, (ruleforge_validation_mode_t)mode), ud);
}

typedef ruleforge_status_t (*rfg_data_bind_text_parser_t)(
    const char *, const char *, const char *, size_t, ruleforge_data_bind_object_t *);
typedef ruleforge_status_t (*rfg_data_bind_text_serializer_t)(
    ruleforge_data_bind_object_t, char **, size_t *);

static exprtk_value_t rfg_publish_data_bind_object(rfg_ud_t *ud,
                                                   ruleforge_status_t status,
                                                   ruleforge_data_bind_object_t object,
                                                   const char *operation) {
    int h;
    if (status != RFG_OK || !object) {
        if (object) (void)ruleforge_data_bind_object_destroy(object);
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    h = rfg_alloc_data_bind_object(ud->ctx, object);
    if (h < 0) {
        (void)ruleforge_data_bind_object_destroy(object);
        rfg_set_error(ud->ctx, operation);
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_int(h);
}

static exprtk_value_t rfg_data_bind_object_from_text(
    size_t argc, exprtk_value_t *args, void *ud_, const char *error_text,
    rfg_data_bind_text_parser_t parse) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object = NULL;
    char *schema_path;
    char *fact_type;
    ruleforge_status_t status;
    if (!ud || argc != 3 || !rfg_arg_string(ud, args[0], &schema_path) ||
        !rfg_arg_string(ud, args[1], &fact_type) || args[2].type != EXPRTK_VAL_STRING) {
        rfg_bad_args(ud, error_text);
        return exprtk_val_num(-1.0);
    }
    status = parse(schema_path, fact_type, args[2].data.string.data,
                   args[2].data.string.len, &object);
    return rfg_publish_data_bind_object(
        ud, status, object, "rules_forge.data_bind_object: too many object handles");
}

#define RFG_DATA_BIND_OBJECT_TEXT_FN(fn_name, parse_fn, error_text)                   \
    static exprtk_value_t fn_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud) {      \
        return rfg_data_bind_object_from_text(argc, args, ud, error_text, parse_fn);  \
    }

RFG_DATA_BIND_OBJECT_TEXT_FN(fn_data_bind_object_from_json,
    ruleforge_data_bind_object_from_json,
    "rules_forge.data_bind_object_from_json: expected (schema_path, type, json)")
RFG_DATA_BIND_OBJECT_TEXT_FN(fn_data_bind_object_from_yaml,
    ruleforge_data_bind_object_from_yaml,
    "rules_forge.data_bind_object_from_yaml: expected (schema_path, type, yaml)")
RFG_DATA_BIND_OBJECT_TEXT_FN(fn_data_bind_object_from_xml,
    ruleforge_data_bind_object_from_xml,
    "rules_forge.data_bind_object_from_xml: expected (schema_path, type, xml)")

static exprtk_value_t fn_data_bind_object_from_binary(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object = NULL;
    const uint8_t *data;
    size_t len;
    char *schema_path;
    char *fact_type;
    ruleforge_status_t status;
    if (!ud || argc != 3 || !rfg_arg_string(ud, args[0], &schema_path) ||
        !rfg_arg_string(ud, args[1], &fact_type) || !rfg_arg_bytes(args[2], &data, &len)) {
        rfg_bad_args(ud,
            "rules_forge.data_bind_object_from_binary: expected (schema_path, type, bytes)");
        return exprtk_val_num(-1.0);
    }
    status = ruleforge_data_bind_object_from_binary(schema_path, fact_type, data, len, &object);
    return rfg_publish_data_bind_object(
        ud, status, object, "rules_forge.data_bind_object: too many object handles");
}

static exprtk_value_t fn_data_bind_object_from_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object = NULL;
    char *schema_path;
    char *fact_type;
    int64_t row;
    ruleforge_status_t status;
    if (!ud || argc != 4 || !rfg_arg_string(ud, args[0], &schema_path) ||
        !rfg_arg_string(ud, args[1], &fact_type) || args[2].type != EXPRTK_VAL_STRING ||
        !rfg_arg_i64(args[3], &row) || row < 0) {
        rfg_bad_args(ud,
            "rules_forge.data_bind_object_from_csv: expected (schema_path, type, csv, non-negative row)");
        return exprtk_val_num(-1.0);
    }
    status = ruleforge_data_bind_object_from_csv(
        schema_path, fact_type, args[2].data.string.data, args[2].data.string.len,
        (size_t)row, &object);
    return rfg_publish_data_bind_object(
        ud, status, object, "rules_forge.data_bind_object: too many object handles");
}

static exprtk_value_t fn_data_bind_object_clone(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object;
    ruleforge_data_bind_object_t clone = NULL;
    ruleforge_status_t status;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) ||
        !(object = rfg_get_data_bind_object(ud->ctx, h))) {
        rfg_bad_args(ud, "rules_forge.data_bind_object_clone: expected valid object handle");
        return exprtk_val_num(-1.0);
    }
    status = ruleforge_data_bind_object_clone(object, &clone);
    return rfg_publish_data_bind_object(
        ud, status, clone,
        "rules_forge.data_bind_object_clone: too many object handles");
}

static exprtk_value_t fn_data_bind_object_type(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) ||
        !(object = rfg_get_data_bind_object(ud->ctx, h))) {
        rfg_bad_args(ud, "rules_forge.data_bind_object_type: expected valid object handle");
        return rfg_string(ud, "");
    }
    return rfg_string(ud, ruleforge_data_bind_object_get_type_name(object));
}

static exprtk_value_t rfg_data_bind_object_serialize_text(
    size_t argc, exprtk_value_t *args, void *ud_, const char *error_text,
    rfg_data_bind_text_serializer_t serialize) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object;
    char *serialized = NULL;
    size_t len = 0;
    ruleforge_status_t status;
    exprtk_value_t value;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) ||
        !(object = rfg_get_data_bind_object(ud->ctx, h))) {
        rfg_bad_args(ud, error_text);
        return rfg_string(ud, "");
    }
    status = serialize(object, &serialized, &len);
    if (status != RFG_OK || !serialized) {
        if (serialized) ruleforge_data_bind_serialized_free(serialized);
        rfg_set_error(ud->ctx, NULL);
        return rfg_string(ud, "");
    }
    value = rfg_string_n(ud, serialized, len);
    ruleforge_data_bind_serialized_free(serialized);
    return value;
}

#define RFG_DATA_BIND_OBJECT_SERIALIZE_TEXT_FN(fn_name, serialize_fn, error_text)     \
    static exprtk_value_t fn_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud) {      \
        return rfg_data_bind_object_serialize_text(                                  \
            argc, args, ud, error_text, serialize_fn);                               \
    }

RFG_DATA_BIND_OBJECT_SERIALIZE_TEXT_FN(fn_data_bind_object_serialize_json,
    ruleforge_data_bind_object_serialize_json,
    "rules_forge.data_bind_object_serialize_json: expected valid object handle")
RFG_DATA_BIND_OBJECT_SERIALIZE_TEXT_FN(fn_data_bind_object_serialize_yaml,
    ruleforge_data_bind_object_serialize_yaml,
    "rules_forge.data_bind_object_serialize_yaml: expected valid object handle")
RFG_DATA_BIND_OBJECT_SERIALIZE_TEXT_FN(fn_data_bind_object_serialize_xml,
    ruleforge_data_bind_object_serialize_xml,
    "rules_forge.data_bind_object_serialize_xml: expected valid object handle")
RFG_DATA_BIND_OBJECT_SERIALIZE_TEXT_FN(fn_data_bind_object_serialize_csv,
    ruleforge_data_bind_object_serialize_csv,
    "rules_forge.data_bind_object_serialize_csv: expected valid object handle")

static exprtk_value_t fn_data_bind_object_serialize_binary(size_t argc,
                                                            exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_object_t object;
    uint8_t *serialized = NULL;
    exprtk_value_t result = rfg_zero();
    size_t len = 0;
    ruleforge_status_t status;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) ||
        !(object = rfg_get_data_bind_object(ud->ctx, h))) {
        rfg_bad_args(ud,
            "rules_forge.data_bind_object_serialize_binary: expected valid object handle");
        return exprtk_val_bytes(tstr_v_from_buf("", 0));
    }
    status = ruleforge_data_bind_object_serialize_binary(object, &serialized, &len);
    if (status != RFG_OK || (!serialized && len != 0)) {
        if (serialized) ruleforge_data_bind_binary_free(serialized);
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_bytes(tstr_v_from_buf("", 0));
    }
    if (exprtk_value_copy_to_env(
            exprtk_val_bytes(tstr_v_from_buf((char *)serialized, len)), ud->env,
            &result) != 0) {
        ruleforge_data_bind_binary_free(serialized);
        rfg_set_error(ud->ctx, "rules_forge.data_bind_object_serialize_binary: OOM");
        return exprtk_val_bytes(tstr_v_from_buf("", 0));
    }
    ruleforge_data_bind_binary_free(serialized);
    return result;
}

static exprtk_value_t fn_data_bind_object_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) ||
        !rfg_get_data_bind_object(ud->ctx, h))
        return rfg_status(rfg_bad_args(
            ud, "rules_forge.data_bind_object_destroy: invalid object handle"), ud);
    rfg_destroy_data_bind_object_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_session_add_fact_json(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *type, *json;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &json)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json: expected (session, type, json)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_json(session, type, json, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_fact_json_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *type, *json, *json_path;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &json) ||
        !rfg_arg_string(ud, args[3], &json_path)) {
        rfg_bad_args(ud,
            "rules_forge.session_add_fact_json_path: expected (session, type, json, json_path)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json_path: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_json_path(session, type, json, json_path, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    if (fh < 0) rfg_set_error(ud->ctx, "rules_forge.session_add_fact_json_path: too many fact handles");
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_facts_json_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *json, *json_path;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &json) ||
        !rfg_arg_string(ud, args[3], &json_path))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_json_path: expected (session, type, json, json_path)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_json_path: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_json_path(session, type, json, json_path, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_fact_yaml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *type, *yaml;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &yaml)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_yaml: expected (session, type, yaml)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_yaml: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_yaml(session, type, yaml, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_fact_yaml_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *type, *yaml, *yaml_path;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &yaml) ||
        !rfg_arg_string(ud, args[3], &yaml_path)) {
        rfg_bad_args(ud,
            "rules_forge.session_add_fact_yaml_path: expected (session, type, yaml, yaml_path)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_yaml_path: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_yaml_path(session, type, yaml, yaml_path, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    if (fh < 0)
        rfg_set_error(ud->ctx, "rules_forge.session_add_fact_yaml_path: too many fact handles");
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_facts_yaml_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *yaml, *yaml_path;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &yaml) ||
        !rfg_arg_string(ud, args[3], &yaml_path))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_yaml_path: expected (session, type, yaml, yaml_path)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_yaml_path: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_yaml_path(session, type, yaml, yaml_path, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_fact_json_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *type, *path, *json;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &path)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json_file: expected (session, type, path)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json_file: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    json = rfg_read_file_text(ud, path);
    if (!json) {
        rfg_set_error(ud->ctx, "rules_forge.session_add_fact_json_file: failed to read path");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_json(session, type, json, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_fact_binary(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    const uint8_t *bytes;
    size_t bytes_len;
    char *type;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_bytes(args[2], &bytes, &bytes_len)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_binary: expected (session, type, bytes)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_binary: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_binary(session, type, bytes, bytes_len, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_facts_csv(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *csv;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &csv))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_csv: expected (session, type, csv)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_csv(session, type, csv, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_facts_csv_path(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *csv, *csv_path;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &csv) ||
        !rfg_arg_string(ud, args[3], &csv_path))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_csv_path: expected (session, type, csv, csv_path)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv_path: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_csv_path(session, type, csv, csv_path, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_facts_csv_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *path, *csv;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &path))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_csv_file: expected (session, type, path)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv_file: invalid session handle"), NULL, 0, h);
    csv = rfg_read_file_text(ud, path);
    if (!csv) {
        rfg_set_error(ud->ctx, "rules_forge.session_add_facts_csv_file: failed to read path");
        return rfg_status_facts_loaded(ud, RFG_INVALID, NULL, 0, h);
    }
    st = ruleforge_session_add_facts_csv(session, type, csv, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_facts_xml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *xml, *xpath;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &xml) ||
        !rfg_arg_string(ud, args[3], &xpath))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_xml: expected (session, type, xml, xpath)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_xml: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_xml(session, type, xml, xpath, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_facts_xml_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *path, *xml, *xpath;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &path) ||
        !rfg_arg_string(ud, args[3], &xpath))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud,
            "rules_forge.session_add_facts_xml_file: expected (session, type, path, xpath)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_xml_file: invalid session handle"), NULL, 0, h);
    xml = rfg_read_file_text(ud, path);
    if (!xml) {
        rfg_set_error(ud->ctx, "rules_forge.session_add_facts_xml_file: failed to read path");
        return rfg_status_facts_loaded(ud, RFG_INVALID, NULL, 0, h);
    }
    st = ruleforge_session_add_facts_xml(session, type, xml, xpath, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_data_bind_object(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_data_bind_object_t object;
    ruleforge_fact_t fact = NULL;
    ruleforge_status_t status;
    int session_h;
    int object_h;
    int fact_h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &session_h) ||
        !rfg_arg_int(args[1], &object_h)) {
        rfg_bad_args(ud,
            "rules_forge.session_add_data_bind_object: expected (session, object)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, session_h);
    object = rfg_get_data_bind_object(ud->ctx, object_h);
    if (!session || !object) {
        rfg_bad_args(ud,
            "rules_forge.session_add_data_bind_object: invalid session or object handle");
        return exprtk_val_num(-1.0);
    }
    status = ruleforge_session_add_data_bind_object(session, object, &fact);
    if (status != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fact_h = rfg_alloc_fact(ud->ctx, fact, 1, session_h);
    if (fact_h < 0)
        rfg_set_error(ud->ctx,
            "rules_forge.session_add_data_bind_object: too many fact handles");
    return exprtk_val_int(fact_h);
}

typedef enum {
    RFG_STREAM_JSON,
    RFG_STREAM_JSON_ALL,
    RFG_STREAM_JSON_PATH,
    RFG_STREAM_JSON_PATH_ALL,
    RFG_STREAM_YAML,
    RFG_STREAM_YAML_ALL,
    RFG_STREAM_YAML_PATH,
    RFG_STREAM_YAML_PATH_ALL,
    RFG_STREAM_CSV_ALL,
    RFG_STREAM_CSV_PATH,
    RFG_STREAM_XML,
    RFG_STREAM_XML_PATH_ALL
} rfg_stream_kind_t;

static exprtk_value_t rfg_stream_create(size_t argc, exprtk_value_t *args, void *ud_,
                                        rfg_stream_kind_t kind, const char *name) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_data_bind_stream_t stream = NULL;
    ruleforge_status_t st;
    char *type, *path = NULL;
    int session_h, stream_h;
    int needs_path = kind == RFG_STREAM_JSON_PATH || kind == RFG_STREAM_JSON_PATH_ALL ||
                     kind == RFG_STREAM_YAML_PATH || kind == RFG_STREAM_YAML_PATH_ALL ||
                     kind == RFG_STREAM_CSV_PATH || kind == RFG_STREAM_XML_PATH_ALL;

    if (!ud || argc != (size_t)(needs_path ? 3 : 2) ||
        !rfg_arg_int(args[0], &session_h) || !rfg_arg_string(ud, args[1], &type) ||
        (needs_path && !rfg_arg_string(ud, args[2], &path))) {
        rfg_bad_args(ud, name);
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, session_h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.stream_create: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    switch (kind) {
    case RFG_STREAM_JSON:
        st = ruleforge_data_bind_stream_json_create(session, type, &stream); break;
    case RFG_STREAM_JSON_ALL:
        st = ruleforge_data_bind_stream_json_all_create(session, type, &stream); break;
    case RFG_STREAM_JSON_PATH:
        st = ruleforge_data_bind_stream_json_path_create(session, type, path, &stream); break;
    case RFG_STREAM_JSON_PATH_ALL:
        st = ruleforge_data_bind_stream_json_path_all_create(session, type, path, &stream); break;
    case RFG_STREAM_YAML:
        st = ruleforge_data_bind_stream_yaml_create(session, type, &stream); break;
    case RFG_STREAM_YAML_ALL:
        st = ruleforge_data_bind_stream_yaml_all_create(session, type, &stream); break;
    case RFG_STREAM_YAML_PATH:
        st = ruleforge_data_bind_stream_yaml_path_create(session, type, path, &stream); break;
    case RFG_STREAM_YAML_PATH_ALL:
        st = ruleforge_data_bind_stream_yaml_path_all_create(session, type, path, &stream); break;
    case RFG_STREAM_CSV_ALL:
        st = ruleforge_data_bind_stream_csv_all_create(session, type, &stream); break;
    case RFG_STREAM_CSV_PATH:
        st = ruleforge_data_bind_stream_csv_path_create(session, type, path, &stream); break;
    case RFG_STREAM_XML:
        st = ruleforge_data_bind_stream_xml_create(session, type, &stream); break;
    default:
        st = ruleforge_data_bind_stream_xml_path_all_create(session, type, path, &stream); break;
    }
    if (st != RFG_OK || !stream) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    stream_h = rfg_alloc_stream(ud->ctx, stream, session_h);
    if (stream_h < 0) {
        ruleforge_data_bind_stream_destroy(stream);
        rfg_set_error(ud->ctx, "rules_forge.stream_create: too many stream handles");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_int(stream_h);
}

#define RFG_STREAM_CREATE_FN(fn_name, kind_value, error_text)                         \
    static exprtk_value_t fn_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud) {       \
        return rfg_stream_create(argc, args, ud, kind_value, error_text);              \
    }

RFG_STREAM_CREATE_FN(fn_stream_json_create, RFG_STREAM_JSON,
                     "rules_forge.stream_json_create: expected (session, type)")
RFG_STREAM_CREATE_FN(fn_stream_json_all_create, RFG_STREAM_JSON_ALL,
                     "rules_forge.stream_json_all_create: expected (session, type)")
RFG_STREAM_CREATE_FN(fn_stream_json_path_create, RFG_STREAM_JSON_PATH,
                     "rules_forge.stream_json_path_create: expected (session, type, json_path)")
RFG_STREAM_CREATE_FN(fn_stream_json_path_all_create, RFG_STREAM_JSON_PATH_ALL,
                     "rules_forge.stream_json_path_all_create: expected (session, type, json_path)")
RFG_STREAM_CREATE_FN(fn_stream_yaml_create, RFG_STREAM_YAML,
                     "rules_forge.stream_yaml_create: expected (session, type)")
RFG_STREAM_CREATE_FN(fn_stream_yaml_all_create, RFG_STREAM_YAML_ALL,
                     "rules_forge.stream_yaml_all_create: expected (session, type)")
RFG_STREAM_CREATE_FN(fn_stream_yaml_path_create, RFG_STREAM_YAML_PATH,
                     "rules_forge.stream_yaml_path_create: expected (session, type, yaml_path)")
RFG_STREAM_CREATE_FN(fn_stream_yaml_path_all_create, RFG_STREAM_YAML_PATH_ALL,
                     "rules_forge.stream_yaml_path_all_create: expected (session, type, yaml_path)")
RFG_STREAM_CREATE_FN(fn_stream_csv_all_create, RFG_STREAM_CSV_ALL,
                     "rules_forge.stream_csv_all_create: expected (session, type)")
RFG_STREAM_CREATE_FN(fn_stream_csv_path_create, RFG_STREAM_CSV_PATH,
                     "rules_forge.stream_csv_path_create: expected (session, type, csv_path)")
RFG_STREAM_CREATE_FN(fn_stream_xml_create, RFG_STREAM_XML,
                     "rules_forge.stream_xml_create: expected (session, type)")
RFG_STREAM_CREATE_FN(fn_stream_xml_path_all_create, RFG_STREAM_XML_PATH_ALL,
                     "rules_forge.stream_xml_path_all_create: expected (session, type, xml_path)")

static exprtk_value_t fn_stream_feed(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_stream_t stream;
    const uint8_t *data;
    size_t len;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_bytes(args[1], &data, &len))
        return rfg_status(rfg_bad_args(ud, "rules_forge.stream_feed: expected (stream, data)"), ud);
    stream = rfg_get_stream(ud->ctx, h);
    if (!stream) return rfg_status(rfg_bad_args(ud, "rules_forge.stream_feed: invalid stream handle"), ud);
    return rfg_status(ruleforge_data_bind_stream_feed(stream, data, len), ud);
}

static exprtk_value_t fn_stream_feed_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_stream_t stream;
    char *path;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &path))
        return rfg_status(rfg_bad_args(ud, "rules_forge.stream_feed_file: expected (stream, path)"), ud);
    stream = rfg_get_stream(ud->ctx, h);
    if (!stream) return rfg_status(rfg_bad_args(ud, "rules_forge.stream_feed_file: invalid stream handle"), ud);
    return rfg_status(ruleforge_data_bind_stream_feed_file(stream, path), ud);
}

static exprtk_value_t fn_stream_finish(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_data_bind_stream_t stream;
    ruleforge_fact_t *facts = NULL;
    ruleforge_status_t st;
    int h, loaded = 0, session_h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.stream_finish: expected stream"), NULL, 0, -1);
    stream = rfg_get_stream(ud->ctx, h);
    if (!stream) return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.stream_finish: invalid stream handle"), NULL, 0, -1);
    session_h = ud->ctx->streams[h].session_handle;
    st = ruleforge_data_bind_stream_finish(stream, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, session_h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_stream_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_stream(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.stream_destroy: invalid stream handle"), ud);
    rfg_destroy_stream_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_session_query(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_query_result_t query = NULL;
    char *name;
    int h, qh;
    ruleforge_status_t st;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &name))
        return exprtk_val_num(-1.0);
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_query: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_query(session, name, &query);
    if (st != RFG_OK || !query) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    qh = rfg_alloc_query(ud->ctx, query, h);
    if (qh < 0) {
        ruleforge_query_result_destroy(query);
        rfg_set_error(ud->ctx, "rules_forge.session_query: too many query handles");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_num((double)qh);
}

static exprtk_value_t fn_query_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_query_result_t query;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_num(-1.0);
    query = rfg_get_query(ud->ctx, h);
    if (!query) return exprtk_val_num(-1.0);
    return exprtk_val_int((int64_t)ruleforge_query_result_get_size(query));
}

static exprtk_value_t fn_query_fact(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_query_result_t query;
    ruleforge_fact_t fact = NULL;
    char *binding;
    int h, row, fh;
    ruleforge_status_t st;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) || !rfg_arg_int(args[1], &row) ||
        !rfg_arg_string(ud, args[2], &binding)) {
        rfg_bad_args(ud, "rules_forge.query_fact: expected (query, row, binding)");
        return exprtk_val_num(-1.0);
    }
    query = rfg_get_query(ud->ctx, h);
    if (!query) {
        rfg_bad_args(ud, "rules_forge.query_fact: invalid query handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_query_result_get_fact_at_index(query, row, binding, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 2, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_query_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_query(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.query_destroy: invalid query handle"), ud);
    rfg_destroy_query_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_fact_string(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_fact_t fact;
    char *field;
    char stack_buf[256];
    char *buf = stack_buf;
    size_t needed = 0;
    int h;
    ruleforge_status_t st;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &field))
        return rfg_string(ud, "");
    fact = rfg_get_fact(ud->ctx, h);
    if (!fact) return rfg_string(ud, "");
    st = ruleforge_fact_get_field_as_string(fact, field, stack_buf, sizeof(stack_buf), &needed);
    if (st == RFG_INVALID && needed >= sizeof(stack_buf)) {
        buf = (char *)mem_alloc(ud->scratch, needed + 1);
        if (!buf) return rfg_zero();
        st = ruleforge_fact_get_field_as_string(fact, field, buf, needed + 1, &needed);
    }
    if (st != RFG_OK) {
        rfg_set_error(ud->ctx, NULL);
        return rfg_string(ud, "");
    }
    return rfg_string(ud, buf);
}

static exprtk_value_t fn_fact_double(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_fact_t fact;
    char *field;
    double value = 0.0;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &field))
        return rfg_zero();
    fact = rfg_get_fact(ud->ctx, h);
    if (!fact || ruleforge_fact_get_field_as_double(fact, field, &value) != RFG_OK) {
        rfg_set_error(ud ? ud->ctx : NULL, NULL);
        return rfg_zero();
    }
    return exprtk_val_num(value);
}

static exprtk_value_t fn_fact_int(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_fact_t fact;
    char *field;
    int64_t value = 0;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &field))
        return rfg_zero();
    fact = rfg_get_fact(ud->ctx, h);
    if (!fact || ruleforge_fact_get_field_as_int(fact, field, &value) != RFG_OK) {
        rfg_set_error(ud ? ud->ctx : NULL, NULL);
        return rfg_zero();
    }
    return exprtk_val_int(value);
}

static exprtk_value_t fn_fact_bool(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_fact_t fact;
    char *field;
    int value = 0;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &field))
        return rfg_zero();
    fact = rfg_get_fact(ud->ctx, h);
    if (!fact || ruleforge_fact_get_field_as_bool(fact, field, &value) != RFG_OK) {
        rfg_set_error(ud ? ud->ctx : NULL, NULL);
        return rfg_zero();
    }
    return exprtk_val_bool(value != 0);
}

static exprtk_value_t fn_session_enable_tracing(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h, enabled;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_int(args[1], &enabled))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_enable_tracing: expected (session, enabled)"), ud);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.session_enable_tracing: invalid session handle"), ud);
    return rfg_status(ruleforge_session_enable_tracing(session, enabled), ud);
}

static exprtk_value_t rfg_session_string_call(rfg_ud_t *ud, ruleforge_stateful_session_t session,
                                              int include_network, int kind) {
    char stack_buf[4096];
    char *buf = stack_buf;
    size_t actual = 0;
    ruleforge_status_t st;
    if (kind == 0)
        st = ruleforge_session_get_execution_trace(session, include_network, buf, sizeof(stack_buf), &actual);
    else if (kind == 1)
        st = ruleforge_session_get_rule_performance_summary(session, buf, sizeof(stack_buf), &actual);
    else
        st = ruleforge_session_get_memory_stats(session, buf, sizeof(stack_buf), &actual);
    if (st == RFG_INVALID && actual >= sizeof(stack_buf)) {
        buf = (char *)mem_alloc(ud->scratch, actual + 1);
        if (!buf) return rfg_zero();
        if (kind == 0)
            st = ruleforge_session_get_execution_trace(session, include_network, buf, actual + 1, &actual);
        else if (kind == 1)
            st = ruleforge_session_get_rule_performance_summary(session, buf, actual + 1, &actual);
        else
            st = ruleforge_session_get_memory_stats(session, buf, actual + 1, &actual);
    }
    if (st != RFG_OK) {
        rfg_set_error(ud->ctx, NULL);
        return rfg_string(ud, "");
    }
    return rfg_string(ud, buf);
}

static exprtk_value_t fn_session_trace(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h, include_network = 0;
    if (!ud || (argc != 1 && argc != 2) || !rfg_arg_int(args[0], &h) ||
        (argc == 2 && !rfg_arg_int(args[1], &include_network)))
        return rfg_string(ud, "");
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_string(ud, "");
    return rfg_session_string_call(ud, session, include_network, 0);
}

static exprtk_value_t fn_session_rule_performance(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return rfg_string(ud, "");
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_string(ud, "");
    return rfg_session_string_call(ud, session, 0, 1);
}

static exprtk_value_t fn_session_clear_trace(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_clear_trace: expected session"), ud);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.session_clear_trace: invalid session handle"), ud);
    return rfg_status(ruleforge_session_clear_trace(session), ud);
}

static exprtk_value_t fn_session_memory_used(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_int(-1);
    session = rfg_get_session(ud->ctx, h);
    return exprtk_val_int(session ? ruleforge_session_get_memory_used(session) : -1);
}

static exprtk_value_t fn_session_memory_peak(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_int(-1);
    session = rfg_get_session(ud->ctx, h);
    return exprtk_val_int(session ? ruleforge_session_get_memory_peak(session) : -1);
}

static exprtk_value_t fn_session_memory_stats(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return rfg_string(ud, "");
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_string(ud, "");
    return rfg_session_string_call(ud, session, 0, 2);
}

static int rfg_config_size(exprtk_value_t config, const char *key, size_t *field) {
    exprtk_value_t value;
    int64_t parsed;
    if (!exprtk_map_has(&config, key)) return 1;
    value = exprtk_map_get(&config, key);
    if (!rfg_arg_i64(value, &parsed) || parsed < 0) return 0;
    *field = (size_t)parsed;
    return 1;
}

static int rfg_config_i64(exprtk_value_t config, const char *key, int64_t *field) {
    exprtk_value_t value;
    if (!exprtk_map_has(&config, key)) return 1;
    value = exprtk_map_get(&config, key);
    return rfg_arg_i64(value, field);
}

static int rfg_config_int(exprtk_value_t config, const char *key, int *field) {
    exprtk_value_t value;
    if (!exprtk_map_has(&config, key)) return 1;
    value = exprtk_map_get(&config, key);
    return rfg_arg_int(value, field);
}

static int rfg_continuous_config_from_value(rfg_ud_t *ud, exprtk_value_t value,
                                            ruleforge_continuous_config_t *config) {
    exprtk_value_t types;
    const char **type_names;
    size_t i;
    if (!ud || !config || ruleforge_continuous_config_init(config) != RFG_OK) return 0;
    if (value.type == EXPRTK_VAL_NULL) return 1;
    if (!exprtk_value_is_object_like(&value)) return 0;
    if (!rfg_config_size(value, "max_active_events", &config->max_active_events) ||
        !rfg_config_size(value, "max_dedup_entries", &config->max_dedup_entries) ||
        !rfg_config_size(value, "max_pending_result_batches", &config->max_pending_result_batches) ||
        !rfg_config_size(value, "max_pending_results", &config->max_pending_results) ||
        !rfg_config_size(value, "max_input_batch_size", &config->max_input_batch_size) ||
        !rfg_config_size(value, "max_replay_steps", &config->max_replay_steps) ||
        !rfg_config_int(value, "max_rules_per_step", &config->max_rules_per_step) ||
        !rfg_config_i64(value, "allowed_lateness_ms", &config->allowed_lateness_ms) ||
        !rfg_config_i64(value, "event_retention_ms", &config->event_retention_ms) ||
        !rfg_config_i64(value, "dedup_retention_ms", &config->dedup_retention_ms) ||
        !rfg_config_i64(value, "max_event_time_lead_ms", &config->max_event_time_lead_ms))
        return 0;
    if (!exprtk_map_has(&value, "output_fact_types")) return 1;
    types = exprtk_map_get(&value, "output_fact_types");
    if (types.type != EXPRTK_VAL_LIST && types.type != EXPRTK_VAL_SET) return 0;
    if (types.data.list.count == 0) {
        config->output_fact_types = NULL;
        config->output_fact_type_count = 0;
        return 1;
    }
    type_names = (const char **)mem_alloc(ud->scratch, types.data.list.count * sizeof(*type_names));
    if (!type_names) return 0;
    for (i = 0; i < types.data.list.count; i++) {
        exprtk_value_t item = exprtk_list_get(&types, i);
        char *name;
        if (!rfg_arg_string(ud, item, &name)) return 0;
        type_names[i] = name;
    }
    config->output_fact_types = type_names;
    config->output_fact_type_count = types.data.list.count;
    return 1;
}

static exprtk_value_t rfg_continuous_result_value(rfg_ud_t *ud, ruleforge_status_t st,
                                                  ruleforge_continuous_result_t result,
                                                  int continuous_h) {
    exprtk_value_t value = exprtk_val_object();
    int result_h = -1;
    int64_t watermark = 0;
    int has_watermark = 0;
    if (!ud || !ud->ctx) {
        if (result) ruleforge_continuous_result_destroy(result);
        exprtk_map_set(&value, "status", exprtk_val_int(RFG_INVALID));
        exprtk_map_set(&value, "result", exprtk_val_int(-1));
        return value;
    }
    if (st == RFG_OK && result) {
        result_h = rfg_alloc_continuous_result(ud->ctx, result, continuous_h);
        if (result_h < 0) {
            ruleforge_continuous_result_destroy(result);
            st = RULES_FORGE_ERROR_RESOURCE_LIMIT;
            rfg_set_error(ud->ctx, "rules_forge.continuous: too many result handles");
            result = NULL;
        }
    }
    exprtk_map_set(&value, "status", exprtk_val_int(st));
    exprtk_map_set(&value, "result", exprtk_val_int(result_h));
    exprtk_map_set(&value, "step_status", exprtk_val_int(result ? ruleforge_continuous_result_get_status(result) : -1));
    exprtk_map_set(&value, "batch_id", exprtk_val_int(result ? (int64_t)ruleforge_continuous_result_get_batch_id(result) : 0));
    exprtk_map_set(&value, "rules_fired", exprtk_val_int(result ? ruleforge_continuous_result_get_rules_fired(result) : 0));
    exprtk_map_set(&value, "events_expired", exprtk_val_int(result ? (int64_t)ruleforge_continuous_result_get_events_expired(result) : 0));
    if (result) (void)ruleforge_continuous_result_get_watermark(result, &watermark, &has_watermark);
    exprtk_map_set(&value, "watermark", exprtk_val_int(watermark));
    exprtk_map_set(&value, "has_watermark", exprtk_val_bool(has_watermark != 0));
    exprtk_map_set(&value, "output_count", exprtk_val_int(result ? ruleforge_continuous_result_get_output_count(result) : 0));
    if (st != RFG_OK) rfg_set_error(ud->ctx, NULL);
    return value;
}

static exprtk_value_t fn_continuous_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_config_t config;
    ruleforge_continuous_session_t session = NULL;
    ruleforge_knowledge_base_t kb;
    ruleforge_status_t st;
    int kb_h, h;
    if (!ud || (argc != 1 && argc != 2) || !rfg_arg_int(args[0], &kb_h) ||
        !rfg_continuous_config_from_value(ud, argc == 2 ? args[1] : rfg_null(), &config)) {
        rfg_bad_args(ud, "rules_forge.continuous_create: expected (kb, config?)");
        return exprtk_val_num(-1.0);
    }
    kb = rfg_get_kb(ud->ctx, kb_h);
    if (!kb) {
        rfg_bad_args(ud, "rules_forge.continuous_create: invalid kb handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_continuous_session_create(kb, &config, &session);
    if (st != RFG_OK || !session) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    h = rfg_alloc_continuous(ud->ctx, session, kb_h);
    if (h < 0) {
        ruleforge_continuous_session_destroy(session);
        rfg_set_error(ud->ctx, "rules_forge.continuous_create: too many session handles");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_int(h);
}

static exprtk_value_t fn_continuous_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_continuous(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_destroy: invalid handle"), ud);
    rfg_destroy_continuous_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_continuous_push_json(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_result_t result = NULL;
    char *type, *event_id, *entry_point, *json;
    int h;
    int64_t event_time;
    ruleforge_status_t st;
    if (!ud || argc != 6 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &event_id) ||
        !rfg_arg_string(ud, args[3], &entry_point) || !rfg_arg_i64(args[4], &event_time) ||
        !rfg_arg_string(ud, args[5], &json))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud,
            "rules_forge.continuous_push_json: expected (session, type, event_id, entry_point, event_time_ms, json)"), NULL, -1);
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_push_json: invalid session handle"), NULL, h);
    st = ruleforge_continuous_push_json(
        session, type, event_id, entry_point, event_time, json, &result);
    return rfg_continuous_result_value(ud, st, result, h);
}

static exprtk_value_t fn_continuous_push_yaml(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_result_t result = NULL;
    char *type, *event_id, *entry_point, *yaml;
    int h;
    int64_t event_time;
    ruleforge_status_t st;
    if (!ud || argc != 6 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &event_id) ||
        !rfg_arg_string(ud, args[3], &entry_point) || !rfg_arg_i64(args[4], &event_time) ||
        !rfg_arg_string(ud, args[5], &yaml))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud,
            "rules_forge.continuous_push_yaml: expected (session, type, event_id, entry_point, event_time_ms, yaml)"), NULL, -1);
    session = rfg_get_continuous(ud->ctx, h);
    if (!session)
        return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_push_yaml: invalid session handle"), NULL, h);
    st = ruleforge_continuous_push_yaml(
        session, type, event_id, entry_point, event_time, yaml, &result);
    return rfg_continuous_result_value(ud, st, result, h);
}

static exprtk_value_t fn_continuous_push_data_bind_object(size_t argc,
                                                           exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_data_bind_object_t object;
    ruleforge_continuous_result_t result = NULL;
    char *event_id;
    char *entry_point;
    int session_h;
    int object_h;
    int64_t event_time;
    ruleforge_status_t status;
    if (!ud || argc != 5 || !rfg_arg_int(args[0], &session_h) ||
        !rfg_arg_int(args[1], &object_h) || !rfg_arg_string(ud, args[2], &event_id) ||
        !rfg_arg_string(ud, args[3], &entry_point) ||
        !rfg_arg_i64(args[4], &event_time))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud,
            "rules_forge.continuous_push_data_bind_object: expected (session, object, event_id, entry_point, event_time_ms)"), NULL, -1);
    session = rfg_get_continuous(ud->ctx, session_h);
    object = rfg_get_data_bind_object(ud->ctx, object_h);
    if (!session || !object)
        return rfg_continuous_result_value(ud, rfg_bad_args(ud,
            "rules_forge.continuous_push_data_bind_object: invalid session or object handle"),
            NULL, session_h);
    status = ruleforge_continuous_push_data_bind_object(
        session, object, event_id, entry_point, event_time, &result);
    return rfg_continuous_result_value(ud, status, result, session_h);
}

typedef enum {
    RFG_CONTINUOUS_PATH_JSON,
    RFG_CONTINUOUS_PATH_YAML,
    RFG_CONTINUOUS_PATH_CSV,
    RFG_CONTINUOUS_PATH_XML
} rfg_continuous_path_kind_t;

static exprtk_value_t rfg_continuous_push_path(size_t argc, exprtk_value_t *args, void *ud_,
                                               rfg_continuous_path_kind_t kind,
                                               const char *error_text) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_result_t result = NULL;
    char *type, *source, *path, *event_id_field, *event_time_field, *entry_point;
    int h;
    ruleforge_status_t st;
    if (!ud || argc != 7 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &source) ||
        !rfg_arg_string(ud, args[3], &path) || !rfg_arg_string(ud, args[4], &event_id_field) ||
        !rfg_arg_string(ud, args[5], &event_time_field) ||
        !rfg_arg_string(ud, args[6], &entry_point))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud, error_text), NULL, -1);
    session = rfg_get_continuous(ud->ctx, h);
    if (!session)
        return rfg_continuous_result_value(ud,
            rfg_bad_args(ud, "rules_forge.continuous_push_path: invalid session handle"),
            NULL, h);
    if (kind == RFG_CONTINUOUS_PATH_JSON)
        st = ruleforge_continuous_push_json_path(session, type, source, path,
            event_id_field, event_time_field, entry_point, &result);
    else if (kind == RFG_CONTINUOUS_PATH_YAML)
        st = ruleforge_continuous_push_yaml_path(session, type, source, path,
            event_id_field, event_time_field, entry_point, &result);
    else if (kind == RFG_CONTINUOUS_PATH_CSV)
        st = ruleforge_continuous_push_csv_path(session, type, source, path,
            event_id_field, event_time_field, entry_point, &result);
    else
        st = ruleforge_continuous_push_xml_path(session, type, source, path,
            event_id_field, event_time_field, entry_point, &result);
    return rfg_continuous_result_value(ud, st, result, h);
}

#define RFG_CONTINUOUS_PUSH_PATH_FN(fn_name, kind_value, error_text)                 \
    static exprtk_value_t fn_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud) {     \
        return rfg_continuous_push_path(argc, args, ud, kind_value, error_text);     \
    }

RFG_CONTINUOUS_PUSH_PATH_FN(fn_continuous_push_json_path, RFG_CONTINUOUS_PATH_JSON,
    "rules_forge.continuous_push_json_path: expected (session, type, json, json_path, event_id_field, event_time_field, entry_point)")
RFG_CONTINUOUS_PUSH_PATH_FN(fn_continuous_push_yaml_path, RFG_CONTINUOUS_PATH_YAML,
    "rules_forge.continuous_push_yaml_path: expected (session, type, yaml, yaml_path, event_id_field, event_time_field, entry_point)")
RFG_CONTINUOUS_PUSH_PATH_FN(fn_continuous_push_csv_path, RFG_CONTINUOUS_PATH_CSV,
    "rules_forge.continuous_push_csv_path: expected (session, type, csv, csv_path, event_id_field, event_time_field, entry_point)")
RFG_CONTINUOUS_PUSH_PATH_FN(fn_continuous_push_xml_path, RFG_CONTINUOUS_PATH_XML,
    "rules_forge.continuous_push_xml_path: expected (session, type, xml, xml_path, event_id_field, event_time_field, entry_point)")

static exprtk_value_t fn_continuous_advance_watermark(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_result_t result = NULL;
    int h;
    int64_t watermark;
    ruleforge_status_t st;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_i64(args[1], &watermark))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_advance_watermark: expected (session, watermark_ms)"), NULL, -1);
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_advance_watermark: invalid session handle"), NULL, h);
    st = ruleforge_continuous_advance_watermark(session, watermark, &result);
    return rfg_continuous_result_value(ud, st, result, h);
}

static exprtk_value_t fn_continuous_drain(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_result_t result = NULL;
    int h;
    ruleforge_status_t st;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_drain: expected session"), NULL, -1);
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_drain: invalid session handle"), NULL, h);
    st = ruleforge_continuous_drain(session, &result);
    return rfg_continuous_result_value(ud, st, result, h);
}

static exprtk_value_t fn_continuous_acknowledge(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    int h;
    int64_t batch_id;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_i64(args[1], &batch_id) || batch_id < 0)
        return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_acknowledge: expected (session, batch_id)"), ud);
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_acknowledge: invalid session handle"), ud);
    return rfg_status(ruleforge_continuous_acknowledge(session, (uint64_t)batch_id), ud);
}

static exprtk_value_t fn_continuous_metrics(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_metrics_t metrics;
    exprtk_value_t value = exprtk_val_object();
    int h;
    ruleforge_status_t st;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return value;
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return value;
    st = ruleforge_continuous_get_metrics(session, &metrics);
    if (st != RFG_OK) { rfg_set_error(ud->ctx, NULL); return value; }
#define RFG_METRIC(name) exprtk_map_set(&value, #name, exprtk_val_int((int64_t)metrics.name))
    RFG_METRIC(accepted_events); RFG_METRIC(expired_events);
    RFG_METRIC(rejected_duplicates); RFG_METRIC(rejected_late_events);
    RFG_METRIC(rejected_resource_limits); RFG_METRIC(replay_recoveries);
    RFG_METRIC(active_events); RFG_METRIC(dedup_entries);
    RFG_METRIC(pending_result_batches); RFG_METRIC(pending_results);
#undef RFG_METRIC
    return value;
}

static exprtk_value_t fn_continuous_result_output(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_result_t result;
    ruleforge_fact_t fact = NULL;
    int h, index, fact_h;
    ruleforge_status_t st;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_int(args[1], &index))
        return exprtk_val_num(-1.0);
    result = rfg_get_continuous_result(ud->ctx, h);
    if (!result) return exprtk_val_num(-1.0);
    st = ruleforge_continuous_result_get_output(result, index, &fact);
    if (st != RFG_OK || !fact) { rfg_set_error(ud->ctx, NULL); return exprtk_val_num(-1.0); }
    fact_h = rfg_alloc_fact(ud->ctx, fact, 3, h);
    return exprtk_val_int(fact_h);
}

static exprtk_value_t fn_continuous_result_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_continuous_result(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_result_destroy: invalid handle"), ud);
    rfg_destroy_continuous_result_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_continuous_stream_json_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_data_bind_stream_t stream = NULL;
    char *type, *event_id, *entry_point;
    int h, stream_h;
    int64_t event_time;
    ruleforge_status_t st;
    if (!ud || argc != 5 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &event_id) ||
        !rfg_arg_string(ud, args[3], &entry_point) || !rfg_arg_i64(args[4], &event_time)) {
        rfg_bad_args(ud, "rules_forge.continuous_stream_json_create: expected (session, type, event_id, entry_point, event_time_ms)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return exprtk_val_num(-1.0);
    st = ruleforge_continuous_data_bind_stream_json_create(session, type, event_id,
                                                            entry_point, event_time, &stream);
    if (st != RFG_OK || !stream) { rfg_set_error(ud->ctx, NULL); return exprtk_val_num(-1.0); }
    stream_h = rfg_alloc_continuous_stream(ud->ctx, stream, h);
    if (stream_h < 0) {
        ruleforge_continuous_data_bind_stream_destroy(stream);
        rfg_set_error(ud->ctx, "rules_forge.continuous_stream_json_create: too many stream handles");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_int(stream_h);
}

static exprtk_value_t fn_continuous_stream_yaml_create(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_data_bind_stream_t stream = NULL;
    char *type, *event_id, *entry_point;
    int h, stream_h;
    int64_t event_time;
    ruleforge_status_t st;
    if (!ud || argc != 5 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &event_id) ||
        !rfg_arg_string(ud, args[3], &entry_point) || !rfg_arg_i64(args[4], &event_time)) {
        rfg_bad_args(ud, "rules_forge.continuous_stream_yaml_create: expected (session, type, event_id, entry_point, event_time_ms)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) return exprtk_val_num(-1.0);
    st = ruleforge_continuous_data_bind_stream_yaml_create(session, type, event_id,
                                                            entry_point, event_time, &stream);
    if (st != RFG_OK || !stream) { rfg_set_error(ud->ctx, NULL); return exprtk_val_num(-1.0); }
    stream_h = rfg_alloc_continuous_stream(ud->ctx, stream, h);
    if (stream_h < 0) {
        ruleforge_continuous_data_bind_stream_destroy(stream);
        rfg_set_error(ud->ctx, "rules_forge.continuous_stream_yaml_create: too many stream handles");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_int(stream_h);
}

static exprtk_value_t rfg_continuous_stream_path_create(size_t argc, exprtk_value_t *args,
                                                        void *ud_,
                                                        rfg_continuous_path_kind_t kind,
                                                        const char *error_text) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_session_t session;
    ruleforge_continuous_data_bind_stream_t stream = NULL;
    char *type, *path, *event_id_field, *event_time_field, *entry_point;
    int h, stream_h;
    ruleforge_status_t st;
    if (!ud || argc != 6 || !rfg_arg_int(args[0], &h) ||
        !rfg_arg_string(ud, args[1], &type) || !rfg_arg_string(ud, args[2], &path) ||
        !rfg_arg_string(ud, args[3], &event_id_field) ||
        !rfg_arg_string(ud, args[4], &event_time_field) ||
        !rfg_arg_string(ud, args[5], &entry_point)) {
        rfg_bad_args(ud, error_text);
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_continuous(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.continuous_stream_path_create: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    if (kind == RFG_CONTINUOUS_PATH_JSON)
        st = ruleforge_continuous_data_bind_stream_json_path_create(session, type, path,
            event_id_field, event_time_field, entry_point, &stream);
    else if (kind == RFG_CONTINUOUS_PATH_YAML)
        st = ruleforge_continuous_data_bind_stream_yaml_path_create(session, type, path,
            event_id_field, event_time_field, entry_point, &stream);
    else if (kind == RFG_CONTINUOUS_PATH_CSV)
        st = ruleforge_continuous_data_bind_stream_csv_path_create(session, type, path,
            event_id_field, event_time_field, entry_point, &stream);
    else
        st = ruleforge_continuous_data_bind_stream_xml_path_create(session, type, path,
            event_id_field, event_time_field, entry_point, &stream);
    if (st != RFG_OK || !stream) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    stream_h = rfg_alloc_continuous_stream(ud->ctx, stream, h);
    if (stream_h < 0) {
        ruleforge_continuous_data_bind_stream_destroy(stream);
        rfg_set_error(ud->ctx, "rules_forge.continuous_stream_path_create: too many stream handles");
        return exprtk_val_num(-1.0);
    }
    return exprtk_val_int(stream_h);
}

#define RFG_CONTINUOUS_STREAM_PATH_FN(fn_name, kind_value, error_text)               \
    static exprtk_value_t fn_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud) {      \
        return rfg_continuous_stream_path_create(argc, args, ud, kind_value,          \
                                                 error_text);                         \
    }

RFG_CONTINUOUS_STREAM_PATH_FN(fn_continuous_stream_json_path_create,
    RFG_CONTINUOUS_PATH_JSON,
    "rules_forge.continuous_stream_json_path_create: expected (session, type, json_path, event_id_field, event_time_field, entry_point)")
RFG_CONTINUOUS_STREAM_PATH_FN(fn_continuous_stream_yaml_path_create,
    RFG_CONTINUOUS_PATH_YAML,
    "rules_forge.continuous_stream_yaml_path_create: expected (session, type, yaml_path, event_id_field, event_time_field, entry_point)")
RFG_CONTINUOUS_STREAM_PATH_FN(fn_continuous_stream_csv_path_create,
    RFG_CONTINUOUS_PATH_CSV,
    "rules_forge.continuous_stream_csv_path_create: expected (session, type, csv_path, event_id_field, event_time_field, entry_point)")
RFG_CONTINUOUS_STREAM_PATH_FN(fn_continuous_stream_xml_path_create,
    RFG_CONTINUOUS_PATH_XML,
    "rules_forge.continuous_stream_xml_path_create: expected (session, type, xml_path, event_id_field, event_time_field, entry_point)")

static exprtk_value_t fn_continuous_stream_feed(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_data_bind_stream_t stream;
    const uint8_t *data;
    size_t len;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_bytes(args[1], &data, &len))
        return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_stream_feed: expected (stream, data)"), ud);
    stream = rfg_get_continuous_stream(ud->ctx, h);
    if (!stream) return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_stream_feed: invalid stream handle"), ud);
    return rfg_status(ruleforge_continuous_data_bind_stream_feed(stream, data, len), ud);
}

static exprtk_value_t fn_continuous_stream_feed_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_data_bind_stream_t stream;
    char *path;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &path))
        return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_stream_feed_file: expected (stream, path)"), ud);
    stream = rfg_get_continuous_stream(ud->ctx, h);
    if (!stream) return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_stream_feed_file: invalid stream handle"), ud);
    return rfg_status(ruleforge_continuous_data_bind_stream_feed_file(stream, path), ud);
}

static exprtk_value_t fn_continuous_stream_finish(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_continuous_data_bind_stream_t stream;
    ruleforge_continuous_result_t result = NULL;
    int h, continuous_h;
    ruleforge_status_t st;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h))
        return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_stream_finish: expected stream"), NULL, -1);
    stream = rfg_get_continuous_stream(ud->ctx, h);
    if (!stream) return rfg_continuous_result_value(ud, rfg_bad_args(ud, "rules_forge.continuous_stream_finish: invalid stream handle"), NULL, -1);
    continuous_h = ud->ctx->continuous_streams[h].continuous_handle;
    st = ruleforge_continuous_data_bind_stream_finish(stream, &result);
    return rfg_continuous_result_value(ud, st, result, continuous_h);
}

static exprtk_value_t fn_continuous_stream_destroy(size_t argc, exprtk_value_t *args, exprtk_env_t *env, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_continuous_stream(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.continuous_stream_destroy: invalid handle"), ud);
    rfg_destroy_continuous_stream_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

void rfg_plugin_load(void *p, void *e, void *s) {
    rfg_ctx_t *ctx = (rfg_ctx_t *)p;
    exprtk_env_t *env = (exprtk_env_t *)e;
    mem_pool_t *scratch = (mem_pool_t *)s;
    rfg_ud_t *ud;
    if (!ctx || !env || !scratch) return;
    ud = (rfg_ud_t *)mem_alloc(&env->arena, sizeof(*ud));
    if (!ud) return;
    ud->ctx = ctx;
    ud->env = env;
    ud->scratch = scratch;

    exprtk_env_register_func(env, "rules_forge.version", fn_version, ud);
    exprtk_env_register_func(env, "rules_forge.error", fn_error, ud);
    exprtk_env_register_func(env, "rules_forge.kb_create", fn_kb_create, ud);
    exprtk_env_register_func(env, "rules_forge.kb_destroy", fn_kb_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load", fn_kb_load, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load_file", fn_kb_load_file, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load_decision_table_csv", fn_kb_load_decision_table_csv, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_from_binary", fn_data_bind_object_from_binary, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_from_json", fn_data_bind_object_from_json, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_from_yaml", fn_data_bind_object_from_yaml, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_from_xml", fn_data_bind_object_from_xml, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_from_csv", fn_data_bind_object_from_csv, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_clone", fn_data_bind_object_clone, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_type", fn_data_bind_object_type, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_serialize_json", fn_data_bind_object_serialize_json, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_serialize_yaml", fn_data_bind_object_serialize_yaml, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_serialize_xml", fn_data_bind_object_serialize_xml, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_serialize_csv", fn_data_bind_object_serialize_csv, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_serialize_binary", fn_data_bind_object_serialize_binary, ud);
    exprtk_env_register_func(env, "rules_forge.data_bind_object_destroy", fn_data_bind_object_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.session_create", fn_session_create, ud);
    exprtk_env_register_func(env, "rules_forge.session_destroy", fn_session_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.session_reset", fn_session_reset, ud);
    exprtk_env_register_func(env, "rules_forge.session_fire_all", fn_session_fire_all, ud);
    exprtk_env_register_func(env, "rules_forge.session_fact_count", fn_session_fact_count, ud);
    exprtk_env_register_func(env, "rules_forge.session_set_validation_mode", fn_session_set_validation_mode, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_data_bind_object", fn_session_add_data_bind_object, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_json", fn_session_add_fact_json, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_json_path", fn_session_add_fact_json_path, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_json_path", fn_session_add_facts_json_path, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_yaml", fn_session_add_fact_yaml, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_yaml_path", fn_session_add_fact_yaml_path, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_yaml_path", fn_session_add_facts_yaml_path, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_json_file", fn_session_add_fact_json_file, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_binary", fn_session_add_fact_binary, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_csv", fn_session_add_facts_csv, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_csv_path", fn_session_add_facts_csv_path, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_csv_file", fn_session_add_facts_csv_file, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_xml", fn_session_add_facts_xml, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_xml_file", fn_session_add_facts_xml_file, ud);
    exprtk_env_register_func(env, "rules_forge.stream_json_create", fn_stream_json_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_json_all_create", fn_stream_json_all_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_json_path_create", fn_stream_json_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_json_path_all_create", fn_stream_json_path_all_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_yaml_create", fn_stream_yaml_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_yaml_all_create", fn_stream_yaml_all_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_yaml_path_create", fn_stream_yaml_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_yaml_path_all_create", fn_stream_yaml_path_all_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_csv_all_create", fn_stream_csv_all_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_csv_path_create", fn_stream_csv_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_xml_create", fn_stream_xml_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_xml_path_all_create", fn_stream_xml_path_all_create, ud);
    exprtk_env_register_func(env, "rules_forge.stream_feed", fn_stream_feed, ud);
    exprtk_env_register_func(env, "rules_forge.stream_feed_file", fn_stream_feed_file, ud);
    exprtk_env_register_func(env, "rules_forge.stream_finish", fn_stream_finish, ud);
    exprtk_env_register_func(env, "rules_forge.stream_destroy", fn_stream_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.session_query", fn_session_query, ud);
    exprtk_env_register_func(env, "rules_forge.query_size", fn_query_size, ud);
    exprtk_env_register_func(env, "rules_forge.query_fact", fn_query_fact, ud);
    exprtk_env_register_func(env, "rules_forge.query_destroy", fn_query_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.fact_string", fn_fact_string, ud);
    exprtk_env_register_func(env, "rules_forge.fact_double", fn_fact_double, ud);
    exprtk_env_register_func(env, "rules_forge.fact_int", fn_fact_int, ud);
    exprtk_env_register_func(env, "rules_forge.fact_bool", fn_fact_bool, ud);
    exprtk_env_register_func(env, "rules_forge.session_enable_tracing", fn_session_enable_tracing, ud);
    exprtk_env_register_func(env, "rules_forge.session_trace", fn_session_trace, ud);
    exprtk_env_register_func(env, "rules_forge.session_rule_performance", fn_session_rule_performance, ud);
    exprtk_env_register_func(env, "rules_forge.session_clear_trace", fn_session_clear_trace, ud);
    exprtk_env_register_func(env, "rules_forge.session_memory_used", fn_session_memory_used, ud);
    exprtk_env_register_func(env, "rules_forge.session_memory_peak", fn_session_memory_peak, ud);
    exprtk_env_register_func(env, "rules_forge.session_memory_stats", fn_session_memory_stats, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_create", fn_continuous_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_destroy", fn_continuous_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_data_bind_object", fn_continuous_push_data_bind_object, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_json", fn_continuous_push_json, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_json_path", fn_continuous_push_json_path, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_yaml", fn_continuous_push_yaml, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_yaml_path", fn_continuous_push_yaml_path, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_csv_path", fn_continuous_push_csv_path, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_push_xml_path", fn_continuous_push_xml_path, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_advance_watermark", fn_continuous_advance_watermark, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_drain", fn_continuous_drain, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_acknowledge", fn_continuous_acknowledge, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_metrics", fn_continuous_metrics, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_result_output", fn_continuous_result_output, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_result_destroy", fn_continuous_result_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_json_create", fn_continuous_stream_json_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_json_path_create", fn_continuous_stream_json_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_yaml_create", fn_continuous_stream_yaml_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_yaml_path_create", fn_continuous_stream_yaml_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_csv_path_create", fn_continuous_stream_csv_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_xml_path_create", fn_continuous_stream_xml_path_create, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_feed", fn_continuous_stream_feed, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_feed_file", fn_continuous_stream_feed_file, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_finish", fn_continuous_stream_finish, ud);
    exprtk_env_register_func(env, "rules_forge.continuous_stream_destroy", fn_continuous_stream_destroy, ud);
}
