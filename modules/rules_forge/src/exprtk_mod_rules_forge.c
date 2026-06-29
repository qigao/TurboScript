#include "rules_forge_ctx.h"

#define RFG_OK RULES_FORGE_OK
#define RFG_INVALID RULES_FORGE_ERROR_INVALID_ARGUMENT

static exprtk_value_t rfg_zero(void) {
    return exprtk_val_num(0.0);
}

static int rfg_is_int(exprtk_value_t v) {
    return v.type == EXPRTK_VAL_NUMBER || v.type == EXPRTK_VAL_INTEGER;
}

static int rfg_arg_int(exprtk_value_t v, int *out) {
    if (!out || !rfg_is_int(v)) return 0;
    *out = v.type == EXPRTK_VAL_INTEGER ? (int)v.data.integer : (int)v.data.number;
    return 1;
}

static int rfg_arg_string(rfg_ud_t *ud, exprtk_value_t v, char **out) {
    char *buf;
    if (!ud || !out || v.type != EXPRTK_VAL_STRING) return 0;
    buf = (char *)mem_alloc(ud->scratch ? ud->scratch : &ud->env->arena, v.data.string.len + 1);
    if (!buf) return 0;
    memcpy(buf, v.data.string.data, v.data.string.len);
    buf[v.data.string.len] = '\0';
    *out = buf;
    return 1;
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

static exprtk_value_t rfg_string(rfg_ud_t *ud, const char *s) {
    size_t len = s ? strlen(s) : 0;
    char *buf;
    if (!ud || !ud->env) return exprtk_val_str(tstr_v_from_buf("", 0));
    buf = (char *)mem_alloc(&ud->env->arena, len + 1);
    if (!buf) return rfg_zero();
    if (len) memcpy(buf, s, len);
    buf[len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, len));
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

static void rfg_destroy_session_handle(rfg_ctx_t *ctx, int h) {
    int i;
    if (!ctx || h < 0 || h >= RFG_MAX_HANDLES || !ctx->sessions[h].ptr) return;
    for (i = 0; i < RFG_MAX_HANDLES; i++) {
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
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_query_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_session_handle(ctx, i);
    for (i = 0; i < RFG_MAX_HANDLES; i++) rfg_destroy_kb_handle(ctx, i);
    if (ctx->initialized) ruleforge_cleanup();
    free(ctx);
}

static exprtk_value_t fn_version(size_t argc, exprtk_value_t *args, void *ud_) {
    (void)args;
    if (argc != 0) return rfg_zero();
    return rfg_string((rfg_ud_t *)ud_, ruleforge_get_version());
}

static exprtk_value_t fn_error(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    (void)args;
    if (argc != 0 || !ud || !ud->ctx) return rfg_zero();
    return rfg_string(ud, ud->ctx->error_msg);
}

static exprtk_value_t fn_kb_create(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_kb_destroy(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_kb(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.kb_destroy: invalid handle"), ud);
    rfg_destroy_kb_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_kb_load(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_kb_load_file(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_kb_load_decision_table_csv(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_kb_load_ts_plugin(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_knowledge_base_t kb;
    char *path;
    int h;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &path))
        return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load_ts_plugin: expected (kb, plugin_path)"), ud);
    kb = rfg_get_kb(ud->ctx, h);
    if (!kb) return rfg_status(rfg_bad_args(ud, "rules_forge.kb_load_ts_plugin: invalid kb handle"), ud);
    return rfg_status(ruleforge_kb_load_ts_plugin(kb, path), ud);
}

static exprtk_value_t fn_session_create(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_session_destroy(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_session(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_destroy: invalid handle"), ud);
    rfg_destroy_session_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_session_reset(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_session_fire_all(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_session_fact_count(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_num(-1.0);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return exprtk_val_num(-1.0);
    return exprtk_val_int((int64_t)ruleforge_session_get_fact_count(session));
}

static exprtk_value_t fn_session_set_validation_mode(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h, mode;
    if (!ud || argc != 2 || !rfg_arg_int(args[0], &h) || !rfg_arg_int(args[1], &mode))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_set_validation_mode: expected (session, mode)"), ud);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.session_set_validation_mode: invalid session handle"), ud);
    return rfg_status(ruleforge_session_set_validation_mode(session, (ruleforge_validation_mode_t)mode), ud);
}

static exprtk_value_t fn_session_add_fact_json(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *type, *json;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &type) ||
        !rfg_arg_string(ud, args[2], &json)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json: expected (session, type, json)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_json_ex(session, type, json, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_fact_json_schema(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    char *schema, *type, *json;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &schema) ||
        !rfg_arg_string(ud, args[2], &type) || !rfg_arg_string(ud, args[3], &json)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json_schema: expected (session, schema_path, type, json)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_json_schema: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_json_schema(session, schema, type, json, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_fact_binary_schema(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t fact = NULL;
    const uint8_t *bytes;
    size_t bytes_len;
    char *schema, *type;
    ruleforge_status_t st;
    int h, fh;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &schema) ||
        !rfg_arg_string(ud, args[2], &type) || !rfg_arg_bytes(args[3], &bytes, &bytes_len)) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_binary_schema: expected (session, schema_path, type, bytes)");
        return exprtk_val_num(-1.0);
    }
    session = rfg_get_session(ud->ctx, h);
    if (!session) {
        rfg_bad_args(ud, "rules_forge.session_add_fact_binary_schema: invalid session handle");
        return exprtk_val_num(-1.0);
    }
    st = ruleforge_session_add_fact_binary_schema(session, schema, type, bytes, bytes_len, &fact);
    if (st != RFG_OK || !fact) {
        rfg_set_error(ud->ctx, NULL);
        return exprtk_val_num(-1.0);
    }
    fh = rfg_alloc_fact(ud->ctx, fact, 1, h);
    return exprtk_val_num((double)fh);
}

static exprtk_value_t fn_session_add_facts_csv(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *type, *csv;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 3 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &type) ||
        !rfg_arg_string(ud, args[2], &csv))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv: expected (session, type, csv)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_csv_ex(session, type, csv, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_facts_csv_schema(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *schema, *type, *csv;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 4 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &schema) ||
        !rfg_arg_string(ud, args[2], &type) || !rfg_arg_string(ud, args[3], &csv))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv_schema: expected (session, schema_path, type, csv)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_csv_schema: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_csv_schema(session, schema, type, csv, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_add_facts_xml_schema(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    ruleforge_fact_t *facts = NULL;
    char *schema, *type, *xml, *xpath;
    int h, loaded = 0;
    ruleforge_status_t st;
    if (!ud || argc != 5 || !rfg_arg_int(args[0], &h) || !rfg_arg_string(ud, args[1], &schema) ||
        !rfg_arg_string(ud, args[2], &type) || !rfg_arg_string(ud, args[3], &xml) ||
        !rfg_arg_string(ud, args[4], &xpath))
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_xml_schema: expected (session, schema_path, type, xml, xpath)"), NULL, 0, -1);
    session = rfg_get_session(ud->ctx, h);
    if (!session)
        return rfg_status_facts_loaded(ud, rfg_bad_args(ud, "rules_forge.session_add_facts_xml_schema: invalid session handle"), NULL, 0, h);
    st = ruleforge_session_add_facts_xml_schema(session, schema, type, xml, xpath, &facts, &loaded);
    {
        exprtk_value_t result = rfg_status_facts_loaded(ud, st, facts, loaded, h);
        if (facts) ruleforge_fact_array_free(facts);
        return result;
    }
}

static exprtk_value_t fn_session_query(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_query_size(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_query_result_t query;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_num(-1.0);
    query = rfg_get_query(ud->ctx, h);
    if (!query) return exprtk_val_num(-1.0);
    return exprtk_val_int((int64_t)ruleforge_query_result_get_size(query));
}

static exprtk_value_t fn_query_fact(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_query_destroy(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h) || !rfg_get_query(ud->ctx, h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.query_destroy: invalid query handle"), ud);
    rfg_destroy_query_handle(ud->ctx, h);
    return rfg_status(RFG_OK, ud);
}

static exprtk_value_t fn_fact_string(size_t argc, exprtk_value_t *args, void *ud_) {
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
        buf = (char *)mem_alloc(&ud->env->arena, needed + 1);
        if (!buf) return rfg_zero();
        st = ruleforge_fact_get_field_as_string(fact, field, buf, needed + 1, &needed);
    }
    if (st != RFG_OK) {
        rfg_set_error(ud->ctx, NULL);
        return rfg_string(ud, "");
    }
    return rfg_string(ud, buf);
}

static exprtk_value_t fn_fact_double(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_fact_int(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_fact_bool(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_session_enable_tracing(size_t argc, exprtk_value_t *args, void *ud_) {
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
        buf = (char *)mem_alloc(&ud->env->arena, actual + 1);
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

static exprtk_value_t fn_session_trace(size_t argc, exprtk_value_t *args, void *ud_) {
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

static exprtk_value_t fn_session_rule_performance(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return rfg_string(ud, "");
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_string(ud, "");
    return rfg_session_string_call(ud, session, 0, 1);
}

static exprtk_value_t fn_session_clear_trace(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h))
        return rfg_status(rfg_bad_args(ud, "rules_forge.session_clear_trace: expected session"), ud);
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_status(rfg_bad_args(ud, "rules_forge.session_clear_trace: invalid session handle"), ud);
    return rfg_status(ruleforge_session_clear_trace(session), ud);
}

static exprtk_value_t fn_session_memory_used(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_int(-1);
    session = rfg_get_session(ud->ctx, h);
    return exprtk_val_int(session ? ruleforge_session_get_memory_used(session) : -1);
}

static exprtk_value_t fn_session_memory_peak(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return exprtk_val_int(-1);
    session = rfg_get_session(ud->ctx, h);
    return exprtk_val_int(session ? ruleforge_session_get_memory_peak(session) : -1);
}

static exprtk_value_t fn_session_memory_stats(size_t argc, exprtk_value_t *args, void *ud_) {
    rfg_ud_t *ud = (rfg_ud_t *)ud_;
    ruleforge_stateful_session_t session;
    int h;
    if (!ud || argc != 1 || !rfg_arg_int(args[0], &h)) return rfg_string(ud, "");
    session = rfg_get_session(ud->ctx, h);
    if (!session) return rfg_string(ud, "");
    return rfg_session_string_call(ud, session, 0, 2);
}

void rfg_plugin_load(void *p, void *e, void *s) {
    rfg_ctx_t *ctx = (rfg_ctx_t *)p;
    exprtk_env_t *env = (exprtk_env_t *)e;
    mem_pool_t *scratch = (mem_pool_t *)s;
    rfg_ud_t *ud;
    if (!ctx || !env) return;
    ud = (rfg_ud_t *)mem_alloc(&env->arena, sizeof(*ud));
    if (!ud) return;
    ud->ctx = ctx;
    ud->env = env;
    ud->scratch = scratch ? scratch : &env->arena;

    exprtk_env_register_func(env, "rules_forge.version", fn_version, ud);
    exprtk_env_register_func(env, "rules_forge.error", fn_error, ud);
    exprtk_env_register_func(env, "rules_forge.kb_create", fn_kb_create, ud);
    exprtk_env_register_func(env, "rules_forge.kb_destroy", fn_kb_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load", fn_kb_load, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load_file", fn_kb_load_file, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load_decision_table_csv", fn_kb_load_decision_table_csv, ud);
    exprtk_env_register_func(env, "rules_forge.kb_load_ts_plugin", fn_kb_load_ts_plugin, ud);
    exprtk_env_register_func(env, "rules_forge.session_create", fn_session_create, ud);
    exprtk_env_register_func(env, "rules_forge.session_destroy", fn_session_destroy, ud);
    exprtk_env_register_func(env, "rules_forge.session_reset", fn_session_reset, ud);
    exprtk_env_register_func(env, "rules_forge.session_fire_all", fn_session_fire_all, ud);
    exprtk_env_register_func(env, "rules_forge.session_fact_count", fn_session_fact_count, ud);
    exprtk_env_register_func(env, "rules_forge.session_set_validation_mode", fn_session_set_validation_mode, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_json", fn_session_add_fact_json, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_json_schema", fn_session_add_fact_json_schema, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_fact_binary_schema", fn_session_add_fact_binary_schema, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_csv", fn_session_add_facts_csv, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_csv_schema", fn_session_add_facts_csv_schema, ud);
    exprtk_env_register_func(env, "rules_forge.session_add_facts_xml_schema", fn_session_add_facts_xml_schema, ud);
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
}
