/**
 * @file exprtk_mod_parser.c
 * @brief Parser module functions for TurboScript
 */
#include "parser_ctx.h"
#include "data_bind.h"
#include "node_tree.h"
#include "schema_parser_dsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

#define PARSER_ZERO ((exprtk_value_t){EXPRTK_VAL_NUMBER, .data.number = 0.0})

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

typedef struct parser_csv_headers_s {
    char **names;
    size_t count;
    size_t cap;
} parser_csv_headers_t;

static void parser_csv_headers_free(parser_csv_headers_t *headers);
static int parser_csv_parse_header_names(exprtk_value_t csv_arg, parser_csv_headers_t *headers);
static int parser_arg_int(exprtk_value_t value, int *out);

/* Helper: convert string view to C string using scratch arena */
static inline char *parser_arena_cstr(mem_pool_t *arena, tstr_v sv) {
    char *buf = (char *)mem_alloc(arena, sv.len + 1);
    if (buf) {
        memcpy(buf, sv.data, sv.len);
        buf[sv.len] = '\0';
    }
    return buf;
}

/* Helper: allocate document handle */
static int parser_alloc_csv_handle(parser_ctx_t *ctx, turbo_csv_doc_t *doc,
                                   parser_csv_headers_t *headers) {
    for (int i = 0; i < PARSER_MAX_DOCS; i++) {
        if (!ctx->csv_docs[i]) {
            ctx->csv_docs[i] = doc;
            if (headers) {
                ctx->csv_header_names[i] = headers->names;
                ctx->csv_header_counts[i] = headers->count;
                headers->names = NULL;
                headers->count = 0;
                headers->cap = 0;
            }
            return i;
        }
    }
    return -1; /* No free slot */
}

/* Helper: free document handle */
static void parser_free_csv_handle(parser_ctx_t *ctx, int handle) {
    if (handle < 0 || handle >= PARSER_MAX_DOCS) return;
    
    if (ctx->csv_docs[handle]) {
        void *doc = ctx->csv_docs[handle];
        turbo_free_csv(&doc);
        ctx->csv_docs[handle] = NULL;
    }
    for (size_t i = 0; i < ctx->csv_header_counts[handle]; i++)
        free(ctx->csv_header_names[handle][i]);
    free(ctx->csv_header_names[handle]);
    ctx->csv_header_names[handle] = NULL;
    ctx->csv_header_counts[handle] = 0;
}

static turbo_csv_doc_t *parser_get_csv_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_DOCS) return NULL;
    return ctx->csv_docs[handle];
}

static parser_csv_headers_t parser_get_csv_headers(parser_ctx_t *ctx, int handle) {
    parser_csv_headers_t headers = {0};
    if (!ctx || handle < 0 || handle >= PARSER_MAX_DOCS) return headers;
    headers.names = ctx->csv_header_names[handle];
    headers.count = ctx->csv_header_counts[handle];
    headers.cap = ctx->csv_header_counts[handle];
    return headers;
}

static int parser_alloc_json_handle(parser_ctx_t *ctx, json_value_t *doc) {
    if (!ctx || !doc) return -1;
    for (int i = 0; i < PARSER_MAX_JSON_DOCS; i++) {
        if (!ctx->json_docs[i]) {
            ctx->json_docs[i] = doc;
            return i;
        }
    }
    return -1;
}

static json_value_t *parser_get_json_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_JSON_DOCS) return NULL;
    return ctx->json_docs[handle];
}

static void parser_free_json_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_JSON_DOCS) return;
    if (ctx->json_docs[handle]) {
        void *doc = ctx->json_docs[handle];
        turbo_free_json(&doc);
        ctx->json_docs[handle] = NULL;
    }
}

static int parser_alloc_schema_handle(parser_ctx_t *ctx, Node *schema_root, DataBind *codec) {
    if (!ctx || !schema_root || !codec) return -1;
    for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
        if (!ctx->schemas[i]) {
            ctx->schemas[i] = (struct turbo_node_s *)schema_root;
            ctx->schema_codecs[i] = codec;
            return i;
        }
    }
    return -1;
}

static Node *parser_get_schema_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_SCHEMAS) return NULL;
    return (Node *)ctx->schemas[handle];
}

static DataBind *parser_get_schema_codec(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_SCHEMAS) return NULL;
    return ctx->schema_codecs[handle];
}

static void parser_free_schema_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_SCHEMAS) return;
    if (ctx->schema_codecs[handle]) {
        data_bind_free(ctx->schema_codecs[handle]);
        ctx->schema_codecs[handle] = NULL;
    }
    if (ctx->schemas[handle]) {
        node_free((Node *)ctx->schemas[handle]);
        ctx->schemas[handle] = NULL;
    }
}

/* =========================================================================
 * CSV Functions
 * ========================================================================= */

/**
 * parser.csv_parse(data: string, has_header: int = 1) -> int
 * 解析CSV数据，返回文档句柄
 */
static exprtk_value_t fn_csv_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    parser_csv_headers_t headers = {0};
    
    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
        return PARSER_ZERO;
    }
    
    tstr_v data_sv = args[0].data.string;
    int has_header = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) 
                     ? (int)args[1].data.number : 1;
    
    turbo_csv_options_t opts = {has_header != 0, ',', '"', true};
    turbo_csv_doc_t *doc = NULL;
    
    int rc = turbo_parse_csv_opts((const uint8_t *)data_sv.data, data_sv.len, &opts, &doc);
    if (rc != 0 || !doc) {
        return PARSER_ZERO;
    }
    
    if (has_header) (void)parser_csv_parse_header_names(args[0], &headers);
    int handle = parser_alloc_csv_handle(ud->ctx, doc, &headers);
    if (handle < 0) {
        void *ptr = doc;
        turbo_free_csv(&ptr);
        parser_csv_headers_free(&headers);
        return PARSER_ZERO;
    }
    
    return exprtk_val_int(handle);
}

/**
 * parser.csv_parse_file(path: string, has_header: int = 1) -> int
 * 从文件解析CSV，返回文档句柄
 */
static exprtk_value_t fn_csv_parse_file(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    parser_csv_headers_t headers = {0};
    
    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING) {
        return PARSER_ZERO;
    }
    
    char *path = parser_arena_cstr(ud->scratch, args[0].data.string);
    if (!path) return PARSER_ZERO;
    
    int has_header = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) 
                     ? (int)args[1].data.number : 1;
    
    /* Read file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) {
        return PARSER_ZERO;
    }
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize <= 0) {
        fclose(f);
        return PARSER_ZERO;
    }
    
    uint8_t *data = (uint8_t *)malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return PARSER_ZERO;
    }
    
    size_t nread = fread(data, 1, (size_t)fsize, f);
    fclose(f);
    
    if (nread == 0) {
        free(data);
        return PARSER_ZERO;
    }
    
    /* Parse CSV */
    turbo_csv_options_t opts = {has_header != 0, ',', '"', true};
    turbo_csv_doc_t *doc = NULL;
    
    int rc = turbo_parse_csv_opts((const uint8_t *)data, nread, &opts, &doc);
    if (rc == 0 && doc && has_header) {
        exprtk_value_t csv_text = exprtk_val_str(tstr_v_from_buf((char *)data, nread));
        (void)parser_csv_parse_header_names(csv_text, &headers);
    }
    free(data);
    
    if (rc != 0 || !doc) {
        parser_csv_headers_free(&headers);
        return PARSER_ZERO;
    }
    
    int handle = parser_alloc_csv_handle(ud->ctx, doc, &headers);
    if (handle < 0) {
        void *ptr = doc;
        turbo_free_csv(&ptr);
        parser_csv_headers_free(&headers);
        return PARSER_ZERO;
    }
    
    return exprtk_val_int(handle);
}

/**
 * parser.csv_rows(handle: int) -> int
 * 获取CSV行数
 */
static exprtk_value_t fn_csv_rows(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 1 || (args[0].type != EXPRTK_VAL_INTEGER && args[0].type != EXPRTK_VAL_NUMBER)) {
        return PARSER_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    if (handle < 0 || handle >= PARSER_MAX_DOCS || !ud->ctx->csv_docs[handle]) {
        return PARSER_ZERO;
    }
    
    size_t rows = turbo_csv_row_count(ud->ctx->csv_docs[handle]);
    return exprtk_val_num((double)rows);
}

/**
 * parser.csv_cols(handle: int) -> int
 * 获取CSV列数
 */
static exprtk_value_t fn_csv_cols(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 1 || (args[0].type != EXPRTK_VAL_INTEGER && args[0].type != EXPRTK_VAL_NUMBER)) {
        return PARSER_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    if (handle < 0 || handle >= PARSER_MAX_DOCS || !ud->ctx->csv_docs[handle]) {
        return PARSER_ZERO;
    }
    
    size_t cols = turbo_csv_column_count(ud->ctx->csv_docs[handle]);
    return exprtk_val_num((double)cols);
}

/**
 * parser.csv_get(handle: int, row: int, col: int) -> string
 * 获取CSV单元格值
 */
static exprtk_value_t fn_csv_get(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 3) {
        return PARSER_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    if (handle < 0 || handle >= PARSER_MAX_DOCS || !ud->ctx->csv_docs[handle]) {
        return PARSER_ZERO;
    }
    
    size_t row = (size_t)((args[1].type == EXPRTK_VAL_INTEGER) 
                          ? args[1].data.integer 
                          : args[1].data.number);
    size_t col = (size_t)((args[2].type == EXPRTK_VAL_INTEGER) 
                          ? args[2].data.integer 
                          : args[2].data.number);
    
    const char *val = turbo_csv_get(ud->ctx->csv_docs[handle], row, col);
    if (!val) {
        return exprtk_val_str(tstr_v_from_buf("", 0));
    }
    
    size_t len = strlen(val);
    char *buf = (char *)mem_alloc(ud->scratch, len + 1);
    if (buf) {
        memcpy(buf, val, len);
        buf[len] = '\0';
        return exprtk_val_str(tstr_v_from_buf(buf, len));
    }
    
    return PARSER_ZERO;
}

/**
 * parser.csv_get_num(handle: int, row: int, col: int, [default: number]) -> number
 * 获取CSV单元格数值
 */
static exprtk_value_t fn_csv_get_num(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 3) {
        return PARSER_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    if (handle < 0 || handle >= PARSER_MAX_DOCS || !ud->ctx->csv_docs[handle]) {
        return PARSER_ZERO;
    }
    
    size_t row = (size_t)((args[1].type == EXPRTK_VAL_INTEGER) 
                          ? args[1].data.integer 
                          : args[1].data.number);
    size_t col = (size_t)((args[2].type == EXPRTK_VAL_INTEGER) 
                          ? args[2].data.integer 
                          : args[2].data.number);
    
    double default_val = (argc >= 4 && args[3].type == EXPRTK_VAL_NUMBER) 
                         ? args[3].data.number : 0.0;
    
    double val = turbo_csv_get_double(ud->ctx->csv_docs[handle], row, col, default_val);
    return exprtk_val_num(val);
}

static turbo_csv_doc_t *parser_parse_csv_string(exprtk_value_t value) {
    turbo_csv_doc_t *doc = NULL;
    turbo_csv_options_t opts = {true, ',', '"', true};

    if (value.type != EXPRTK_VAL_STRING) return NULL;
    if (turbo_parse_csv_opts((const uint8_t *)value.data.string.data,
                             value.data.string.len, &opts, &doc) != 0) {
        return NULL;
    }
    return doc;
}

static void parser_free_csv_doc(turbo_csv_doc_t *doc) {
    void *ptr = doc;
    if (ptr) turbo_free_csv(&ptr);
}

static int parser_csv_arg_col(turbo_csv_doc_t *doc, exprtk_value_t arg, size_t *out_col) {
    if (!doc || !out_col) return 0;

    if (arg.type == EXPRTK_VAL_INTEGER || arg.type == EXPRTK_VAL_NUMBER) {
        double n = arg.type == EXPRTK_VAL_INTEGER ? (double)arg.data.integer : arg.data.number;
        if (n < 0) return 0;
        *out_col = (size_t)n;
    } else if (arg.type == EXPRTK_VAL_STRING) {
        char name_buf[256];
        size_t len = arg.data.string.len;
        if (len >= sizeof(name_buf)) return 0;
        memcpy(name_buf, arg.data.string.data, len);
        name_buf[len] = '\0';
        *out_col = turbo_csv_find_column(doc, name_buf);
    } else {
        return 0;
    }

    return *out_col < turbo_csv_column_count(doc);
}

static exprtk_value_t fn_csv_rows_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    if (argc < 1) return PARSER_ZERO;
    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    if (!doc) return PARSER_ZERO;
    exprtk_value_t result = exprtk_val_num((double)turbo_csv_row_count(doc));
    parser_free_csv_doc(doc);
    return result;
}

static exprtk_value_t fn_csv_cols_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    if (argc < 1) return PARSER_ZERO;
    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    if (!doc) return PARSER_ZERO;
    exprtk_value_t result = exprtk_val_num((double)turbo_csv_column_count(doc));
    parser_free_csv_doc(doc);
    return result;
}

static exprtk_value_t fn_csv_get_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    if (!ud || !ud->env || argc < 3) return PARSER_ZERO;

    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    if (!doc) return PARSER_ZERO;

    size_t row = (size_t)(args[1].type == EXPRTK_VAL_INTEGER
                              ? args[1].data.integer
                              : args[1].data.number);
    size_t col = 0;
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));

    if (parser_csv_arg_col(doc, args[2], &col)) {
        const char *val = turbo_csv_get(doc, row, col);
        if (val) {
            size_t len = strlen(val);
            char *buf = (char *)mem_alloc(&ud->env->arena, len + 1);
            if (buf) {
                memcpy(buf, val, len);
                buf[len] = '\0';
                result = exprtk_val_str(tstr_v_from_buf(buf, len));
            }
        }
    }

    parser_free_csv_doc(doc);
    return result;
}

static exprtk_value_t fn_csv_get_num_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    if (argc < 3) return PARSER_ZERO;

    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    if (!doc) return PARSER_ZERO;

    size_t row = (size_t)(args[1].type == EXPRTK_VAL_INTEGER
                              ? args[1].data.integer
                              : args[1].data.number);
    size_t col = 0;
    double def = (argc >= 4 && args[3].type == EXPRTK_VAL_NUMBER) ? args[3].data.number : 0.0;
    double val = def;

    if (parser_csv_arg_col(doc, args[2], &col))
        val = turbo_csv_get_double(doc, row, col, def);

    parser_free_csv_doc(doc);
    return exprtk_val_num(val);
}

static exprtk_value_t fn_csv_col_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    if (!ud || !ud->env || argc < 2) return exprtk_val_vec(NULL, 0);

    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    if (!doc) return exprtk_val_vec(NULL, 0);

    size_t col = 0;
    exprtk_value_t result = exprtk_val_vec(NULL, 0);
    if (parser_csv_arg_col(doc, args[1], &col)) {
        size_t rows = turbo_csv_row_count(doc);
        double *data = rows ? (double *)mem_alloc(&ud->env->arena, rows * sizeof(double)) : NULL;
        if (rows == 0 || data) {
            for (size_t i = 0; i < rows; ++i)
                data[i] = turbo_csv_get_double(doc, i, col, 0.0);
            result = exprtk_val_vec(data, rows);
        }
    }

    parser_free_csv_doc(doc);
    return result;
}

static char *parser_filter_expr_cstr(parser_ud_t *ud, exprtk_value_t value) {
    if (!ud || !ud->env || value.type != EXPRTK_VAL_STRING) return NULL;
    char *buf = (char *)mem_alloc(&ud->env->arena, value.data.string.len + 1);
    if (!buf) return NULL;
    memcpy(buf, value.data.string.data, value.data.string.len);
    buf[value.data.string.len] = '\0';
    return buf;
}

static int parser_csv_find_named_column(turbo_csv_doc_t *doc, const char *name, size_t *out_col) {
    char typed_name[256];
    size_t col;

    if (!doc || !name || !out_col) return 0;

    col = turbo_csv_find_column(doc, name);
    if (col < turbo_csv_column_count(doc)) {
        *out_col = col;
        return 1;
    }

    if (snprintf(typed_name, sizeof(typed_name), "%s_n", name) < (int)sizeof(typed_name)) {
        col = turbo_csv_find_column(doc, typed_name);
        if (col < turbo_csv_column_count(doc)) {
            *out_col = col;
            return 1;
        }
    }

    if (snprintf(typed_name, sizeof(typed_name), "%s_s", name) < (int)sizeof(typed_name)) {
        col = turbo_csv_find_column(doc, typed_name);
        if (col < turbo_csv_column_count(doc)) {
            *out_col = col;
            return 1;
        }
    }

    return 0;
}

static void parser_csv_sanitize_path(const char *path, char *out, size_t out_size) {
    size_t w = 0;
    int last_underscore = 0;
    if (!out || out_size == 0) return;
    if (!path) {
        out[0] = '\0';
        return;
    }

    for (size_t i = 0; path[i] != '\0' && w + 1 < out_size; ++i) {
        char ch = path[i];
        if (ch == ']' || ch == ')') continue;
        if (ch == '.' || ch == '[' || ch == '(') {
            if (w > 0 && !last_underscore) {
                out[w++] = '_';
                last_underscore = 1;
            }
            continue;
        }
        out[w++] = ch;
        last_underscore = 0;
    }
    if (w > 0 && out[w - 1] == '_') --w;
    out[w] = '\0';
}

static int parser_csv_find_path_column(turbo_csv_doc_t *doc, const char *path, size_t *out_col) {
    char sanitized[256];

    if (parser_csv_find_named_column(doc, path, out_col)) return 1;
    parser_csv_sanitize_path(path, sanitized, sizeof(sanitized));
    if (sanitized[0] != '\0' && strcmp(sanitized, path) != 0)
        return parser_csv_find_named_column(doc, sanitized, out_col);
    return 0;
}

static int parser_compare_number(double lhs, const char *op, double rhs) {
    if (strcmp(op, ">") == 0) return lhs > rhs;
    if (strcmp(op, ">=") == 0) return lhs >= rhs;
    if (strcmp(op, "<") == 0) return lhs < rhs;
    if (strcmp(op, "<=") == 0) return lhs <= rhs;
    if (strcmp(op, "==") == 0) return lhs == rhs;
    if (strcmp(op, "!=") == 0) return lhs != rhs;
    return 0;
}

static int parser_csv_filter_row_matches(turbo_csv_doc_t *doc, size_t row, const char *expr) {
    char lhs[64], rhs_name[64], op[3], text[128];
    double rhs;
    size_t col, rhs_col;

    if (!doc || !expr) return 0;

    if (sscanf(expr, " %63[A-Za-z0-9_] + %63[A-Za-z0-9_] %2[!<>=] %lf",
               lhs, rhs_name, op, &rhs) == 4) {
        if (!parser_csv_find_named_column(doc, lhs, &col) ||
            !parser_csv_find_named_column(doc, rhs_name, &rhs_col)) {
            return 0;
        }
        double sum = turbo_csv_get_double(doc, row, col, 0.0) +
                     turbo_csv_get_double(doc, row, rhs_col, 0.0);
        return parser_compare_number(sum, op, rhs);
    }

    if (sscanf(expr, " %63[A-Za-z0-9_] == \"%127[^\"]\"", lhs, text) == 2) {
        if (!parser_csv_find_named_column(doc, lhs, &col)) return 0;
        const char *value = turbo_csv_get(doc, row, col);
        return value && strcmp(value, text) == 0;
    }

    if (sscanf(expr, " %63[A-Za-z0-9_] %2[!<>=] %lf", lhs, op, &rhs) == 3) {
        if (!parser_csv_find_named_column(doc, lhs, &col)) return 0;
        return parser_compare_number(turbo_csv_get_double(doc, row, col, 0.0), op, rhs);
    }

    return 0;
}

static exprtk_value_t fn_csv_filter_count_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    if (!ud || argc < 2) return PARSER_ZERO;

    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    char *expr = parser_filter_expr_cstr(ud, args[1]);
    if (!doc || !expr) {
        parser_free_csv_doc(doc);
        return PARSER_ZERO;
    }

    size_t matches = 0;
    size_t rows = turbo_csv_row_count(doc);
    for (size_t i = 0; i < rows; ++i) {
        if (parser_csv_filter_row_matches(doc, i, expr)) matches++;
    }

    parser_free_csv_doc(doc);
    return exprtk_val_num((double)matches);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} parser_filter_buf_t;

static int parser_buf_reserve(parser_filter_buf_t *buf, size_t extra) {
    size_t need;
    size_t next_cap;
    char *next;

    if (!buf) return 0;
    need = buf->len + extra + 1;
    if (need <= buf->cap) return 1;
    next_cap = buf->cap ? buf->cap * 2 : 128;
    while (next_cap < need) next_cap *= 2;
    next = (char *)realloc(buf->data, next_cap);
    if (!next) return 0;
    buf->data = next;
    buf->cap = next_cap;
    return 1;
}

static int parser_buf_append_len(parser_filter_buf_t *buf, const char *text, size_t len) {
    if (!buf || !text) return 0;
    if (!parser_buf_reserve(buf, len)) return 0;
    memcpy(buf->data + buf->len, text, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return 1;
}

static int parser_buf_append(parser_filter_buf_t *buf, const char *text) {
    return parser_buf_append_len(buf, text, text ? strlen(text) : 0);
}

static int parser_buf_append_char(parser_filter_buf_t *buf, char ch) {
    return parser_buf_append_len(buf, &ch, 1);
}

static int parser_csv_append_cell(parser_filter_buf_t *buf, const char *text) {
    const char *p;
    int quote = 0;

    if (!buf) return 0;
    if (!text) text = "";
    for (p = text; *p; ++p) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            quote = 1;
            break;
        }
    }
    if (!quote) return parser_buf_append(buf, text);
    if (!parser_buf_append_char(buf, '"')) return 0;
    for (p = text; *p; ++p) {
        if (*p == '"' && !parser_buf_append_char(buf, '"')) return 0;
        if (!parser_buf_append_char(buf, *p)) return 0;
    }
    return parser_buf_append_char(buf, '"');
}

static int parser_csv_append_doc_row(parser_filter_buf_t *buf, turbo_csv_doc_t *doc,
                                     size_t row, size_t cols) {
    if (!buf || !doc) return 0;
    for (size_t col = 0; col < cols; ++col) {
        const char *cell;
        if (col > 0 && !parser_buf_append_char(buf, ',')) return 0;
        cell = turbo_csv_get(doc, row, col);
        if (!parser_csv_append_cell(buf, cell ? cell : "")) return 0;
    }
    return 1;
}

static int parser_csv_append_header_row(parser_filter_buf_t *buf,
                                        const parser_csv_headers_t *headers,
                                        size_t cols) {
    if (!buf || !headers || headers->count == 0) return 0;
    for (size_t col = 0; col < cols; ++col) {
        const char *cell = col < headers->count ? headers->names[col] : "";
        if (col > 0 && !parser_buf_append_char(buf, ',')) return 0;
        if (!parser_csv_append_cell(buf, cell ? cell : "")) return 0;
    }
    return 1;
}

static int parser_buf_append_fmt(parser_filter_buf_t *buf, const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int n;

    if (!buf || !fmt) return 0;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return 0;
    }
    if (!parser_buf_reserve(buf, (size_t)n)) {
        va_end(ap2);
        return 0;
    }
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap2);
    va_end(ap2);
    buf->len += (size_t)n;
    return 1;
}

static exprtk_value_t parser_buf_to_string(parser_filter_buf_t *buf, exprtk_env_t *env) {
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    char *copy;

    if (!env || !buf || !buf->data) return result;
    copy = (char *)mem_alloc(&env->arena, buf->len + 1);
    if (!copy) return result;
    memcpy(copy, buf->data, buf->len + 1);
    return exprtk_val_str(tstr_v_from_buf(copy, buf->len));
}

typedef struct {
    size_t *values;
    size_t count;
    size_t cap;
} parser_index_list_t;

static void parser_csv_headers_free(parser_csv_headers_t *headers) {
    if (!headers) return;
    for (size_t i = 0; i < headers->count; ++i)
        free(headers->names[i]);
    free(headers->names);
    headers->names = NULL;
    headers->count = 0;
    headers->cap = 0;
}

static int parser_csv_headers_push(parser_csv_headers_t *headers, const char *text, size_t len) {
    char **next;
    char *copy;

    if (!headers || !text) return 0;
    if (headers->count == headers->cap) {
        size_t next_cap = headers->cap ? headers->cap * 2 : 8;
        next = (char **)realloc(headers->names, next_cap * sizeof(char *));
        if (!next) return 0;
        headers->names = next;
        headers->cap = next_cap;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, text, len);
    copy[len] = '\0';
    headers->names[headers->count++] = copy;
    return 1;
}

static int parser_csv_headers_contains(const parser_csv_headers_t *headers, const char *text) {
    if (!headers || !text) return 0;
    for (size_t i = 0; i < headers->count; ++i)
        if (headers->names[i] && strcmp(headers->names[i], text) == 0) return 1;
    return 0;
}

static int parser_csv_headers_push_unique(parser_csv_headers_t *headers, const char *text) {
    if (!headers || !text) return 0;
    if (parser_csv_headers_contains(headers, text)) return 1;
    return parser_csv_headers_push(headers, text, strlen(text));
}

static int parser_csv_headers_merge(parser_csv_headers_t *dest,
                                    const parser_csv_headers_t *src) {
    if (!dest || !src) return 0;
    for (size_t i = 0; i < src->count; ++i)
        if (!parser_csv_headers_push_unique(dest, src->names[i])) return 0;
    return 1;
}

static int parser_csv_parse_header_names(exprtk_value_t csv_arg, parser_csv_headers_t *headers) {
    const char *data;
    size_t len;
    size_t i = 0;
    parser_filter_buf_t cell = {0};
    int in_quotes = 0;

    if (!headers || csv_arg.type != EXPRTK_VAL_STRING) return 0;
    memset(headers, 0, sizeof(*headers));
    data = csv_arg.data.string.data;
    len = csv_arg.data.string.len;

    while (i < len) {
        char ch = data[i++];
        if (in_quotes) {
            if (ch == '"') {
                if (i < len && data[i] == '"') {
                    if (!parser_buf_append_char(&cell, '"')) goto fail;
                    ++i;
                } else {
                    in_quotes = 0;
                }
            } else if (!parser_buf_append_char(&cell, ch)) {
                goto fail;
            }
            continue;
        }

        if (ch == '"') {
            in_quotes = 1;
        } else if (ch == ',') {
            if (!parser_csv_headers_push(headers, cell.data ? cell.data : "", cell.len)) goto fail;
            cell.len = 0;
            if (cell.data) cell.data[0] = '\0';
        } else if (ch == '\n' || ch == '\r') {
            if (!parser_csv_headers_push(headers, cell.data ? cell.data : "", cell.len)) goto fail;
            free(cell.data);
            return 1;
        } else if (!parser_buf_append_char(&cell, ch)) {
            goto fail;
        }
    }

    if (!parser_csv_headers_push(headers, cell.data ? cell.data : "", cell.len)) goto fail;
    free(cell.data);
    return headers->count > 0;

fail:
    free(cell.data);
    parser_csv_headers_free(headers);
    return 0;
}

static int parser_csv_header_map_key(const char *header, const char *path,
                                     char *key, size_t key_size) {
    size_t path_len;
    const char *start = NULL;
    const char *end;
    size_t len;

    if (!header || !path || !key || key_size == 0) return 0;
    path_len = strlen(path);
    if (strncmp(header, path, path_len) == 0 && header[path_len] == '.') {
        start = header + path_len + 1;
    } else if (strncmp(header, path, path_len) == 0 && header[path_len] == '_') {
        start = header + path_len + 1;
    }
    if (!start || start[0] == '\0') return 0;

    end = start;
    while (*end && *end != '.' && *end != '[' && *end != '_') ++end;
    len = (size_t)(end - start);
    if (len == 0 || len >= key_size) return 0;
    memcpy(key, start, len);
    key[len] = '\0';
    return 1;
}

static int parser_csv_map_key_seen(exprtk_value_t *map, const char *key) {
    return exprtk_value_is_object_like(map) && key && exprtk_map_has(map, key);
}

static void parser_index_list_free(parser_index_list_t *indexes) {
    if (!indexes) return;
    free(indexes->values);
    indexes->values = NULL;
    indexes->count = 0;
    indexes->cap = 0;
}

static int parser_index_list_contains(const parser_index_list_t *indexes, size_t value) {
    if (!indexes) return 0;
    for (size_t i = 0; i < indexes->count; ++i)
        if (indexes->values[i] == value) return 1;
    return 0;
}

static int parser_index_list_push(parser_index_list_t *indexes, size_t value) {
    size_t *next;

    if (!indexes || parser_index_list_contains(indexes, value)) return 1;
    if (indexes->count == indexes->cap) {
        size_t next_cap = indexes->cap ? indexes->cap * 2 : 8;
        next = (size_t *)realloc(indexes->values, next_cap * sizeof(size_t));
        if (!next) return 0;
        indexes->values = next;
        indexes->cap = next_cap;
    }
    indexes->values[indexes->count++] = value;
    return 1;
}

static int parser_index_compare(const void *a, const void *b) {
    size_t lhs = *(const size_t *)a;
    size_t rhs = *(const size_t *)b;
    return (lhs > rhs) - (lhs < rhs);
}

static int parser_csv_header_group_index(const char *header, const char *path, size_t *out_index) {
    char sanitized[256];
    const char *start = NULL;
    char *end = NULL;
    unsigned long value;
    size_t path_len;

    if (!header || !path || !out_index) return 0;
    path_len = strlen(path);
    if (strncmp(header, path, path_len) == 0 && header[path_len] == '[') {
        start = header + path_len + 1;
    } else {
        parser_csv_sanitize_path(path, sanitized, sizeof(sanitized));
        path_len = strlen(sanitized);
        if (path_len > 0 && strncmp(header, sanitized, path_len) == 0 && header[path_len] == '_')
            start = header + path_len + 1;
    }
    if (!start || start[0] < '0' || start[0] > '9') return 0;

    errno = 0;
    value = strtoul(start, &end, 10);
    if (errno != 0 || !end || end == start) return 0;
    if (*end != ']' && *end != '_' && *end != '.' && *end != '\0') return 0;
    *out_index = (size_t)value;
    return 1;
}

static int parser_csv_collect_group_indexes(const parser_csv_headers_t *headers,
                                            const char *path,
                                            parser_index_list_t *indexes) {
    if (!headers || !path || !indexes) return 0;
    for (size_t i = 0; i < headers->count; ++i) {
        size_t index = 0;
        if (parser_csv_header_group_index(headers->names[i], path, &index) &&
            !parser_index_list_push(indexes, index)) {
            return 0;
        }
    }
    if (indexes->count > 1)
        qsort(indexes->values, indexes->count, sizeof(size_t), parser_index_compare);
    return indexes->count > 0;
}

static int parser_csv_headers_have_path(const parser_csv_headers_t *headers, const char *path) {
    char sanitized[256];
    size_t path_len;
    size_t sanitized_len;

    if (!headers || !path) return 0;
    path_len = strlen(path);
    parser_csv_sanitize_path(path, sanitized, sizeof(sanitized));
    sanitized_len = strlen(sanitized);

    for (size_t i = 0; i < headers->count; ++i) {
        const char *header = headers->names[i];
        if (!header) continue;
        if (strcmp(header, path) == 0) return 1;
        if (path_len > 0 && strncmp(header, path, path_len) == 0 &&
            (header[path_len] == '.' || header[path_len] == '[')) return 1;
        if (sanitized_len > 0 && strcmp(header, sanitized) == 0) return 1;
        if (sanitized_len > 0 && strncmp(header, sanitized, sanitized_len) == 0 &&
            header[sanitized_len] == '_') return 1;
    }
    return 0;
}

static int parser_csv_text_is_empty(const char *text) {
    return !text || text[0] == '\0';
}

static int parser_csv_header_matches_path(const char *header, const char *path) {
    char sanitized[256];
    size_t path_len;
    size_t sanitized_len;

    if (!header || !path) return 0;
    path_len = strlen(path);
    parser_csv_sanitize_path(path, sanitized, sizeof(sanitized));
    sanitized_len = strlen(sanitized);

    if (strcmp(header, path) == 0) return 1;
    if (path_len > 0 && strncmp(header, path, path_len) == 0 &&
        (header[path_len] == '.' || header[path_len] == '[')) return 1;
    if (sanitized_len > 0 && strcmp(header, sanitized) == 0) return 1;
    if (sanitized_len > 0 && strncmp(header, sanitized, sanitized_len) == 0 &&
        header[sanitized_len] == '_') return 1;
    return 0;
}

static int parser_csv_row_has_nonempty_path(turbo_csv_doc_t *doc, size_t row,
                                            const parser_csv_headers_t *headers,
                                            const char *path) {
    if (!doc || !headers || !path || row >= turbo_csv_row_count(doc)) return 0;
    for (size_t i = 0; i < headers->count; ++i) {
        if (parser_csv_header_matches_path(headers->names[i], path) &&
            !parser_csv_text_is_empty(turbo_csv_get(doc, row, i))) {
            return 1;
        }
    }
    return 0;
}

static void parser_filter_append(void *user_data, size_t row_index, const char *rendered_row) {
    (void)row_index;
    parser_filter_buf_t *buf = (parser_filter_buf_t *)user_data;
    size_t row_len;
    size_t need;
    char *next;

    if (!buf || !rendered_row) return;
    row_len = strlen(rendered_row);
    need = buf->len + row_len + (buf->len ? 1 : 0) + 1;
    if (need > buf->cap) {
        size_t next_cap = buf->cap ? buf->cap * 2 : 128;
        while (next_cap < need) next_cap *= 2;
        next = (char *)realloc(buf->data, next_cap);
        if (!next) return;
        buf->data = next;
        buf->cap = next_cap;
    }
    if (buf->len) buf->data[buf->len++] = '\n';
    memcpy(buf->data + buf->len, rendered_row, row_len);
    buf->len += row_len;
    buf->data[buf->len] = '\0';
}

static exprtk_value_t fn_csv_filter_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    parser_filter_buf_t out = {0};
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));

    if (!ud || !ud->env || argc < 2) return result;

    turbo_csv_doc_t *doc = parser_parse_csv_string(args[0]);
    char *expr = parser_filter_expr_cstr(ud, args[1]);
    if (!doc || !expr) {
        parser_free_csv_doc(doc);
        return result;
    }

    size_t rows = turbo_csv_row_count(doc);
    size_t cols = turbo_csv_column_count(doc);
    for (size_t row = 0; row < rows; ++row) {
        if (!parser_csv_filter_row_matches(doc, row, expr)) continue;
        parser_filter_buf_t row_buf = {0};
        for (size_t col = 0; col < cols; ++col) {
            const char *cell = turbo_csv_get(doc, row, col);
            parser_filter_append(&row_buf, row, cell ? cell : "");
        }
        if (row_buf.data) {
            parser_filter_append(&out, row, row_buf.data);
            free(row_buf.data);
        }
    }

    if (out.data) {
        char *buf = (char *)mem_alloc(&ud->env->arena, out.len + 1);
        if (buf) {
            memcpy(buf, out.data, out.len + 1);
            result = exprtk_val_str(tstr_v_from_buf(buf, out.len));
        }
    }

    free(out.data);
    parser_free_csv_doc(doc);
    return result;
}

static exprtk_value_t fn_csv_filter_table_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    parser_csv_headers_t headers = {0};
    parser_filter_buf_t out = {0};
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    turbo_csv_doc_t *doc;
    char *expr;
    size_t rows;
    size_t cols;
    int wrote_row = 0;

    if (!ud || !ud->env || argc < 2) return result;

    doc = parser_parse_csv_string(args[0]);
    expr = parser_filter_expr_cstr(ud, args[1]);
    if (!doc || !expr || !parser_csv_parse_header_names(args[0], &headers)) {
        parser_free_csv_doc(doc);
        parser_csv_headers_free(&headers);
        return result;
    }

    rows = turbo_csv_row_count(doc);
    cols = turbo_csv_column_count(doc);
    if (cols > 0 && parser_csv_append_header_row(&out, &headers, cols)) {
        wrote_row = 1;
        for (size_t row = 0; row < rows; ++row) {
            if (!parser_csv_filter_row_matches(doc, row, expr)) continue;
            if (wrote_row && !parser_buf_append_char(&out, '\n')) break;
            if (!parser_csv_append_doc_row(&out, doc, row, cols)) break;
            wrote_row = 1;
        }
    }

    if (out.data) {
        char *buf = (char *)mem_alloc(&ud->env->arena, out.len + 1);
        if (buf) {
            memcpy(buf, out.data, out.len + 1);
            result = exprtk_val_str(tstr_v_from_buf(buf, out.len));
        }
    }

    free(out.data);
    parser_csv_headers_free(&headers);
    parser_free_csv_doc(doc);
    return result;
}

static exprtk_value_t fn_csv_write_inline(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    if (!ud || argc < 2 ||
        args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return exprtk_val_num(-1.0);
    }

    char *path = parser_arena_cstr(&ud->env->arena, args[0].data.string);
    if (!path) return exprtk_val_num(-1.0);

    FILE *f = fopen(path, "wb");
    if (!f) return exprtk_val_num(-1.0);
    size_t written = fwrite(args[1].data.string.data, 1, args[1].data.string.len, f);
    int close_rc = fclose(f);
    return exprtk_val_num((written == args[1].data.string.len && close_rc == 0) ? 0.0 : -1.0);
}

/**
 * parser.csv_close(handle: int) -> int
 * 关闭CSV文档
 */
static exprtk_value_t fn_csv_close(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 1) {
        return PARSER_ZERO;
    }
    
    int handle = (args[0].type == EXPRTK_VAL_INTEGER) 
                 ? (int)args[0].data.integer 
                 : (int)args[0].data.number;
    
    parser_free_csv_handle(ud->ctx, handle);
    return exprtk_val_num(1.0);
}

/* =========================================================================
 * Schema Binding Helpers
 * ========================================================================= */

typedef enum {
    PARSER_BIND_NUMBER,
    PARSER_BIND_INTEGER,
    PARSER_BIND_STRING,
    PARSER_BIND_BYTES,
    PARSER_BIND_BOOL,
    PARSER_BIND_UUID,
    PARSER_BIND_DATETIME,
    PARSER_BIND_DATE,
    PARSER_BIND_TIME,
    PARSER_BIND_DURATION,
    PARSER_BIND_DECIMAL,
    PARSER_BIND_BIGINT,
    PARSER_BIND_MONEY,
    PARSER_BIND_UNSUPPORTED
} parser_bind_kind_t;

static exprtk_value_t parser_null(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static Node *parser_node_child(Node *parent, const char *name) {
    if (!parent || !name) return NULL;
    if (parent->type == NODE_MAP) {
        for (size_t i = 0; i < parent->data.map.count; ++i) {
            Node *child = parent->data.map.items[i];
            if (child && child->name && strcmp(child->name, name) == 0) return child;
        }
    } else if (parent->type == NODE_LIST) {
        for (size_t i = 0; i < parent->data.list.count; ++i) {
            Node *child = parent->data.list.items[i];
            if (child && child->name && strcmp(child->name, name) == 0) return child;
        }
    }
    return NULL;
}

static const char *parser_node_string(Node *node, const char *name) {
    Node *child = parser_node_child(node, name);
    return child && child->type == NODE_STRING ? child->data.string_val : NULL;
}

static int parser_node_has(Node *node, const char *name) {
    return parser_node_child(node, name) != NULL;
}

static int parser_field_missing_allowed(Node *field) {
    return parser_node_has(field, "is_optional") || parser_node_has(field, "has_default");
}

static void parser_set_ctx_error(parser_ctx_t *ctx, const char *message) {
    if (!ctx) return;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", message ? message : "");
}

static const char *parser_schema_error_message(const tbe_error_t *err) {
    if (!err) return "schema parse failed";
    if (err->message[0] != '\0') return err->message;
    return tbe_error_string(err->code);
}

static Node *parser_parse_schema_value_ex(exprtk_value_t value, tbe_error_t *err) {
    Node *root;

    if (err) tbe_error_init(err);
    if (value.type != EXPRTK_VAL_STRING) {
        tbe_error_set(err, TBE_ERR_INVALID_ARGUMENT, -1, -1, "expected schema string");
        return NULL;
    }
    root = create_node_map(NULL);
    if (!root) {
        tbe_error_set(err, TBE_ERR_OUT_OF_MEMORY, -1, -1, "schema allocation failed");
        return NULL;
    }
    if (parse_schema(value.data.string.data, value.data.string.len, root, err) != 0) {
        node_free(root);
        return NULL;
    }
    return root;
}

static Node *parser_parse_schema_value(exprtk_value_t value) {
    tbe_error_t err;
    return parser_parse_schema_value_ex(value, &err);
}

static int parser_create_schema_state(exprtk_value_t value, Node **out_root,
                                      DataBind **out_codec, tbe_error_t *err) {
    Node *root;
    DataBind *codec = NULL;
    DataBindError db_err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;

    if (out_root) *out_root = NULL;
    if (out_codec) *out_codec = NULL;
    if (err) tbe_error_init(err);
    if (!out_root || !out_codec) {
        tbe_error_set(err, TBE_ERR_INVALID_ARGUMENT, -1, -1, "invalid schema output");
        return 0;
    }

    root = parser_parse_schema_value_ex(value, err);
    if (!root) return 0;

    status = data_bind_create_from_text(value.data.string.data, value.data.string.len,
                                        &codec, &db_err);
    if (status != DATA_BIND_OK || !codec) {
        node_free(root);
        tbe_error_set(err, TBE_ERR_SEMANTIC_ERROR,
                      db_err.line >= 0 ? db_err.line : -1,
                      db_err.column >= 0 ? db_err.column : -1,
                      db_err.message[0] != '\0' ? db_err.message
                                                : data_bind_status_name(status));
        return 0;
    }

    *out_root = root;
    *out_codec = codec;
    return 1;
}

static Node *parser_find_record(Node *root, const char *type_name) {
    const char *lists[] = {"messages", "composites", "groups"};
    for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); ++l) {
        Node *records = parser_node_child(root, lists[l]);
        if (!records || records->type != NODE_LIST) continue;
        for (size_t i = 0; i < records->data.list.count; ++i) {
            Node *record = records->data.list.items[i];
            const char *name = parser_node_string(record, "name");
            if (name && strcmp(name, type_name) == 0) return record;
        }
    }
    return NULL;
}

static Node *parser_find_union(Node *root, const char *type_name) {
    Node *unions = parser_node_child(root, "unions");
    if (!unions || unions->type != NODE_LIST || !type_name) return NULL;
    for (size_t i = 0; i < unions->data.list.count; ++i) {
        Node *record = unions->data.list.items[i];
        const char *name = parser_node_string(record, "name");
        if (!name) name = parser_node_string(record, "union_name");
        if (name && strcmp(name, type_name) == 0) return record;
    }
    return NULL;
}

static Node *parser_find_enum(Node *root, const char *type_name) {
    Node *enums = parser_node_child(root, "enums");
    if (!enums || enums->type != NODE_LIST || !type_name) return NULL;
    for (size_t i = 0; i < enums->data.list.count; ++i) {
        Node *e = enums->data.list.items[i];
        const char *name = parser_node_string(e, "name");
        if (!name) name = parser_node_string(e, "enum_name");
        if (name && strcmp(name, type_name) == 0) return e;
    }
    return NULL;
}

static int parser_is_enum_type(Node *root, const char *type_name) {
    return parser_find_enum(root, type_name) != NULL;
}

static int parser_is_flags_type(Node *root, const char *type_name) {
    Node *e = parser_find_enum(root, type_name);
    return e && parser_node_has(e, "is_flags");
}

static parser_bind_kind_t parser_type_kind(Node *root, const char *type) {
    if (!type) return PARSER_BIND_UNSUPPORTED;
    if (strcmp(type, "uuid") == 0) return PARSER_BIND_UUID;
    if (strcmp(type, "datetime") == 0) return PARSER_BIND_DATETIME;
    if (strcmp(type, "date") == 0) return PARSER_BIND_DATE;
    if (strcmp(type, "time") == 0) return PARSER_BIND_TIME;
    if (strcmp(type, "duration") == 0) return PARSER_BIND_DURATION;
    if (strcmp(type, "decimal") == 0) return PARSER_BIND_DECIMAL;
    if (strcmp(type, "bigint") == 0) return PARSER_BIND_BIGINT;
    if (strcmp(type, "money") == 0) return PARSER_BIND_MONEY;
    if (strcmp(type, "bytes") == 0) return PARSER_BIND_BYTES;
    if (strcmp(type, "string") == 0) return PARSER_BIND_STRING;
    if (strcmp(type, "bool") == 0) return PARSER_BIND_BOOL;
    if (strcmp(type, "float") == 0 || strcmp(type, "double") == 0) return PARSER_BIND_NUMBER;
    if (strstr(type, "int") != NULL || strcmp(type, "uint8") == 0 || strcmp(type, "uint16") == 0 ||
        strcmp(type, "uint32") == 0 || strcmp(type, "uint64") == 0 || strcmp(type, "byte") == 0 ||
        parser_is_enum_type(root, type)) {
        return PARSER_BIND_INTEGER;
    }
    return PARSER_BIND_UNSUPPORTED;
}

static parser_bind_kind_t parser_field_kind(Node *root, Node *field) {
    const char *type = parser_node_string(field, "type");
    if (!type || parser_node_has(field, "is_collection") || parser_node_has(field, "is_composite_ref") ||
        parser_node_has(field, "is_group_field")) {
        return PARSER_BIND_UNSUPPORTED;
    }
    return parser_type_kind(root, type);
}

static int parser_type_supported(Node *root, const char *type_name) {
    return parser_find_record(root, type_name) || parser_find_union(root, type_name) ||
           parser_type_kind(root, type_name) != PARSER_BIND_UNSUPPORTED;
}

static exprtk_value_t parser_string_value(exprtk_env_t *env, const char *text) {
    size_t len;
    char *buf;
    if (!env || !text) return exprtk_val_str(tstr_v_from_buf("", 0));
    len = strlen(text);
    buf = (char *)mem_alloc(&env->arena, len + 1);
    if (!buf) return exprtk_val_str(tstr_v_from_buf("", 0));
    memcpy(buf, text, len + 1);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static exprtk_value_t parser_string_value_len(exprtk_env_t *env, const char *text, size_t len) {
    char *buf;
    if (!env || !text) return exprtk_val_str(tstr_v_from_buf("", 0));
    buf = (char *)mem_alloc(&env->arena, len + 1);
    if (!buf) return exprtk_val_str(tstr_v_from_buf("", 0));
    memcpy(buf, text, len);
    buf[len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static exprtk_value_t parser_data_bind_bytes_value(exprtk_env_t *env,
                                                   const DataBindValue *value) {
    size_t len = 0;
    const uint8_t *bytes = data_bind_value_as_bytes(value, &len);
    char *copy;

    if (!env || (!bytes && len > 0)) return PARSER_ZERO;
    copy = (char *)mem_alloc(&env->arena, len + 1);
    if (!copy) return PARSER_ZERO;
    if (len > 0) memcpy(copy, bytes, len);
    copy[len] = '\0';
    return exprtk_val_bytes(tstr_v_from_buf(copy, len));
}

static exprtk_value_t parser_data_bind_uuid_value(const DataBindValue *value) {
    uuid_t uuid;
    if (!data_bind_value_as_uuid(value, &uuid)) return PARSER_ZERO;
    return exprtk_val_uuid(uuid);
}

static exprtk_value_t parser_data_bind_datetime_value(const DataBindValue *value) {
    turbo_datetime_t dt;
    if (!data_bind_value_as_datetime(value, &dt)) return PARSER_ZERO;
    return exprtk_val_datetime(dt);
}

static exprtk_value_t parser_data_bind_date_value(const DataBindValue *value) {
    DataBindDate date;
    if (!data_bind_value_as_date(value, &date)) return PARSER_ZERO;
    return exprtk_val_date((exprtk_date_t){date.year, date.month, date.day});
}

static exprtk_value_t parser_data_bind_time_value(const DataBindValue *value) {
    DataBindTime time;
    if (!data_bind_value_as_time(value, &time)) return PARSER_ZERO;
    return exprtk_val_time((exprtk_time_t){time.hour, time.minute, time.second,
                                           time.millisecond});
}

static exprtk_value_t parser_data_bind_duration_value(const DataBindValue *value) {
    int64_t ms = 0;
    if (data_bind_value_get_duration_milliseconds(value, &ms) != DATA_BIND_OK) return PARSER_ZERO;
    return exprtk_val_duration(ms);
}

static exprtk_value_t parser_data_bind_decimal_value(const DataBindValue *value) {
    DataBindDecimal decimal;
    if (data_bind_value_get_decimal(value, &decimal) != DATA_BIND_OK) return PARSER_ZERO;
    return exprtk_val_decimal((exprtk_decimal_t){decimal.mantissa, decimal.scale});
}

static exprtk_value_t parser_data_bind_bigint_value(exprtk_env_t *env,
                                                    const DataBindValue *value) {
    const char *text = data_bind_value_as_bigint_string(value);
    return exprtk_val_bigint(parser_string_value(env, text ? text : "").data.string);
}

static exprtk_value_t parser_data_bind_money_value(parser_ud_t *ud,
                                                   const DataBindValue *value) {
    DataBindMoney money;
    exprtk_money_t out;

    if (!ud || data_bind_value_get_money(value, &money) != DATA_BIND_OK) return parser_null();
    (void)ud;
    memset(&out, 0, sizeof(out));
    out.amount = (exprtk_decimal_t){money.amount.mantissa, money.amount.scale};
    memcpy(out.currency, money.currency, sizeof(out.currency));
    out.currency[3] = '\0';
    return exprtk_val_money(out);
}

static exprtk_value_t parser_data_bind_value_to_exprtk(parser_ud_t *ud,
                                                       const DataBindValue *value) {
    size_t i;

    if (!ud || !value) return parser_null();

    switch (data_bind_value_kind(value)) {
        case DATA_BIND_VALUE_OBJECT: {
            exprtk_value_t map = exprtk_val_object();
            size_t count = data_bind_value_field_count(value);
            for (i = 0; i < count; i++) {
                const char *name = data_bind_value_field_name(value, i);
                const DataBindValue *child = data_bind_value_field_at(value, i);
                if (name) exprtk_map_set(&map, name, parser_data_bind_value_to_exprtk(ud, child));
            }
            return map;
        }
        case DATA_BIND_VALUE_LIST:
        case DATA_BIND_VALUE_SET: {
            exprtk_value_t list = data_bind_value_kind(value) == DATA_BIND_VALUE_SET
                                      ? exprtk_val_set_empty()
                                      : exprtk_val_list_empty();
            size_t count = data_bind_value_count(value);
            for (i = 0; i < count; i++)
                exprtk_list_push(&list,
                                 parser_data_bind_value_to_exprtk(ud, data_bind_value_at(value, i)));
            return list;
        }
        case DATA_BIND_VALUE_MAP: {
            exprtk_value_t map = exprtk_val_map();
            size_t count = data_bind_value_count(value);
            for (i = 0; i < count; i++) {
                DataBindMapEntry entry = data_bind_value_map_entry_at(value, i);
                if (entry.key)
                    exprtk_map_set(&map, entry.key,
                                   parser_data_bind_value_to_exprtk(ud, entry.value));
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
            return parser_string_value(ud->env, data_bind_value_as_string(value));
        case DATA_BIND_VALUE_BYTES:
            return parser_data_bind_bytes_value(ud->env, value);
        case DATA_BIND_VALUE_UUID:
            return parser_data_bind_uuid_value(value);
        case DATA_BIND_VALUE_DATETIME:
            return parser_data_bind_datetime_value(value);
        case DATA_BIND_VALUE_DATE:
            return parser_data_bind_date_value(value);
        case DATA_BIND_VALUE_TIME:
            return parser_data_bind_time_value(value);
        case DATA_BIND_VALUE_DURATION:
            return parser_data_bind_duration_value(value);
        case DATA_BIND_VALUE_DECIMAL:
            return parser_data_bind_decimal_value(value);
        case DATA_BIND_VALUE_BIGINT:
            return parser_data_bind_bigint_value(ud->env, value);
        case DATA_BIND_VALUE_MONEY:
            return parser_data_bind_money_value(ud, value);
        case DATA_BIND_VALUE_NULL:
        default:
            return parser_null();
    }
}

static void parser_data_bind_error(parser_ud_t *ud, DataBindStatus status,
                                   const DataBindError *err, const char *fallback) {
    const char *message = fallback ? fallback : data_bind_status_name(status);
    if (err && err->message[0] != '\0') message = err->message;
    parser_set_ctx_error(ud ? ud->ctx : NULL, message);
}

static exprtk_value_t parser_data_bind_convert(parser_ud_t *ud, DataBindStatus status,
                                               DataBindValue *value,
                                               const DataBindError *err,
                                               exprtk_value_t failure_value) {
    exprtk_value_t result;
    if (status != DATA_BIND_OK || !value) {
        parser_data_bind_error(ud, status, err, "schema bind failed");
        return failure_value;
    }
    result = parser_data_bind_value_to_exprtk(ud, value);
    data_bind_value_free(value);
    parser_set_ctx_error(ud ? ud->ctx : NULL, "");
    return result;
}

static exprtk_value_t parser_data_bind_json_text(parser_ud_t *ud, DataBind *codec,
                                                 exprtk_value_t json_arg,
                                                 exprtk_value_t type_arg,
                                                 int bind_all) {
    char *type_name;
    DataBindValue *value = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    exprtk_value_t failure = bind_all ? exprtk_val_list_empty() : parser_null();

    if (!ud || !codec || json_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud ? ud->ctx : NULL, "expected JSON text and schema type");
        return failure;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) {
        parser_set_ctx_error(ud->ctx, "out of memory");
        return failure;
    }

    status = bind_all
                 ? data_bind_parse_json_all(codec, type_name, json_arg.data.string.data,
                                            json_arg.data.string.len, &value, &err)
                 : data_bind_parse_json(codec, type_name, json_arg.data.string.data,
                                        json_arg.data.string.len, &value, &err);
    return parser_data_bind_convert(ud, status, value, &err, failure);
}

static exprtk_value_t parser_data_bind_csv_text(parser_ud_t *ud, DataBind *codec,
                                                exprtk_value_t csv_arg,
                                                exprtk_value_t row_arg,
                                                exprtk_value_t type_arg,
                                                int bind_all) {
    char *type_name;
    int row = 0;
    DataBindValue *value = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    exprtk_value_t failure = bind_all ? exprtk_val_list_empty() : parser_null();

    if (!ud || !codec || csv_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud ? ud->ctx : NULL, "expected CSV text and schema type");
        return failure;
    }
    if (!bind_all && !parser_arg_int(row_arg, &row)) {
        parser_set_ctx_error(ud->ctx, "expected CSV row index");
        return failure;
    }
    if (row < 0) {
        parser_set_ctx_error(ud->ctx, "CSV row index must be non-negative");
        return failure;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) {
        parser_set_ctx_error(ud->ctx, "out of memory");
        return failure;
    }

    status = bind_all
                 ? data_bind_parse_csv_all(codec, type_name, csv_arg.data.string.data,
                                           csv_arg.data.string.len, &value, &err)
                 : data_bind_parse_csv(codec, type_name, csv_arg.data.string.data,
                                       csv_arg.data.string.len, (size_t)row, &value, &err);
    return parser_data_bind_convert(ud, status, value, &err, failure);
}

static exprtk_value_t parser_data_bind_xml_text(parser_ud_t *ud, DataBind *codec,
                                                exprtk_value_t xml_arg,
                                                exprtk_value_t xpath_arg,
                                                exprtk_value_t type_arg,
                                                int bind_all) {
    char *type_name;
    char *xpath = NULL;
    DataBindValue *value = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    exprtk_value_t failure = bind_all ? exprtk_val_list_empty() : parser_null();

    if (!ud || !codec || xml_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud ? ud->ctx : NULL, "expected XML text and schema type");
        return failure;
    }
    if (bind_all && xpath_arg.type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud->ctx, "expected XML XPath string");
        return failure;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) {
        parser_set_ctx_error(ud->ctx, "out of memory");
        return failure;
    }
    if (bind_all) {
        xpath = parser_arena_cstr(ud->scratch, xpath_arg.data.string);
        if (!xpath) {
            parser_set_ctx_error(ud->ctx, "out of memory");
            return failure;
        }
    }

    status = bind_all
                 ? data_bind_parse_xml_all(codec, type_name, xml_arg.data.string.data,
                                           xml_arg.data.string.len, xpath, &value, &err)
                 : data_bind_parse_xml(codec, type_name, xml_arg.data.string.data,
                                       xml_arg.data.string.len, &value, &err);
    return parser_data_bind_convert(ud, status, value, &err, failure);
}

static exprtk_value_t parser_data_bind_validate_text(
    parser_ud_t *ud, DataBind *codec, exprtk_value_t data_arg, exprtk_value_t type_arg,
    DataBindStatus (*validate_fn)(DataBind *, const char *, const char *, size_t,
                                  DataBindError *)) {
    char *type_name;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;

    if (!ud || !codec || data_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING || !validate_fn) {
        return exprtk_val_int(0);
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return exprtk_val_int(0);

    status = validate_fn(codec, type_name, data_arg.data.string.data,
                         data_arg.data.string.len, &err);
    if (status == DATA_BIND_OK) {
        parser_set_ctx_error(ud->ctx, "");
        return exprtk_val_int(1);
    }
    return exprtk_val_int(0);
}

static exprtk_value_t parser_data_bind_validate_xml_text(parser_ud_t *ud, DataBind *codec,
                                                         exprtk_value_t xml_arg,
                                                         exprtk_value_t xpath_arg,
                                                         exprtk_value_t type_arg) {
    char *type_name;
    char *xpath = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;

    if (!ud || !codec || xml_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        return exprtk_val_int(0);
    }
    if (xpath_arg.type != EXPRTK_VAL_NULL && xpath_arg.type != EXPRTK_VAL_STRING)
        return exprtk_val_int(0);

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return exprtk_val_int(0);
    if (xpath_arg.type == EXPRTK_VAL_STRING) {
        xpath = parser_arena_cstr(ud->scratch, xpath_arg.data.string);
        if (!xpath) return exprtk_val_int(0);
    }

    status = data_bind_validate_xml(codec, type_name, xml_arg.data.string.data,
                                    xml_arg.data.string.len, xpath, &err);
    if (status == DATA_BIND_OK) {
        parser_set_ctx_error(ud->ctx, "");
        return exprtk_val_int(1);
    }
    return exprtk_val_int(0);
}

static DataBind *parser_data_bind_from_schema_text(parser_ud_t *ud, exprtk_value_t schema_arg) {
    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;

    if (!ud || schema_arg.type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud ? ud->ctx : NULL, "expected schema string");
        return NULL;
    }
    status = data_bind_create_from_text(schema_arg.data.string.data, schema_arg.data.string.len,
                                        &codec, &err);
    if (status != DATA_BIND_OK || !codec) {
        parser_data_bind_error(ud, status, &err, "schema parse failed");
        return NULL;
    }
    return codec;
}

static int parser_parse_bool_text(const char *text) {
    if (!text) return 0;
    return strcmp(text, "true") == 0 || strcmp(text, "1") == 0 || strcmp(text, "yes") == 0;
}

static int parser_datetime_from_value(exprtk_value_t value, turbo_datetime_t *out) {
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_DATETIME) {
        *out = value.data.datetime;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data)
        return turbo_parse_datetime(value.data.string.data, value.data.string.len, out) == 0;
    return 0;
}

static int parser_datetime_to_text(const turbo_datetime_t *dt, char *out, size_t out_size) {
    time_t ts;
    if (!dt || !out || out_size == 0) return 0;
    ts = turbo_datetime_to_time(dt);
    if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, out, out_size) < 0) return 0;
    return 1;
}

static int parser_datetime_value_text(exprtk_value_t value, char *out, size_t out_size) {
    turbo_datetime_t dt;
    return parser_datetime_from_value(value, &dt) && parser_datetime_to_text(&dt, out, out_size);
}

static int parser_date_valid(int year, int month, int day) {
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days;
    if (year < 1 || month < 1 || month > 12 || day < 1) return 0;
    days = days_per_month[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days = 29;
    return day <= days;
}

static int parser_date_from_text(const char *text, exprtk_date_t *out) {
    turbo_datetime_t dt;
    int year = 0, month = 0, day = 0, consumed = 0;
    if (!text || !out) return 0;
    if (sscanf(text, "%d-%d-%d%n", &year, &month, &day, &consumed) == 3 ||
        sscanf(text, "%d/%d/%d%n", &year, &month, &day, &consumed) == 3) {
        if (text[consumed] != '\0' || !parser_date_valid(year, month, day)) return 0;
        out->year = year;
        out->month = month;
        out->day = day;
        return 1;
    }
    if (turbo_parse_datetime(text, strlen(text), &dt) == 0) {
        out->year = dt.year;
        out->month = dt.month;
        out->day = dt.day;
        return 1;
    }
    return 0;
}

static int parser_time_from_text(const char *text, exprtk_time_t *out) {
    int hour = 0, minute = 0, second = 0, millisecond = 0, consumed = 0;
    if (!text || !out) return 0;
    if (sscanf(text, "%d:%d:%d.%d%n", &hour, &minute, &second, &millisecond, &consumed) >= 3 ||
        sscanf(text, "%d:%d:%d%n", &hour, &minute, &second, &consumed) >= 3 ||
        sscanf(text, "%d:%d%n", &hour, &minute, &consumed) >= 2) {
        if (text[consumed] == '\0' && hour >= 0 && hour <= 23 && minute >= 0 &&
            minute <= 59 && second >= 0 && second <= 60 && millisecond >= 0 &&
            millisecond <= 999) {
            out->hour = hour;
            out->minute = minute;
            out->second = second;
            out->millisecond = millisecond;
            return 1;
        }
    }
    return 0;
}

static int parser_duration_from_text(const char *text, int64_t *out) {
    const char *p;
    int sign = 1;
    double total = 0.0;
    int saw_value = 0;
    long long hours = 0, minutes = 0, seconds = 0, millis = 0;
    int consumed = 0;
    int colon_sign;
    if (!text || !out) return 0;
    if (sscanf(text, "%lld:%lld:%lld.%lld%n", &hours, &minutes, &seconds, &millis,
               &consumed) == 4 &&
        text[consumed] == '\0' && minutes >= 0 && minutes <= 59 && seconds >= 0 &&
        seconds <= 60 && millis >= 0 && millis <= 999) {
        colon_sign = hours < 0 ? -1 : 1;
        if (hours < 0) hours = -hours;
        *out = (int64_t)(colon_sign *
                         (hours * 3600000LL + minutes * 60000LL + seconds * 1000LL + millis));
        return 1;
    }
    if (sscanf(text, "%lld:%lld:%lld%n", &hours, &minutes, &seconds, &consumed) == 3 &&
        text[consumed] == '\0' && minutes >= 0 && minutes <= 59 && seconds >= 0 &&
        seconds <= 60) {
        colon_sign = hours < 0 ? -1 : 1;
        if (hours < 0) hours = -hours;
        *out = (int64_t)(colon_sign *
                         (hours * 3600000LL + minutes * 60000LL + seconds * 1000LL));
        return 1;
    }
    p = text;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    while (*p) {
        char *next = NULL;
        double n;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        errno = 0;
        n = strtod(p, &next);
        if (errno != 0 || next == p) return 0;
        p = next;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) {
            total += n;
            saw_value = 1;
            break;
        }
        if (p[0] == 'm' && p[1] == 's') {
            total += n;
            p += 2;
        } else if (*p == 's') {
            total += n * 1000.0;
            p++;
        } else if (*p == 'm') {
            total += n * 60000.0;
            p++;
        } else if (*p == 'h') {
            total += n * 3600000.0;
            p++;
        } else if (*p == 'd') {
            total += n * 86400000.0;
            p++;
        } else {
            return 0;
        }
        saw_value = 1;
    }
    if (!saw_value) return 0;
    *out = (int64_t)(sign * total);
    return 1;
}

static int parser_date_value_text(exprtk_value_t value, char *out, size_t out_size) {
    if (value.type != EXPRTK_VAL_DATE || !out || out_size == 0) return 0;
    return snprintf(out, out_size, "%04d-%02d-%02d", value.data.date.year,
                    value.data.date.month, value.data.date.day) > 0;
}

static int parser_time_value_text(exprtk_value_t value, char *out, size_t out_size) {
    if (value.type != EXPRTK_VAL_TIME || !out || out_size == 0) return 0;
    if (value.data.time.millisecond > 0) {
        return snprintf(out, out_size, "%02d:%02d:%02d.%03d", value.data.time.hour,
                        value.data.time.minute, value.data.time.second,
                        value.data.time.millisecond) > 0;
    }
    return snprintf(out, out_size, "%02d:%02d:%02d", value.data.time.hour,
                    value.data.time.minute, value.data.time.second) > 0;
}

static int parser_duration_value_text(exprtk_value_t value, char *out, size_t out_size) {
    int64_t rem;
    int64_t h;
    int64_t m;
    int64_t s;
    if (value.type != EXPRTK_VAL_DURATION || !out || out_size == 0) return 0;
    rem = value.data.duration_ms < 0 ? -value.data.duration_ms : value.data.duration_ms;
    h = rem / 3600000;
    rem %= 3600000;
    m = rem / 60000;
    rem %= 60000;
    s = rem / 1000;
    rem %= 1000;
    return snprintf(out, out_size, "%s%lld:%02lld:%02lld.%03lld",
                    value.data.duration_ms < 0 ? "-" : "", (long long)h,
                    (long long)m, (long long)s, (long long)rem) > 0;
}

static int parser_decimal_from_text(const char *text, exprtk_decimal_t *out) {
    const char *p;
    int sign = 1;
    int saw_digit = 0;
    int saw_dot = 0;
    int32_t scale = 0;
    uint64_t acc = 0;
    uint64_t limit;

    if (!text || !out) return 0;
    p = text;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    limit = sign < 0 ? (uint64_t)INT64_MAX + 1ULL : (uint64_t)INT64_MAX;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            unsigned digit = (unsigned)(*p - '0');
            if (acc > (limit - digit) / 10ULL) return 0;
            acc = acc * 10ULL + digit;
            saw_digit = 1;
            if (saw_dot) {
                if (scale == INT32_MAX) return 0;
                scale++;
            }
            p++;
            continue;
        }
        if (*p == '.') {
            if (saw_dot) return 0;
            saw_dot = 1;
            p++;
            continue;
        }
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p) break;
        }
        return 0;
    }
    if (!saw_digit) return 0;
    out->mantissa = sign < 0
                        ? (acc == (uint64_t)INT64_MAX + 1ULL ? INT64_MIN : -(int64_t)acc)
                        : (int64_t)acc;
    out->scale = scale;
    while (out->scale > 0 && out->mantissa % 10 == 0) {
        out->mantissa /= 10;
        out->scale--;
    }
    if (out->mantissa == 0) out->scale = 0;
    return 1;
}

static int parser_decimal_value_text(exprtk_value_t value, char *out, size_t out_size) {
    exprtk_decimal_t decimal;
    char digits[32];
    char *p = digits + sizeof(digits);
    uint64_t mag;
    size_t digit_count;
    size_t pos = 0;
    int negative;

    if (!out || out_size == 0) return 0;
    if (value.type == EXPRTK_VAL_DECIMAL) {
        decimal = value.data.decimal;
    } else if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
        char text[128];
        size_t len = value.data.string.len < sizeof(text) - 1 ? value.data.string.len
                                                              : sizeof(text) - 1;
        memcpy(text, value.data.string.data, len);
        text[len] = '\0';
        if (!parser_decimal_from_text(text, &decimal)) return 0;
    } else {
        return 0;
    }
    while (decimal.scale > 0 && decimal.mantissa % 10 == 0) {
        decimal.mantissa /= 10;
        decimal.scale--;
    }
    if (decimal.mantissa == 0) decimal.scale = 0;
    negative = decimal.mantissa < 0;
    mag = negative ? (uint64_t)(-(decimal.mantissa + 1)) + 1ULL
                   : (uint64_t)decimal.mantissa;
    *--p = '\0';
    do {
        *--p = (char)('0' + (mag % 10ULL));
        mag /= 10ULL;
    } while (mag != 0);
    digit_count = strlen(p);
    if (negative) {
        if (pos + 1 >= out_size) return 0;
        out[pos++] = '-';
    }
    if (decimal.scale == 0) {
        if (pos + digit_count >= out_size) return 0;
        memcpy(out + pos, p, digit_count + 1);
        return 1;
    }
    if ((size_t)decimal.scale >= digit_count) {
        size_t zeros = (size_t)decimal.scale - digit_count;
        if (pos + 2 + zeros + digit_count >= out_size) return 0;
        out[pos++] = '0';
        out[pos++] = '.';
        while (zeros-- > 0) out[pos++] = '0';
        memcpy(out + pos, p, digit_count);
        pos += digit_count;
        out[pos] = '\0';
        return 1;
    }
    {
        size_t whole = digit_count - (size_t)decimal.scale;
        if (pos + digit_count + 1 >= out_size) return 0;
        memcpy(out + pos, p, whole);
        pos += whole;
        out[pos++] = '.';
        memcpy(out + pos, p + whole, (size_t)decimal.scale);
        pos += (size_t)decimal.scale;
        out[pos] = '\0';
        return 1;
    }
}

static int parser_datetime_text_valid(const char *text) {
    turbo_datetime_t dt;
    return text && turbo_parse_datetime(text, strlen(text), &dt) == 0;
}

static int parser_date_text_valid(const char *text) {
    exprtk_date_t date;
    return parser_date_from_text(text, &date);
}

static int parser_time_text_valid(const char *text) {
    exprtk_time_t time;
    return parser_time_from_text(text, &time);
}

static int parser_duration_text_valid(const char *text) {
    int64_t ms;
    return parser_duration_from_text(text, &ms);
}

static int parser_decimal_text_valid(const char *text) {
    exprtk_decimal_t decimal;
    return parser_decimal_from_text(text, &decimal);
}

static exprtk_value_t parser_bytes_value(exprtk_env_t *env, const char *data, size_t len) {
    char *copy;
    if (!env || (!data && len > 0)) return PARSER_ZERO;
    copy = (char *)mem_alloc(&env->arena, len + 1);
    if (!copy) return PARSER_ZERO;
    if (len > 0) memcpy(copy, data, len);
    copy[len] = '\0';
    return exprtk_val_bytes(tstr_v_from_buf(copy, len));
}

static exprtk_value_t parser_bind_text_value(parser_bind_kind_t kind, const char *text,
                                             exprtk_env_t *env) {
    char *end = NULL;
    uuid_t uuid;
    turbo_datetime_t dt;
    exprtk_date_t date;
    exprtk_time_t time;
    int64_t duration_ms;
    exprtk_decimal_t decimal;
    errno = 0;
    switch (kind) {
        case PARSER_BIND_STRING:
            return parser_string_value(env, text ? text : "");
        case PARSER_BIND_BYTES:
            return parser_bytes_value(env, text ? text : "", text ? strlen(text) : 0);
        case PARSER_BIND_UUID:
            if (text && uuid_from_s(text, &uuid)) return exprtk_val_uuid(uuid);
            return parser_null();
        case PARSER_BIND_DATETIME:
            if (text && turbo_parse_datetime(text, strlen(text), &dt) == 0)
                return exprtk_val_datetime(dt);
            return parser_null();
        case PARSER_BIND_DATE:
            if (parser_date_from_text(text, &date)) return exprtk_val_date(date);
            return parser_null();
        case PARSER_BIND_TIME:
            if (parser_time_from_text(text, &time)) return exprtk_val_time(time);
            return parser_null();
        case PARSER_BIND_DURATION:
            if (parser_duration_from_text(text, &duration_ms))
                return exprtk_val_duration(duration_ms);
            return parser_null();
        case PARSER_BIND_DECIMAL:
            if (parser_decimal_from_text(text, &decimal))
                return exprtk_val_decimal(decimal);
            return parser_null();
        case PARSER_BIND_BIGINT:
            return parser_string_value(env, text ? text : "");
        case PARSER_BIND_MONEY:
            return parser_string_value(env, text ? text : "");
        case PARSER_BIND_BOOL:
            return exprtk_val_int(parser_parse_bool_text(text));
        case PARSER_BIND_INTEGER: {
            long long value = text ? strtoll(text, &end, 10) : 0;
            if (errno != 0 || !end || *end != '\0') return exprtk_val_int(0);
            return exprtk_val_int((int64_t)value);
        }
        case PARSER_BIND_NUMBER: {
            double value = text ? strtod(text, &end) : 0.0;
            if (errno != 0 || !end || *end != '\0') return exprtk_val_num(0.0);
            return exprtk_val_num(value);
        }
        default:
            return parser_null();
    }
}

static const char *parser_enum_item_value(Node *root, const char *type_name, const char *item_name) {
    Node *e = parser_find_enum(root, type_name);
    Node *items = parser_node_child(e, "items");
    if (!items || items->type != NODE_LIST || !item_name) return NULL;
    for (size_t j = 0; j < items->data.list.count; ++j) {
        Node *item = items->data.list.items[j];
        const char *candidate = parser_node_string(item, "name");
        if (candidate && strcmp(candidate, item_name) == 0)
            return parser_node_string(item, "value");
    }
    return NULL;
}

static int parser_parse_i64_text(const char *text, int64_t *out) {
    char *end = NULL;
    long long value;

    if (!text || !out) return 0;
    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || !end || end == text || *end != '\0') return 0;
    *out = (int64_t)value;
    return 1;
}

static int parser_enum_text_value(Node *root, const char *type_name,
                                  const char *text, int64_t *out) {
    const char *value_text;

    if (parser_parse_i64_text(text, out)) return 1;
    value_text = parser_enum_item_value(root, type_name, text);
    return value_text ? parser_parse_i64_text(value_text, out) : 0;
}

static exprtk_value_t parser_enum_expr_value(Node *root, const char *type_name,
                                             const char *symbol, int64_t value,
                                             int is_flags, exprtk_env_t *env) {
    exprtk_value_t type_text = parser_string_value(env, type_name ? type_name : "");
    exprtk_value_t symbol_text = parser_string_value(env, symbol ? symbol : "");
    return exprtk_val_enum(type_text.data.string, symbol_text.data.string, value, is_flags);
}

static int parser_flags_text_value(Node *root, const char *type_name,
                                   const char *text, int64_t *out) {
    const char *p = text;
    int64_t acc = 0;
    int any = 0;

    if (!text || !out) return 0;
    if (parser_parse_i64_text(text, out)) return 1;

    while (*p) {
        char token[128];
        size_t len = 0;
        int64_t item_value = 0;

        while (*p == ' ' || *p == '\t' || *p == '|' || *p == ',' || *p == '+') ++p;
        if (!*p) break;
        while (*p && *p != '|' && *p != ',' && *p != '+' &&
               *p != ' ' && *p != '\t' && len + 1 < sizeof(token)) {
            token[len++] = *p++;
        }
        token[len] = '\0';
        if (len == 0 || !parser_enum_text_value(root, type_name, token, &item_value))
            return 0;
        acc |= item_value;
        any = 1;
    }

    if (!any) return 0;
    *out = acc;
    return 1;
}

static int parser_schema_text_value(Node *root, const char *type_name,
                                    parser_bind_kind_t kind, const char *text,
                                    int64_t *integer_out) {
    if (parser_is_flags_type(root, type_name))
        return parser_flags_text_value(root, type_name, text, integer_out);
    if (parser_is_enum_type(root, type_name))
        return parser_enum_text_value(root, type_name, text, integer_out);
    if (kind == PARSER_BIND_INTEGER)
        return parser_parse_i64_text(text, integer_out);
    return 0;
}

static int parser_schema_text_value_valid(Node *root, const char *type_name,
                                          parser_bind_kind_t kind, const char *text);

static exprtk_value_t parser_bind_schema_text_value(Node *root, const char *type_name,
                                                    parser_bind_kind_t kind,
                                                    const char *text, exprtk_env_t *env) {
    int64_t integer_value = 0;
    if ((parser_is_enum_type(root, type_name) || kind == PARSER_BIND_INTEGER) &&
        parser_schema_text_value(root, type_name, kind, text, &integer_value))
        return exprtk_val_int(integer_value);
    return parser_bind_text_value(kind, text, env);
}

static int parser_bind_schema_text_value_checked(Node *root, const char *type_name,
                                                 parser_bind_kind_t kind,
                                                 const char *text, exprtk_env_t *env,
                                                 exprtk_value_t *out) {
    if (!out || kind == PARSER_BIND_UNSUPPORTED) return 0;
    if (!parser_schema_text_value_valid(root, type_name, kind, text)) return 0;
    *out = parser_bind_schema_text_value(root, type_name, kind, text, env);
    return out->type != EXPRTK_VAL_NULL;
}

static exprtk_value_t parser_field_default_value(Node *schema_root, Node *field, exprtk_env_t *env) {
    const char *default_text = parser_node_string(field, "default_value");
    const char *type = parser_node_string(field, "type");
    parser_bind_kind_t kind;
    const char *enum_value;

    if (!default_text) return parser_null();
    kind = parser_field_kind(schema_root, field);
    if (kind == PARSER_BIND_UNSUPPORTED) return parser_null();
    if (kind == PARSER_BIND_INTEGER) {
        if (strcmp(default_text, "true") == 0) return exprtk_val_int(1);
        if (strcmp(default_text, "false") == 0) return exprtk_val_int(0);
    }
    enum_value = parser_enum_item_value(schema_root, type, default_text);
    if (enum_value) default_text = enum_value;
    return parser_bind_schema_text_value(schema_root, type, kind, default_text, env);
}

static exprtk_value_t parser_bind_json_value(parser_bind_kind_t kind, json_value_t *value,
                                             exprtk_env_t *env) {
    if (!value) return parser_null();
    switch (kind) {
        case PARSER_BIND_STRING:
            if (turbo_json_type(value) == TURBO_JSON_STRING)
                return parser_string_value(env, turbo_json_string(value));
            if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", turbo_json_number(value));
                return parser_string_value(env, buf);
            }
            if (turbo_json_type(value) == TURBO_JSON_BOOL)
                return parser_string_value(env, turbo_json_bool(value) ? "true" : "false");
            return parser_string_value(env, "");
        case PARSER_BIND_BYTES:
            if (turbo_json_type(value) == TURBO_JSON_STRING)
                return parser_bytes_value(env, turbo_json_string(value), turbo_json_string_len(value));
            return parser_null();
        case PARSER_BIND_BOOL:
            if (turbo_json_type(value) == TURBO_JSON_BOOL) return exprtk_val_int(turbo_json_bool(value));
            if (turbo_json_type(value) == TURBO_JSON_NUMBER) return exprtk_val_int(turbo_json_number(value) != 0.0);
            if (turbo_json_type(value) == TURBO_JSON_STRING) return exprtk_val_int(parser_parse_bool_text(turbo_json_string(value)));
            return exprtk_val_int(0);
        case PARSER_BIND_INTEGER:
            if (turbo_json_type(value) == TURBO_JSON_NUMBER) return exprtk_val_int((int64_t)turbo_json_number(value));
            if (turbo_json_type(value) == TURBO_JSON_BOOL) return exprtk_val_int(turbo_json_bool(value));
            if (turbo_json_type(value) == TURBO_JSON_STRING) return parser_bind_text_value(kind, turbo_json_string(value), env);
            return exprtk_val_int(0);
        case PARSER_BIND_NUMBER:
            if (turbo_json_type(value) == TURBO_JSON_NUMBER) return exprtk_val_num(turbo_json_number(value));
            if (turbo_json_type(value) == TURBO_JSON_BOOL) return exprtk_val_num(turbo_json_bool(value) ? 1.0 : 0.0);
            if (turbo_json_type(value) == TURBO_JSON_STRING) return parser_bind_text_value(kind, turbo_json_string(value), env);
            return exprtk_val_num(0.0);
        case PARSER_BIND_DATETIME:
            if (turbo_json_type(value) == TURBO_JSON_STRING)
                return parser_bind_text_value(kind, turbo_json_string(value), env);
            return parser_null();
        case PARSER_BIND_DATE:
        case PARSER_BIND_TIME:
            if (turbo_json_type(value) == TURBO_JSON_STRING)
                return parser_bind_text_value(kind, turbo_json_string(value), env);
            return parser_null();
        case PARSER_BIND_DURATION:
            if (turbo_json_type(value) == TURBO_JSON_NUMBER)
                return exprtk_val_duration((int64_t)turbo_json_number(value));
            if (turbo_json_type(value) == TURBO_JSON_STRING)
                return parser_bind_text_value(kind, turbo_json_string(value), env);
            return parser_null();
        case PARSER_BIND_DECIMAL:
            if (turbo_json_type(value) == TURBO_JSON_STRING)
                return parser_bind_text_value(kind, turbo_json_string(value), env);
            if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", turbo_json_number(value));
                return parser_bind_text_value(kind, buf, env);
            }
            return parser_null();
        default:
            return parser_null();
    }
}

static int parser_json_integer_value(json_value_t *value, int64_t *out) {
    if (!value || !out) return 0;
    if (turbo_json_type(value) == TURBO_JSON_NUMBER) {
        *out = (int64_t)turbo_json_number(value);
        return 1;
    }
    if (turbo_json_type(value) == TURBO_JSON_BOOL) {
        *out = turbo_json_bool(value) ? 1 : 0;
        return 1;
    }
    return 0;
}

static int parser_json_enum_value(Node *root, const char *type_name,
                                  json_value_t *value, int64_t *out) {
    if (parser_json_integer_value(value, out)) return 1;
    if (value && turbo_json_type(value) == TURBO_JSON_STRING)
        return parser_enum_text_value(root, type_name, turbo_json_string(value), out);
    return 0;
}

static int parser_json_flags_value(Node *root, const char *type_name,
                                   json_value_t *value, int64_t *out) {
    int64_t acc = 0;

    if (parser_json_integer_value(value, out)) return 1;
    if (!value) return 0;
    if (turbo_json_type(value) == TURBO_JSON_STRING)
        return parser_flags_text_value(root, type_name, turbo_json_string(value), out);
    if (turbo_json_type(value) != TURBO_JSON_ARRAY) return 0;
    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        int64_t item_value = 0;
        if (!parser_json_flags_value(root, type_name, turbo_json_array_get(value, i), &item_value))
            return 0;
        acc |= item_value;
    }
    *out = acc;
    return 1;
}

static int parser_json_schema_scalar_valid(Node *root, const char *type_name,
                                           parser_bind_kind_t kind, json_value_t *value);

static exprtk_value_t parser_bind_json_schema_value(Node *root, const char *type_name,
                                                    parser_bind_kind_t kind, json_value_t *value,
                                                    exprtk_env_t *env) {
    int64_t integer_value = 0;
    if (!parser_json_schema_scalar_valid(root, type_name, kind, value))
        return parser_null();
    if (parser_is_flags_type(root, type_name) &&
        parser_json_flags_value(root, type_name, value, &integer_value)) {
        const char *symbol = turbo_json_type(value) == TURBO_JSON_STRING ? turbo_json_string(value) : "";
        return parser_enum_expr_value(root, type_name, symbol, integer_value, 1, env);
    }
    if (parser_is_enum_type(root, type_name) &&
        parser_json_enum_value(root, type_name, value, &integer_value)) {
        const char *symbol = turbo_json_type(value) == TURBO_JSON_STRING ? turbo_json_string(value) : "";
        return parser_enum_expr_value(root, type_name, symbol, integer_value, 0, env);
    }
    return parser_bind_json_value(kind, value, env);
}

static exprtk_value_t parser_bind_json_object(Node *schema_root, Node *record, json_value_t *object,
                                              exprtk_env_t *env);
static exprtk_value_t parser_bind_json_typed_value(Node *schema_root, const char *type_name,
                                                   json_value_t *value, exprtk_env_t *env);

static Node *parser_union_variant(Node *union_node, const char *variant_name) {
    Node *fields = parser_node_child(union_node, "fields");
    if (!fields || fields->type != NODE_LIST || !variant_name) return NULL;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        if (name && strcmp(name, variant_name) == 0) return field;
    }
    return NULL;
}

static int parser_node_size_value(Node *node, const char *name, size_t *out);

static exprtk_value_t parser_bind_json_array(Node *schema_root, Node *field, json_value_t *value,
                                             exprtk_env_t *env) {
    const char *collection_kind = parser_node_string(field, "collection_kind");
    exprtk_value_t result =
        collection_kind && strcmp(collection_kind, "set") == 0 ? exprtk_val_set_empty()
                                                               : exprtk_val_list_empty();
    const char *inner_type = parser_node_string(field, "inner_type");
    parser_bind_kind_t scalar_kind;
    size_t expected = 0;

    if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY || !inner_type) return parser_null();
    if (parser_node_size_value(field, "length_field", &expected) &&
        turbo_json_array_size(value) != expected) {
        return parser_null();
    }

    scalar_kind = parser_type_kind(schema_root, inner_type);
    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        json_value_t *item = turbo_json_array_get(value, i);
        exprtk_value_t bound;
        if (!item) continue;
        if (parser_node_has(field, "collection_element_is_composite") ||
            parser_find_union(schema_root, inner_type)) {
            bound = parser_bind_json_typed_value(schema_root, inner_type, item, env);
        } else if (scalar_kind != PARSER_BIND_UNSUPPORTED) {
            bound = parser_bind_json_schema_value(schema_root, inner_type, scalar_kind, item, env);
        } else {
            bound = parser_null();
        }
        if (bound.type == EXPRTK_VAL_NULL) return parser_null();
        exprtk_list_push(&result, bound);
    }
    return result;
}

static exprtk_value_t parser_bind_json_record_array(Node *schema_root, const char *type_name,
                                                    json_value_t *value, exprtk_env_t *env) {
    exprtk_value_t result = exprtk_val_list_empty();
    if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY || !type_name) return parser_null();

    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        json_value_t *item = turbo_json_array_get(value, i);
        exprtk_value_t bound = parser_bind_json_typed_value(schema_root, type_name, item, env);
        if (bound.type == EXPRTK_VAL_NULL) return parser_null();
        exprtk_list_push(&result, bound);
    }
    return result;
}

static exprtk_value_t parser_bind_json_map(Node *schema_root, Node *field, json_value_t *value,
                                           exprtk_env_t *env) {
    exprtk_value_t result = exprtk_val_object();
    const char *value_type = parser_node_string(field, "value_type");
    parser_bind_kind_t value_kind = parser_type_kind(schema_root, value_type);

    if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT || !value_type) return parser_null();

    for (size_t i = 0; i < turbo_json_object_size(value); ++i) {
        const char *key = turbo_json_object_key(value, i);
        json_value_t *item = turbo_json_object_value(value, i);
        exprtk_value_t bound;
        if (!key || !item) continue;
        if (parser_find_record(schema_root, value_type) || parser_find_union(schema_root, value_type)) {
            bound = parser_bind_json_typed_value(schema_root, value_type, item, env);
        } else if (value_kind != PARSER_BIND_UNSUPPORTED) {
            bound = parser_bind_json_schema_value(schema_root, value_type, value_kind, item, env);
        } else {
            bound = parser_null();
        }
        if (bound.type == EXPRTK_VAL_NULL) return parser_null();
        exprtk_map_set(&result, key, bound);
    }
    return result;
}

static exprtk_value_t parser_bind_json_union(Node *schema_root, Node *union_node,
                                             json_value_t *object, exprtk_env_t *env) {
    exprtk_value_t result = exprtk_val_object();
    const char *variant_name;
    json_value_t *payload;
    Node *variant;
    const char *variant_type;
    parser_bind_kind_t scalar_kind;
    exprtk_value_t bound;

    if (!union_node || !object || turbo_json_type(object) != TURBO_JSON_OBJECT ||
        turbo_json_object_size(object) != 1) {
        return parser_null();
    }

    variant_name = turbo_json_object_key(object, 0);
    payload = turbo_json_object_value(object, 0);
    variant = parser_union_variant(union_node, variant_name);
    variant_type = parser_node_string(variant, "type");
    if (!variant || !variant_type || !payload) return parser_null();

    if (parser_find_record(schema_root, variant_type) || parser_find_union(schema_root, variant_type)) {
        bound = parser_bind_json_typed_value(schema_root, variant_type, payload, env);
    } else {
        scalar_kind = parser_type_kind(schema_root, variant_type);
        bound = scalar_kind != PARSER_BIND_UNSUPPORTED
                    ? parser_bind_json_schema_value(schema_root, variant_type, scalar_kind, payload, env)
                    : parser_null();
    }
    if (bound.type == EXPRTK_VAL_NULL) return parser_null();
    exprtk_map_set(&result, variant_name, bound);
    return result;
}

static exprtk_value_t parser_bind_json_typed_value(Node *schema_root, const char *type_name,
                                                   json_value_t *value, exprtk_env_t *env) {
    Node *record = parser_find_record(schema_root, type_name);
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    if (record && value && turbo_json_type(value) == TURBO_JSON_OBJECT)
        return parser_bind_json_object(schema_root, record, value, env);
    union_node = parser_find_union(schema_root, type_name);
    if (union_node)
        return parser_bind_json_union(schema_root, union_node, value, env);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (scalar_kind != PARSER_BIND_UNSUPPORTED)
        return parser_bind_json_schema_value(schema_root, type_name, scalar_kind, value, env);
    return parser_null();
}

static exprtk_value_t parser_bind_json_object(Node *schema_root, Node *record, json_value_t *object,
                                              exprtk_env_t *env) {
    exprtk_value_t result = exprtk_val_object();
    Node *fields = parser_node_child(record, "fields");
    if (!fields || fields->type != NODE_LIST || !object || turbo_json_type(object) != TURBO_JSON_OBJECT)
        return result;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        const char *field_type = parser_node_string(field, "type");
        parser_bind_kind_t kind = parser_field_kind(schema_root, field);
        json_value_t *value;
        exprtk_value_t bound;
        if (!name) continue;
        value = turbo_json_object_get(object, name);
        if (!value) {
            bound = parser_field_default_value(schema_root, field, env);
            if (bound.type != EXPRTK_VAL_NULL) exprtk_map_set(&result, name, bound);
            else if (!parser_field_missing_allowed(field)) return parser_null();
            continue;
        }

        if (parser_node_has(field, "is_group_field")) {
            const char *group_type = parser_node_string(field, "group_type");
            bound = parser_bind_json_record_array(schema_root, group_type, value, env);
        } else if (parser_node_has(field, "is_map")) {
            bound = parser_bind_json_map(schema_root, field, value, env);
        } else if (parser_node_has(field, "is_collection")) {
            bound = parser_bind_json_array(schema_root, field, value, env);
        } else if (parser_node_has(field, "is_composite_ref") && field_type) {
            bound = parser_bind_json_typed_value(schema_root, field_type, value, env);
        } else if (field_type && parser_find_union(schema_root, field_type)) {
            bound = parser_bind_json_typed_value(schema_root, field_type, value, env);
        } else if (kind != PARSER_BIND_UNSUPPORTED) {
            bound = parser_bind_json_schema_value(schema_root, field_type, kind, value, env);
        } else {
            continue;
        }
        if (bound.type == EXPRTK_VAL_NULL) return parser_null();
        exprtk_map_set(&result, name, bound);
    }
    return result;
}

static int parser_node_size_value(Node *node, const char *name, size_t *out) {
    const char *text = parser_node_string(node, name);
    char *end = NULL;
    unsigned long value;

    if (!text || !out) return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return 0;
    *out = (size_t)value;
    return 1;
}

static int parser_csv_join_path(char *out, size_t out_size, const char *prefix, const char *name) {
    int written;
    if (!out || out_size == 0 || !name) return 0;
    if (prefix && prefix[0] != '\0')
        written = snprintf(out, out_size, "%s.%s", prefix, name);
    else
        written = snprintf(out, out_size, "%s", name);
    return written > 0 && (size_t)written < out_size;
}

static int parser_csv_index_path(char *out, size_t out_size, const char *prefix, size_t index) {
    int written;
    if (!out || out_size == 0 || !prefix) return 0;
    written = snprintf(out, out_size, "%s[%zu]", prefix, index);
    return written > 0 && (size_t)written < out_size;
}

static int parser_bind_csv_scalar_at_path(Node *schema_root, turbo_csv_doc_t *doc, size_t row,
                                          const char *path, const char *type_name,
                                          parser_bind_kind_t kind,
                                          exprtk_env_t *env, exprtk_value_t *out) {
    size_t col = 0;
    const char *text;

    if (!doc || !path || kind == PARSER_BIND_UNSUPPORTED || !out) return 0;
    if (!parser_csv_find_path_column(doc, path, &col)) return 0;
    text = turbo_csv_get(doc, row, col);
    if (!text) return 0;
    return parser_bind_schema_text_value_checked(schema_root, type_name, kind, text, env, out);
}

static int parser_csv_find_value_column(turbo_csv_doc_t *doc, size_t *out_col) {
    if (!doc || !out_col) return 0;
    if (parser_csv_find_path_column(doc, "value", out_col)) return 1;
    *out_col = 0;
    return 1;
}

static int parser_bind_csv_scalar_value(Node *schema_root, turbo_csv_doc_t *doc, size_t row,
                                        const char *type_name, parser_bind_kind_t kind,
                                        exprtk_env_t *env, exprtk_value_t *out) {
    size_t col = 0;
    const char *text;

    if (!doc || !type_name || kind == PARSER_BIND_UNSUPPORTED || !out ||
        row >= turbo_csv_row_count(doc) || !parser_csv_find_value_column(doc, &col)) {
        return 0;
    }
    text = turbo_csv_get(doc, row, col);
    if (!text) return 0;
    return parser_bind_schema_text_value_checked(schema_root, type_name, kind, text, env, out);
}

static int parser_bind_csv_record_at_path(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                          size_t row, exprtk_env_t *env,
                                          const parser_csv_headers_t *headers,
                                          const char *prefix, exprtk_value_t *out);
static int parser_bind_csv_union_at_path(Node *schema_root, Node *union_node, turbo_csv_doc_t *doc,
                                         size_t row, exprtk_env_t *env,
                                         const parser_csv_headers_t *headers,
                                         const char *path, exprtk_value_t *out);

static int parser_bind_csv_map_at_path(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                       size_t row, exprtk_env_t *env,
                                       const parser_csv_headers_t *headers,
                                       const char *path, exprtk_value_t *out) {
    const char *value_type = parser_node_string(field, "value_type");
    parser_bind_kind_t value_kind = parser_type_kind(schema_root, value_type);
    Node *value_record = parser_find_record(schema_root, value_type);
    Node *value_union = parser_find_union(schema_root, value_type);
    exprtk_value_t result = exprtk_val_object();
    int any = 0;

    if (!value_type || !headers || !path || !out) return 0;
    for (size_t i = 0; i < headers->count; ++i) {
        char key[128];
        char item_path[256];
        exprtk_value_t item = parser_null();
        int ok = 0;

        if (!parser_csv_header_map_key(headers->names[i], path, key, sizeof(key))) continue;
        if (parser_csv_map_key_seen(&result, key)) continue;
        if (!parser_csv_join_path(item_path, sizeof(item_path), path, key)) return 0;

        if (value_record) {
            ok = parser_bind_csv_record_at_path(schema_root, value_record, doc, row, env,
                                                headers, item_path, &item);
        } else if (value_union) {
            ok = parser_bind_csv_union_at_path(schema_root, value_union, doc, row, env,
                                               headers, item_path, &item);
        } else if (value_kind != PARSER_BIND_UNSUPPORTED) {
            ok = parser_bind_csv_scalar_at_path(schema_root, doc, row, item_path, value_type,
                                                value_kind, env, &item);
        }
        if (ok) {
            exprtk_map_set(&result, key, item);
            any = 1;
        } else if (parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) {
            return 0;
        }
    }

    if (!any) return 0;
    *out = result;
    return 1;
}

static int parser_bind_csv_group_at_path(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                         size_t row, exprtk_env_t *env,
                                         const parser_csv_headers_t *headers,
                                         const char *path, exprtk_value_t *out) {
    const char *group_type = parser_node_string(field, "group_type");
    Node *group_record = parser_find_record(schema_root, group_type);
    parser_index_list_t indexes = {0};
    exprtk_value_t result = exprtk_val_list_empty();

    if (!group_record || !headers || !path || !out) return 0;
    if (!parser_csv_collect_group_indexes(headers, path, &indexes)) {
        parser_index_list_free(&indexes);
        return 0;
    }

    for (size_t i = 0; i < indexes.count; ++i) {
        char item_path[256];
        exprtk_value_t item = parser_null();

        if (!parser_csv_index_path(item_path, sizeof(item_path), path, indexes.values[i])) {
            parser_index_list_free(&indexes);
            return 0;
        }
        if (parser_bind_csv_record_at_path(schema_root, group_record, doc, row, env,
                                           headers, item_path, &item)) {
            exprtk_list_push(&result, item);
        } else if (parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) {
            parser_index_list_free(&indexes);
            return 0;
        }
    }

    parser_index_list_free(&indexes);
    *out = result;
    return result.type == EXPRTK_VAL_LIST && result.data.list.count > 0;
}

static int parser_bind_csv_union_at_path(Node *schema_root, Node *union_node, turbo_csv_doc_t *doc,
                                         size_t row, exprtk_env_t *env,
                                         const parser_csv_headers_t *headers,
                                         const char *path, exprtk_value_t *out) {
    Node *fields = parser_node_child(union_node, "fields");
    exprtk_value_t result = exprtk_val_object();
    int matches = 0;

    if (!fields || fields->type != NODE_LIST || !headers || !path || !out) return 0;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        parser_bind_kind_t scalar_kind;
        char item_path[256];
        exprtk_value_t item = parser_null();
        int ok = 0;

        if (!name || !variant_type ||
            !parser_csv_join_path(item_path, sizeof(item_path), path, name)) return 0;
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
        if (variant_record) {
            ok = parser_bind_csv_record_at_path(schema_root, variant_record, doc, row, env,
                                                headers, item_path, &item);
        } else if (variant_union) {
            ok = parser_bind_csv_union_at_path(schema_root, variant_union, doc, row, env,
                                               headers, item_path, &item);
        } else {
            scalar_kind = parser_type_kind(schema_root, variant_type);
            if (scalar_kind != PARSER_BIND_UNSUPPORTED)
                ok = parser_bind_csv_scalar_at_path(schema_root, doc, row, item_path, variant_type,
                                                    scalar_kind, env, &item);
        }
        if (ok) {
            exprtk_map_set(&result, name, item);
            matches++;
        } else {
            return 0;
        }
    }

    if (matches != 1) return 0;
    *out = result;
    return 1;
}

static int parser_bind_csv_field_at_path(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                         size_t row, exprtk_env_t *env,
                                         const parser_csv_headers_t *headers,
                                         const char *path, exprtk_value_t *out) {
    const char *field_type = parser_node_string(field, "type");

    if (!field || !path || !out) return 0;

    if (parser_node_has(field, "is_group_field"))
        return parser_bind_csv_group_at_path(schema_root, field, doc, row, env, headers, path, out);

    if (parser_node_has(field, "is_map"))
        return parser_bind_csv_map_at_path(schema_root, field, doc, row, env, headers, path, out);

    if (parser_node_has(field, "is_collection") && !parser_node_has(field, "is_map")) {
        const char *collection_kind = parser_node_string(field, "collection_kind");
        const char *inner_type = parser_node_string(field, "inner_type");
        parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);
        size_t count = 0;
        int fixed_count = 0;
        parser_index_list_t indexes = {0};
        exprtk_value_t list =
            collection_kind && strcmp(collection_kind, "set") == 0 ? exprtk_val_set_empty()
                                                                   : exprtk_val_list_empty();
        int any = 0;

        if (!inner_type) return 0;
        fixed_count = parser_node_size_value(field, "length_field", &count);
        if (fixed_count) {
            for (size_t i = 0; i < count; ++i)
                if (!parser_index_list_push(&indexes, i)) {
                    parser_index_list_free(&indexes);
                    return 0;
                }
        } else if (!parser_csv_collect_group_indexes(headers, path, &indexes)) {
            parser_index_list_free(&indexes);
            return 0;
        }
        for (size_t i = 0; i < indexes.count; ++i) {
            char item_path[256];
            exprtk_value_t item = parser_null();
            int ok = 0;

            if (!parser_csv_index_path(item_path, sizeof(item_path), path, indexes.values[i])) {
                parser_index_list_free(&indexes);
                return 0;
            }
            if (parser_node_has(field, "collection_element_is_composite")) {
                Node *inner_record = parser_find_record(schema_root, inner_type);
                ok = parser_bind_csv_record_at_path(schema_root, inner_record, doc, row, env,
                                                    headers, item_path, &item);
            } else if (parser_find_union(schema_root, inner_type)) {
                ok = parser_bind_csv_union_at_path(schema_root, parser_find_union(schema_root, inner_type),
                                                   doc, row, env, headers, item_path, &item);
            } else if (scalar_kind != PARSER_BIND_UNSUPPORTED) {
                ok = parser_bind_csv_scalar_at_path(schema_root, doc, row, item_path, inner_type,
                                                    scalar_kind, env, &item);
            }
            if (ok) {
                exprtk_list_push(&list, item);
                any = 1;
            } else if (fixed_count ||
                       parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) {
                parser_index_list_free(&indexes);
                return 0;
            }
        }
        parser_index_list_free(&indexes);
        if (!any) return 0;
        *out = list;
        return 1;
    }

    if (parser_node_has(field, "is_composite_ref") && field_type) {
        Node *inner_record = parser_find_record(schema_root, field_type);
        if (inner_record)
            return parser_bind_csv_record_at_path(schema_root, inner_record, doc, row, env,
                                                  headers, path, out);
        return 0;
    }
    if (field_type && parser_find_union(schema_root, field_type))
        return parser_bind_csv_union_at_path(schema_root, parser_find_union(schema_root, field_type),
                                             doc, row, env, headers, path, out);

    return parser_bind_csv_scalar_at_path(schema_root, doc, row, path,
                                          field_type, parser_field_kind(schema_root, field), env, out);
}

static int parser_value_is_sequence(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_SET;
}

static size_t parser_value_sequence_count(exprtk_value_t value) {
    return parser_value_is_sequence(value) ? value.data.list.count : 0;
}

static exprtk_value_t parser_value_sequence_item(exprtk_value_t value, size_t index) {
    return parser_value_is_sequence(value) && index < value.data.list.count
               ? value.data.list.items[index]
               : parser_null();
}

static int parser_bind_csv_record_at_path(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                          size_t row, exprtk_env_t *env,
                                          const parser_csv_headers_t *headers,
                                          const char *prefix, exprtk_value_t *out) {
    exprtk_value_t result = exprtk_val_object();
    Node *fields = parser_node_child(record, "fields");
    int input_present;

    if (!fields || fields->type != NODE_LIST || !doc || row >= turbo_csv_row_count(doc) || !out)
        return 0;
    input_present = (!prefix || prefix[0] == '\0') ? 1 : parser_csv_headers_have_path(headers, prefix);
    if (!input_present) return 0;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        char path[256];
        exprtk_value_t bound;

        if (!name || !parser_csv_join_path(path, sizeof(path), prefix, name)) continue;
        if (!parser_bind_csv_field_at_path(schema_root, field, doc, row, env, headers, path, &bound)) {
            bound = parser_field_default_value(schema_root, field, env);
            if (bound.type == EXPRTK_VAL_NULL) {
                if (parser_field_missing_allowed(field) &&
                    !parser_csv_row_has_nonempty_path(doc, row, headers, path)) {
                    continue;
                }
                return 0;
            }
        }
        exprtk_map_set(&result, name, bound);
    }

    *out = result;
    return 1;
}

static exprtk_value_t parser_bind_csv_row(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                          size_t row, exprtk_env_t *env,
                                          const parser_csv_headers_t *headers) {
    exprtk_value_t result = parser_null();
    if (parser_bind_csv_record_at_path(schema_root, record, doc, row, env, headers, "", &result))
        return result;
    return parser_null();
}

static int parser_arg_int(exprtk_value_t value, int *out) {
    if (!out) return 0;
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

static exprtk_value_t parser_json_bind_doc_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       json_value_t *json,
                                                       exprtk_value_t type_arg) {
    exprtk_value_t result = parser_null();
    char *type_name;

    if (!ud || !ud->env || !schema_root || !json || type_arg.type != EXPRTK_VAL_STRING)
        return result;
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name || !parser_type_supported(schema_root, type_name)) return result;
    return parser_bind_json_typed_value(schema_root, type_name, json, ud->env);
}

static exprtk_value_t parser_json_bind_all_doc_with_schema(parser_ud_t *ud, Node *schema_root,
                                                           json_value_t *json,
                                                           exprtk_value_t type_arg) {
    exprtk_value_t result = exprtk_val_list_empty();
    char *type_name;

    if (!ud || !ud->env || !schema_root || !json || type_arg.type != EXPRTK_VAL_STRING)
        return result;
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name || !parser_type_supported(schema_root, type_name)) return result;

    if (turbo_json_type(json) == TURBO_JSON_ARRAY) {
        size_t count = turbo_json_array_size(json);
        for (size_t i = 0; i < count; ++i) {
            exprtk_value_t bound =
                parser_bind_json_typed_value(schema_root, type_name, turbo_json_array_get(json, i),
                                             ud->env);
            if (bound.type != EXPRTK_VAL_NULL) exprtk_list_push(&result, bound);
        }
    } else {
        exprtk_value_t bound = parser_bind_json_typed_value(schema_root, type_name, json, ud->env);
        if (bound.type != EXPRTK_VAL_NULL) exprtk_list_push(&result, bound);
    }
    return result;
}

static exprtk_value_t parser_csv_bind_doc_with_schema(parser_ud_t *ud, Node *schema_root,
                                                      turbo_csv_doc_t *doc,
                                                      const parser_csv_headers_t *headers,
                                                      exprtk_value_t row_arg,
                                                      exprtk_value_t type_arg) {
    Node *record;
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    exprtk_value_t result = parser_null();
    char *type_name;
    int row;

    if (!ud || !ud->env || !schema_root || !doc || !headers ||
        type_arg.type != EXPRTK_VAL_STRING || !parser_arg_int(row_arg, &row) || row < 0) {
        return result;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    record = parser_find_record(schema_root, type_name);
    union_node = parser_find_union(schema_root, type_name);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) return result;

    if (record) {
        result = parser_bind_csv_row(schema_root, record, doc, (size_t)row, ud->env, headers);
    } else if (union_node) {
        (void)parser_bind_csv_union_at_path(schema_root, union_node, doc, (size_t)row,
                                            ud->env, headers, "", &result);
    } else {
        (void)parser_bind_csv_scalar_value(schema_root, doc, (size_t)row, type_name,
                                           scalar_kind, ud->env, &result);
    }
    return result;
}

static exprtk_value_t parser_csv_bind_all_doc_with_schema(parser_ud_t *ud, Node *schema_root,
                                                          turbo_csv_doc_t *doc,
                                                          const parser_csv_headers_t *headers,
                                                          exprtk_value_t type_arg) {
    Node *record;
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    exprtk_value_t result = exprtk_val_list_empty();
    char *type_name;

    if (!ud || !ud->env || !schema_root || !doc || !headers ||
        type_arg.type != EXPRTK_VAL_STRING) {
        return result;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    record = parser_find_record(schema_root, type_name);
    union_node = parser_find_union(schema_root, type_name);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) return result;

    for (size_t i = 0; i < turbo_csv_row_count(doc); ++i) {
        if (record) {
            exprtk_value_t item = parser_bind_csv_row(schema_root, record, doc, i, ud->env, headers);
            if (item.type != EXPRTK_VAL_NULL) exprtk_list_push(&result, item);
        } else {
            exprtk_value_t item = parser_null();
            if (union_node) {
                if (parser_bind_csv_union_at_path(schema_root, union_node, doc, i, ud->env,
                                                  headers, "", &item))
                    exprtk_list_push(&result, item);
            } else if (parser_bind_csv_scalar_value(schema_root, doc, i, type_name,
                                                    scalar_kind, ud->env, &item)) {
                exprtk_list_push(&result, item);
            }
        }
    }
    return result;
}

static exprtk_value_t parser_json_bind_text_with_schema(parser_ud_t *ud, Node *schema_root,
                                                        exprtk_value_t json_arg,
                                                        exprtk_value_t type_arg) {
    json_value_t *json = NULL;
    exprtk_value_t result = parser_null();
    void *ptr;

    if (!ud || !schema_root || json_arg.type != EXPRTK_VAL_STRING) return result;
    if (turbo_parse_json((const uint8_t *)json_arg.data.string.data,
                         json_arg.data.string.len, &json) != 0 || !json) {
        return result;
    }
    result = parser_json_bind_doc_with_schema(ud, schema_root, json, type_arg);
    ptr = json;
    turbo_free_json(&ptr);
    return result;
}

static exprtk_value_t parser_json_bind_all_text_with_schema(parser_ud_t *ud, Node *schema_root,
                                                            exprtk_value_t json_arg,
                                                            exprtk_value_t type_arg) {
    json_value_t *json = NULL;
    exprtk_value_t result = exprtk_val_list_empty();
    void *ptr;

    if (!ud || !schema_root || json_arg.type != EXPRTK_VAL_STRING) return result;
    if (turbo_parse_json((const uint8_t *)json_arg.data.string.data,
                         json_arg.data.string.len, &json) != 0 || !json) {
        return result;
    }
    result = parser_json_bind_all_doc_with_schema(ud, schema_root, json, type_arg);
    ptr = json;
    turbo_free_json(&ptr);
    return result;
}

static exprtk_value_t parser_csv_bind_text_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       exprtk_value_t csv_arg,
                                                       exprtk_value_t row_arg,
                                                       exprtk_value_t type_arg) {
    turbo_csv_doc_t *doc = NULL;
    parser_csv_headers_t headers = {0};
    turbo_csv_options_t opts = {true, ',', '"', true};
    exprtk_value_t result = parser_null();
    void *ptr;

    if (!ud || !schema_root || csv_arg.type != EXPRTK_VAL_STRING) return result;
    if (turbo_parse_csv_opts((const uint8_t *)csv_arg.data.string.data,
                             csv_arg.data.string.len, &opts, &doc) != 0 || !doc) {
        return result;
    }
    if (parser_csv_parse_header_names(csv_arg, &headers)) {
        result = parser_csv_bind_doc_with_schema(ud, schema_root, doc, &headers,
                                                 row_arg, type_arg);
    }
    parser_csv_headers_free(&headers);
    ptr = doc;
    turbo_free_csv(&ptr);
    return result;
}

static exprtk_value_t parser_csv_bind_all_text_with_schema(parser_ud_t *ud, Node *schema_root,
                                                           exprtk_value_t csv_arg,
                                                           exprtk_value_t type_arg) {
    turbo_csv_doc_t *doc = NULL;
    parser_csv_headers_t headers = {0};
    turbo_csv_options_t opts = {true, ',', '"', true};
    exprtk_value_t result = exprtk_val_list_empty();
    void *ptr;

    if (!ud || !schema_root || csv_arg.type != EXPRTK_VAL_STRING) return result;
    if (turbo_parse_csv_opts((const uint8_t *)csv_arg.data.string.data,
                             csv_arg.data.string.len, &opts, &doc) != 0 || !doc) {
        return result;
    }
    if (parser_csv_parse_header_names(csv_arg, &headers)) {
        result = parser_csv_bind_all_doc_with_schema(ud, schema_root, doc, &headers,
                                                     type_arg);
    }
    parser_csv_headers_free(&headers);
    ptr = doc;
    turbo_free_csv(&ptr);
    return result;
}

static int parser_value_as_number(exprtk_value_t value, double *out) {
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_INTEGER) {
        *out = (double)value.data.integer;
        return 1;
    }
    if (value.type == EXPRTK_VAL_NUMBER) {
        *out = value.data.number;
        return 1;
    }
    return 0;
}

static int parser_json_emit_string(parser_filter_buf_t *out, const char *text, size_t len) {
    if (!parser_buf_append_char(out, '"')) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        switch (ch) {
            case '"':
                if (!parser_buf_append(out, "\\\"")) return 0;
                break;
            case '\\':
                if (!parser_buf_append(out, "\\\\")) return 0;
                break;
            case '\b':
                if (!parser_buf_append(out, "\\b")) return 0;
                break;
            case '\f':
                if (!parser_buf_append(out, "\\f")) return 0;
                break;
            case '\n':
                if (!parser_buf_append(out, "\\n")) return 0;
                break;
            case '\r':
                if (!parser_buf_append(out, "\\r")) return 0;
                break;
            case '\t':
                if (!parser_buf_append(out, "\\t")) return 0;
                break;
            default:
                if (ch < 0x20) {
                    if (!parser_buf_append_fmt(out, "\\u%04x", ch)) return 0;
                } else if (!parser_buf_append_char(out, (char)ch)) {
                    return 0;
                }
                break;
        }
    }
    return parser_buf_append_char(out, '"');
}

static int parser_json_emit_cstr(parser_filter_buf_t *out, const char *text) {
    return parser_json_emit_string(out, text ? text : "", text ? strlen(text) : 0);
}

static int parser_json_emit_scalar(parser_filter_buf_t *out, parser_bind_kind_t kind,
                                   exprtk_value_t value) {
    double n;
    char uuid_text[UUID4_STR_BUFFER_SIZE];
    char datetime_text[64];
    char temporal_text[64];
    char decimal_text[64];

    switch (kind) {
        case PARSER_BIND_UUID:
            if (value.type == EXPRTK_VAL_UUID &&
                uuid_to_s(value.data.uuid, uuid_text, sizeof(uuid_text))) {
                return parser_json_emit_cstr(out, uuid_text);
            }
            if (value.type == EXPRTK_VAL_STRING &&
                value.data.string.len == UUID4_STR_BUFFER_SIZE - 1) {
                char text[UUID4_STR_BUFFER_SIZE];
                uuid_t parsed;
                memcpy(text, value.data.string.data, value.data.string.len);
                text[value.data.string.len] = '\0';
                if (uuid_from_s(text, &parsed)) return parser_json_emit_cstr(out, text);
            }
            return parser_buf_append(out, "null");
        case PARSER_BIND_DATETIME:
            if (parser_datetime_value_text(value, datetime_text, sizeof(datetime_text)))
                return parser_json_emit_cstr(out, datetime_text);
            return parser_buf_append(out, "null");
        case PARSER_BIND_DATE:
            if (parser_date_value_text(value, temporal_text, sizeof(temporal_text)))
                return parser_json_emit_cstr(out, temporal_text);
            return parser_buf_append(out, "null");
        case PARSER_BIND_TIME:
            if (parser_time_value_text(value, temporal_text, sizeof(temporal_text)))
                return parser_json_emit_cstr(out, temporal_text);
            return parser_buf_append(out, "null");
        case PARSER_BIND_DURATION:
            if (parser_duration_value_text(value, temporal_text, sizeof(temporal_text)))
                return parser_json_emit_cstr(out, temporal_text);
            if (value.type == EXPRTK_VAL_INTEGER &&
                parser_duration_value_text(exprtk_val_duration(value.data.integer),
                                           temporal_text, sizeof(temporal_text)))
                return parser_json_emit_cstr(out, temporal_text);
            if (value.type == EXPRTK_VAL_NUMBER &&
                parser_duration_value_text(exprtk_val_duration((int64_t)value.data.number),
                                           temporal_text, sizeof(temporal_text)))
                return parser_json_emit_cstr(out, temporal_text);
            return parser_buf_append(out, "null");
        case PARSER_BIND_DECIMAL:
            if (parser_decimal_value_text(value, decimal_text, sizeof(decimal_text)))
                return parser_json_emit_cstr(out, decimal_text);
            return parser_buf_append(out, "null");
        case PARSER_BIND_BIGINT:
            if (value.type == EXPRTK_VAL_BIGINT)
                return parser_json_emit_string(out, value.data.bigint.text.data, value.data.bigint.text.len);
            if (value.type == EXPRTK_VAL_STRING)
                return parser_json_emit_string(out, value.data.string.data, value.data.string.len);
            if (value.type == EXPRTK_VAL_INTEGER)
                return parser_buf_append_fmt(out, "%lld", (long long)value.data.integer);
            return parser_buf_append(out, "null");
        case PARSER_BIND_MONEY:
            if (value.type == EXPRTK_VAL_MONEY) {
                char money_text[96];
                char amount_text[64];
                exprtk_decimal_t dec = value.data.money.amount;
                if (parser_decimal_value_text(exprtk_val_decimal(dec), amount_text, sizeof(amount_text))) {
                    snprintf(money_text, sizeof(money_text), "%c%c%c %s",
                             value.data.money.currency[0], value.data.money.currency[1],
                             value.data.money.currency[2], amount_text);
                    return parser_json_emit_cstr(out, money_text);
                }
            }
            return parser_buf_append(out, "null");
        case PARSER_BIND_BYTES:
            if (value.type == EXPRTK_VAL_BYTES)
                return parser_json_emit_string(out, value.data.bytes.data, value.data.bytes.len);
            if (value.type == EXPRTK_VAL_STRING)
                return parser_json_emit_string(out, value.data.string.data, value.data.string.len);
            return parser_buf_append(out, "null");
        case PARSER_BIND_STRING:
            if (value.type == EXPRTK_VAL_STRING)
                return parser_json_emit_string(out, value.data.string.data, value.data.string.len);
            if (value.type == EXPRTK_VAL_UUID && uuid_to_s(value.data.uuid, uuid_text, sizeof(uuid_text)))
                return parser_json_emit_cstr(out, uuid_text);
            if (parser_value_as_number(value, &n))
                return parser_buf_append_fmt(out, "%.17g", n);
            return parser_json_emit_cstr(out, "");
        case PARSER_BIND_BOOL:
            if (parser_value_as_number(value, &n))
                return parser_buf_append(out, n != 0.0 ? "true" : "false");
            return parser_buf_append(out, "false");
        case PARSER_BIND_INTEGER:
            if (value.type == EXPRTK_VAL_INTEGER)
                return parser_buf_append_fmt(out, "%lld", (long long)value.data.integer);
            if (value.type == EXPRTK_VAL_NUMBER)
                return parser_buf_append_fmt(out, "%.0f", value.data.number);
            return parser_buf_append(out, "0");
        case PARSER_BIND_NUMBER:
            if (parser_value_as_number(value, &n))
                return parser_buf_append_fmt(out, "%.17g", n);
            return parser_buf_append(out, "0");
        default:
            return parser_buf_append(out, "null");
    }
}

static exprtk_value_t parser_json_value_to_expr(exprtk_env_t *env, json_value_t *value) {
    size_t i;

    if (!value) return parser_null();
    switch (turbo_json_type(value)) {
        case TURBO_JSON_NULL:
            return parser_null();
        case TURBO_JSON_BOOL:
            return exprtk_val_bool(turbo_json_bool(value));
        case TURBO_JSON_NUMBER:
            return exprtk_val_num(turbo_json_number(value));
        case TURBO_JSON_STRING:
            return parser_string_value_len(env, turbo_json_string(value), turbo_json_string_len(value));
        case TURBO_JSON_ARRAY: {
            exprtk_value_t list = exprtk_val_list_empty();
            for (i = 0; i < turbo_json_array_size(value); ++i)
                exprtk_list_push(&list, parser_json_value_to_expr(env, turbo_json_array_get(value, i)));
            return list;
        }
        case TURBO_JSON_OBJECT: {
            exprtk_value_t map = exprtk_val_object();
            for (i = 0; i < turbo_json_object_size(value); ++i) {
                const char *key = turbo_json_object_key(value, i);
                exprtk_map_set(&map, key ? key : "",
                               parser_json_value_to_expr(env, turbo_json_object_value(value, i)));
            }
            return map;
        }
        default:
            return parser_null();
    }
}

static int parser_json_emit_expr_value(parser_filter_buf_t *out, exprtk_value_t value, int depth) {
    exprtk_map_iter_t it;
    const char *key;
    exprtk_value_t item;
    int first = 1;

    if (depth > 64) return parser_buf_append(out, "null");
    switch (value.type) {
        case EXPRTK_VAL_NULL:
            return parser_buf_append(out, "null");
        case EXPRTK_VAL_BOOL:
            return parser_buf_append(out, value.data.boolean ? "true" : "false");
        case EXPRTK_VAL_STRING:
            return parser_json_emit_string(out, value.data.string.data, value.data.string.len);
        case EXPRTK_VAL_BYTES:
            if (!parser_buf_append_char(out, '[')) return 0;
            for (size_t i = 0; i < value.data.bytes.len; ++i) {
                if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
                if (!parser_buf_append_fmt(out, "%u", (unsigned int)(unsigned char)value.data.bytes.data[i])) return 0;
            }
            return parser_buf_append_char(out, ']');
        case EXPRTK_VAL_UUID: {
            char uuid_text[UUID4_STR_BUFFER_SIZE];
            if (!uuid_to_s(value.data.uuid, uuid_text, sizeof(uuid_text))) return parser_buf_append(out, "null");
            return parser_json_emit_cstr(out, uuid_text);
        }
        case EXPRTK_VAL_DATETIME: {
            char datetime_text[64];
            if (!parser_datetime_value_text(value, datetime_text, sizeof(datetime_text)))
                return parser_buf_append(out, "null");
            return parser_json_emit_cstr(out, datetime_text);
        }
        case EXPRTK_VAL_OFFSET_DATETIME: {
            int offset = value.data.offset_datetime.offset_minutes;
            char sign = '+';
            char datetime_text[64];
            if (offset < 0) {
                sign = '-';
                offset = -offset;
            }
            snprintf(datetime_text, sizeof(datetime_text),
                     "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                     value.data.offset_datetime.datetime.year,
                     value.data.offset_datetime.datetime.month,
                     value.data.offset_datetime.datetime.day,
                     value.data.offset_datetime.datetime.hour,
                     value.data.offset_datetime.datetime.minute,
                     value.data.offset_datetime.datetime.second,
                     sign, offset / 60, offset % 60);
            return parser_json_emit_cstr(out, datetime_text);
        }
        case EXPRTK_VAL_DATE: {
            char date_text[32];
            if (!parser_date_value_text(value, date_text, sizeof(date_text)))
                return parser_buf_append(out, "null");
            return parser_json_emit_cstr(out, date_text);
        }
        case EXPRTK_VAL_TIME: {
            char time_text[32];
            if (!parser_time_value_text(value, time_text, sizeof(time_text)))
                return parser_buf_append(out, "null");
            return parser_json_emit_cstr(out, time_text);
        }
        case EXPRTK_VAL_DURATION: {
            char duration_text[64];
            if (!parser_duration_value_text(value, duration_text, sizeof(duration_text)))
                return parser_buf_append(out, "null");
            return parser_json_emit_cstr(out, duration_text);
        }
        case EXPRTK_VAL_DECIMAL: {
            char decimal_text[64];
            if (!parser_decimal_value_text(value, decimal_text, sizeof(decimal_text)))
                return parser_buf_append(out, "null");
            return parser_json_emit_cstr(out, decimal_text);
        }
        case EXPRTK_VAL_BIGINT:
            return parser_json_emit_string(out, value.data.bigint.text.data, value.data.bigint.text.len);
        case EXPRTK_VAL_MONEY: {
            char amount_text[64];
            if (!parser_buf_append_char(out, '{')) return 0;
            if (!parser_json_emit_cstr(out, "amount")) return 0;
            if (!parser_buf_append_char(out, ':')) return 0;
            if (!parser_decimal_value_text(exprtk_val_decimal(value.data.money.amount),
                                           amount_text, sizeof(amount_text)) ||
                !parser_json_emit_cstr(out, amount_text)) return 0;
            if (!parser_buf_append_char(out, ',')) return 0;
            if (!parser_json_emit_cstr(out, "currency")) return 0;
            if (!parser_buf_append_char(out, ':')) return 0;
            if (!parser_json_emit_string(out, value.data.money.currency, 3)) return 0;
            return parser_buf_append_char(out, '}');
        }
        case EXPRTK_VAL_ENUM:
        case EXPRTK_VAL_FLAGS:
            if (value.data.enum_val.symbol.data && value.data.enum_val.symbol.len > 0)
                return parser_json_emit_string(out, value.data.enum_val.symbol.data,
                                               value.data.enum_val.symbol.len);
            return parser_buf_append_fmt(out, "%lld", (long long)value.data.enum_val.value);
        case EXPRTK_VAL_INTEGER:
            return parser_buf_append_fmt(out, "%lld", (long long)value.data.integer);
        case EXPRTK_VAL_NUMBER:
            return parser_buf_append_fmt(out, "%.17g", value.data.number);
        case EXPRTK_VAL_VECTOR:
            if (!parser_buf_append_char(out, '[')) return 0;
            for (size_t i = 0; i < value.data.vector.size; ++i) {
                if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
                if (!parser_buf_append_fmt(out, "%.17g", value.data.vector.data[i])) return 0;
            }
            return parser_buf_append_char(out, ']');
        case EXPRTK_VAL_TYPED_ARRAY:
            if (!parser_buf_append_char(out, '[')) return 0;
            for (size_t i = 0; i < value.data.typed_array.count; ++i) {
                exprtk_value_t array_item = exprtk_typed_array_get_value(value, i);
                if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
                if (!parser_json_emit_expr_value(out, array_item, depth + 1)) return 0;
            }
            return parser_buf_append_char(out, ']');
        case EXPRTK_VAL_LIST:
        case EXPRTK_VAL_SET:
            if (!parser_buf_append_char(out, '[')) return 0;
            for (size_t i = 0; i < value.data.list.count; ++i) {
                if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
                if (!parser_json_emit_expr_value(out, value.data.list.items[i], depth + 1)) return 0;
            }
            return parser_buf_append_char(out, ']');
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT:
            if (!parser_buf_append_char(out, '{')) return 0;
            it = exprtk_map_iter_begin(&value);
            while (exprtk_map_iter_next(&it, &key, &item)) {
                if (!first && !parser_buf_append_char(out, ',')) return 0;
                first = 0;
                if (!parser_json_emit_cstr(out, key ? key : "")) return 0;
                if (!parser_buf_append_char(out, ':')) return 0;
                if (!parser_json_emit_expr_value(out, item, depth + 1)) return 0;
            }
            return parser_buf_append_char(out, '}');
        default:
            return parser_buf_append(out, "null");
    }
}

static int parser_json_emit_record(Node *schema_root, Node *record, exprtk_value_t value,
                                   parser_filter_buf_t *out);
static int parser_json_emit_typed(Node *schema_root, const char *type_name, exprtk_value_t value,
                                  parser_filter_buf_t *out);
static int parser_json_emit_any_value(Node *schema_root, const char *type_name,
                                      exprtk_value_t value, parser_filter_buf_t *out);

static int parser_json_emit_array(Node *schema_root, Node *field, exprtk_value_t value,
                                  parser_filter_buf_t *out) {
    const char *inner_type = parser_node_string(field, "inner_type");
    parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);

    if (!inner_type) return parser_buf_append(out, "[]");
    if (!parser_buf_append_char(out, '[')) return 0;
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_SET) {
        for (size_t i = 0; i < value.data.list.count; ++i) {
            if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
            if (parser_node_has(field, "collection_element_is_composite") ||
                parser_find_union(schema_root, inner_type)) {
                if (!parser_json_emit_typed(schema_root, inner_type, value.data.list.items[i], out))
                    return 0;
            } else if (!parser_json_emit_scalar(out, scalar_kind, value.data.list.items[i])) {
                return 0;
            }
        }
    } else if (value.type == EXPRTK_VAL_VECTOR) {
        for (size_t i = 0; i < value.data.vector.size; ++i) {
            if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
            if (!parser_buf_append_fmt(out, "%.17g", value.data.vector.data[i])) return 0;
        }
    }
    return parser_buf_append_char(out, ']');
}

static int parser_json_emit_group(Node *schema_root, const char *group_type, exprtk_value_t value,
                                  parser_filter_buf_t *out) {
    if (!parser_buf_append_char(out, '[')) return 0;
    if (value.type == EXPRTK_VAL_LIST) {
        for (size_t i = 0; i < value.data.list.count; ++i) {
            if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
            if (!parser_json_emit_typed(schema_root, group_type, value.data.list.items[i], out))
                return 0;
        }
    }
    return parser_buf_append_char(out, ']');
}

static int parser_json_emit_map(Node *schema_root, Node *field, exprtk_value_t value,
                                parser_filter_buf_t *out) {
    const char *value_type = parser_node_string(field, "value_type");
    parser_bind_kind_t value_kind = parser_type_kind(schema_root, value_type);
    exprtk_map_iter_t it;
    const char *key;
    exprtk_value_t item;
    int first = 1;

    if (!parser_buf_append_char(out, '{')) return 0;
    if (exprtk_value_is_object_like(&value) && value_type) {
        it = exprtk_map_iter_begin(&value);
        while (exprtk_map_iter_next(&it, &key, &item)) {
            if (!first && !parser_buf_append_char(out, ',')) return 0;
            first = 0;
            if (!parser_json_emit_cstr(out, key)) return 0;
            if (!parser_buf_append_char(out, ':')) return 0;
            if (parser_find_record(schema_root, value_type) || parser_find_union(schema_root, value_type)) {
                if (!parser_json_emit_typed(schema_root, value_type, item, out)) return 0;
            } else if (!parser_json_emit_scalar(out, value_kind, item)) {
                return 0;
            }
        }
    }
    return parser_buf_append_char(out, '}');
}

static int parser_json_emit_typed(Node *schema_root, const char *type_name, exprtk_value_t value,
                                  parser_filter_buf_t *out) {
    Node *record = parser_find_record(schema_root, type_name);
    Node *union_node = parser_find_union(schema_root, type_name);
    parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, type_name);
    Node *fields;
    const char *selected_name = NULL;
    const char *selected_type = NULL;
    exprtk_value_t selected_value = parser_null();
    int matches = 0;

    if (record) return parser_json_emit_record(schema_root, record, value, out);
    if (!union_node && scalar_kind != PARSER_BIND_UNSUPPORTED)
        return parser_json_emit_scalar(out, scalar_kind, value);
    if (!union_node) return parser_buf_append(out, "null");
    if (!exprtk_value_is_object_like(&value)) return parser_buf_append(out, "{}");

    fields = parser_node_child(union_node, "fields");
    if (!fields || fields->type != NODE_LIST) return parser_buf_append(out, "{}");
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        if (!name || !exprtk_map_has(&value, name)) continue;
        selected_name = name;
        selected_type = parser_node_string(field, "type");
        selected_value = exprtk_map_get(&value, name);
        matches++;
    }
    if (matches != 1 || !selected_name || !selected_type) return parser_buf_append(out, "{}");
    if (!parser_buf_append_char(out, '{')) return 0;
    if (!parser_json_emit_cstr(out, selected_name)) return 0;
    if (!parser_buf_append_char(out, ':')) return 0;
    if (parser_find_record(schema_root, selected_type) || parser_find_union(schema_root, selected_type)) {
        if (!parser_json_emit_typed(schema_root, selected_type, selected_value, out)) return 0;
    } else if (!parser_json_emit_scalar(out, parser_type_kind(schema_root, selected_type), selected_value)) {
        return 0;
    }
    return parser_buf_append_char(out, '}');
}

static int parser_json_emit_record(Node *schema_root, Node *record, exprtk_value_t value,
                                   parser_filter_buf_t *out) {
    Node *fields = parser_node_child(record, "fields");
    int first = 1;

    if (!fields || fields->type != NODE_LIST || !exprtk_value_is_object_like(&value))
        return parser_buf_append(out, "{}");
    if (!parser_buf_append_char(out, '{')) return 0;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        const char *field_type = parser_node_string(field, "type");
        exprtk_value_t field_value;

        if (!name || !exprtk_map_has(&value, name)) continue;
        field_value = exprtk_map_get(&value, name);
        if (!first && !parser_buf_append_char(out, ',')) return 0;
        first = 0;
        if (!parser_json_emit_cstr(out, name)) return 0;
        if (!parser_buf_append_char(out, ':')) return 0;

        if (parser_node_has(field, "is_group_field")) {
            if (!parser_json_emit_group(schema_root, parser_node_string(field, "group_type"),
                                        field_value, out)) return 0;
        } else if (parser_node_has(field, "is_map")) {
            if (!parser_json_emit_map(schema_root, field, field_value, out)) return 0;
        } else if (parser_node_has(field, "is_collection")) {
            if (!parser_json_emit_array(schema_root, field, field_value, out)) return 0;
        } else if (parser_node_has(field, "is_composite_ref") && field_type) {
            if (!parser_json_emit_typed(schema_root, field_type, field_value, out)) return 0;
        } else if (field_type && parser_find_union(schema_root, field_type)) {
            if (!parser_json_emit_typed(schema_root, field_type, field_value, out)) return 0;
        } else if (!parser_json_emit_scalar(out, parser_field_kind(schema_root, field), field_value)) {
            return 0;
        }
    }

    return parser_buf_append_char(out, '}');
}

static int parser_json_emit_value(Node *schema_root, Node *record, exprtk_value_t value,
                                  parser_filter_buf_t *out) {
    if (value.type == EXPRTK_VAL_LIST) {
        if (!parser_buf_append_char(out, '[')) return 0;
        for (size_t i = 0; i < value.data.list.count; ++i) {
            if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
            if (!parser_json_emit_record(schema_root, record, value.data.list.items[i], out))
                return 0;
        }
        return parser_buf_append_char(out, ']');
    }
    return parser_json_emit_record(schema_root, record, value, out);
}

static int parser_json_emit_any_value(Node *schema_root, const char *type_name,
                                      exprtk_value_t value, parser_filter_buf_t *out) {
    if (value.type == EXPRTK_VAL_LIST) {
        if (!parser_buf_append_char(out, '[')) return 0;
        for (size_t i = 0; i < value.data.list.count; ++i) {
            if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
            if (!parser_json_emit_typed(schema_root, type_name, value.data.list.items[i], out))
                return 0;
        }
        return parser_buf_append_char(out, ']');
    }
    return parser_json_emit_typed(schema_root, type_name, value, out);
}

static exprtk_value_t parser_json_emit_with_schema(parser_ud_t *ud, Node *schema_root,
                                                   exprtk_value_t value_arg,
                                                   exprtk_value_t type_arg) {
    parser_filter_buf_t out = {0};
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    char *type_name;

    if (!ud || !ud->env || !schema_root || type_arg.type != EXPRTK_VAL_STRING)
        return result;
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    if (!parser_type_supported(schema_root, type_name))
        return result;

    if (parser_json_emit_any_value(schema_root, type_name, value_arg, &out))
        result = parser_buf_to_string(&out, ud->env);
    free(out.data);
    return result;
}

static int parser_csv_emit_cell(parser_filter_buf_t *out, exprtk_value_t value) {
    char num_buf[64];
    char uuid_buf[UUID4_STR_BUFFER_SIZE];
    char datetime_buf[64];
    char temporal_buf[64];
    const char *text = "";
    size_t len = 0;
    int needs_quote = 0;

    if (value.type == EXPRTK_VAL_STRING) {
        text = value.data.string.data ? value.data.string.data : "";
        len = value.data.string.len;
    } else if (value.type == EXPRTK_VAL_BYTES) {
        text = value.data.bytes.data ? value.data.bytes.data : "";
        len = value.data.bytes.len;
    } else if (value.type == EXPRTK_VAL_INTEGER) {
        snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value.data.integer);
        text = num_buf;
        len = strlen(num_buf);
    } else if (value.type == EXPRTK_VAL_NUMBER) {
        snprintf(num_buf, sizeof(num_buf), "%.17g", value.data.number);
        text = num_buf;
        len = strlen(num_buf);
    } else if (value.type == EXPRTK_VAL_BOOL) {
        text = value.data.boolean ? "true" : "false";
        len = strlen(text);
    } else if (value.type == EXPRTK_VAL_UUID &&
               uuid_to_s(value.data.uuid, uuid_buf, sizeof(uuid_buf))) {
        text = uuid_buf;
        len = strlen(uuid_buf);
    } else if (value.type == EXPRTK_VAL_DATETIME &&
               parser_datetime_value_text(value, datetime_buf, sizeof(datetime_buf))) {
        text = datetime_buf;
        len = strlen(datetime_buf);
    } else if (value.type == EXPRTK_VAL_OFFSET_DATETIME) {
        int offset = value.data.offset_datetime.offset_minutes;
        char sign = '+';
        if (offset < 0) {
            sign = '-';
            offset = -offset;
        }
        snprintf(datetime_buf, sizeof(datetime_buf),
                 "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                 value.data.offset_datetime.datetime.year,
                 value.data.offset_datetime.datetime.month,
                 value.data.offset_datetime.datetime.day,
                 value.data.offset_datetime.datetime.hour,
                 value.data.offset_datetime.datetime.minute,
                 value.data.offset_datetime.datetime.second,
                 sign, offset / 60, offset % 60);
        text = datetime_buf;
        len = strlen(datetime_buf);
    } else if (value.type == EXPRTK_VAL_DATE &&
               parser_date_value_text(value, temporal_buf, sizeof(temporal_buf))) {
        text = temporal_buf;
        len = strlen(temporal_buf);
    } else if (value.type == EXPRTK_VAL_TIME &&
               parser_time_value_text(value, temporal_buf, sizeof(temporal_buf))) {
        text = temporal_buf;
        len = strlen(temporal_buf);
    } else if (value.type == EXPRTK_VAL_DURATION &&
               parser_duration_value_text(value, temporal_buf, sizeof(temporal_buf))) {
        text = temporal_buf;
        len = strlen(temporal_buf);
    } else if (value.type == EXPRTK_VAL_DECIMAL &&
               parser_decimal_value_text(value, temporal_buf, sizeof(temporal_buf))) {
        text = temporal_buf;
        len = strlen(temporal_buf);
    } else if (value.type == EXPRTK_VAL_BIGINT) {
        text = value.data.bigint.text.data ? value.data.bigint.text.data : "";
        len = value.data.bigint.text.len;
    } else if (value.type == EXPRTK_VAL_MONEY) {
        char amount_text[64];
        if (parser_decimal_value_text(exprtk_val_decimal(value.data.money.amount),
                                      amount_text, sizeof(amount_text))) {
            snprintf(temporal_buf, sizeof(temporal_buf), "%c%c%c %s",
                     value.data.money.currency[0], value.data.money.currency[1],
                     value.data.money.currency[2], amount_text);
            text = temporal_buf;
            len = strlen(temporal_buf);
        }
    } else if (value.type == EXPRTK_VAL_ENUM || value.type == EXPRTK_VAL_FLAGS) {
        if (value.data.enum_val.symbol.data && value.data.enum_val.symbol.len > 0) {
            text = value.data.enum_val.symbol.data;
            len = value.data.enum_val.symbol.len;
        } else {
            snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value.data.enum_val.value);
            text = num_buf;
            len = strlen(num_buf);
        }
    }

    for (size_t i = 0; i < len; ++i) {
        if (text[i] == ',' || text[i] == '"' || text[i] == '\n' || text[i] == '\r') {
            needs_quote = 1;
            break;
        }
    }
    if (!needs_quote) return parser_buf_append_len(out, text, len);
    if (!parser_buf_append_char(out, '"')) return 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '"' && !parser_buf_append_char(out, '"')) return 0;
        if (!parser_buf_append_char(out, text[i])) return 0;
    }
    return parser_buf_append_char(out, '"');
}

static int parser_csv_emit_headers_for_record(Node *schema_root, Node *record,
                                              const char *prefix, exprtk_value_t value,
                                              parser_filter_buf_t *out, int *first);
static int parser_csv_emit_values_for_record(Node *schema_root, Node *record, exprtk_value_t value,
                                             parser_filter_buf_t *out, int *first);
static int parser_csv_emit_headers_for_union(Node *schema_root, Node *union_node,
                                             const char *path, exprtk_value_t value,
                                             parser_filter_buf_t *out, int *first);
static int parser_csv_emit_values_for_union(Node *schema_root, Node *union_node,
                                            exprtk_value_t value,
                                            parser_filter_buf_t *out, int *first);
static int parser_csv_emit_headers_for_union_schema(Node *schema_root, Node *union_node,
                                                    const char *path,
                                                    parser_filter_buf_t *out, int *first);
static int parser_csv_emit_values_for_union_schema(Node *schema_root, Node *union_node,
                                                   exprtk_value_t value,
                                                   parser_filter_buf_t *out, int *first);

static int parser_csv_emit_separator(parser_filter_buf_t *out, int *first) {
    if (!first) return 0;
    if (*first) {
        *first = 0;
        return 1;
    }
    return parser_buf_append_char(out, ',');
}

static int parser_csv_emit_headers_for_field(Node *schema_root, Node *field, const char *path,
                                             exprtk_value_t value, parser_filter_buf_t *out,
                                             int *first) {
    const char *field_type = parser_node_string(field, "type");

    if (parser_node_has(field, "is_group_field")) {
        const char *group_type = parser_node_string(field, "group_type");
        Node *group_record = parser_find_record(schema_root, group_type);

        if (!group_record || !parser_value_is_sequence(value)) return 1;
        for (size_t i = 0; i < value.data.list.count; ++i) {
            char item_path[256];
            if (!parser_csv_index_path(item_path, sizeof(item_path), path, i)) return 0;
            if (!parser_csv_emit_headers_for_record(schema_root, group_record, item_path,
                                                    value.data.list.items[i], out, first))
                return 0;
        }
        return 1;
    }

    if (parser_node_has(field, "is_map")) {
        const char *value_type = parser_node_string(field, "value_type");
        Node *value_record = parser_find_record(schema_root, value_type);
        Node *value_union = parser_find_union(schema_root, value_type);
        exprtk_map_iter_t it;
        const char *key;
        exprtk_value_t item;

        if (!exprtk_value_is_object_like(&value) || !value_type) return 1;
        it = exprtk_map_iter_begin(&value);
        while (exprtk_map_iter_next(&it, &key, &item)) {
            char item_path[256];
            if (!parser_csv_join_path(item_path, sizeof(item_path), path, key)) return 0;
            if (value_record) {
                if (!parser_csv_emit_headers_for_record(schema_root, value_record, item_path,
                                                        item, out, first)) return 0;
            } else if (value_union) {
                if (!parser_csv_emit_headers_for_union(schema_root, value_union, item_path,
                                                       item, out, first)) return 0;
            } else {
                if (!parser_csv_emit_separator(out, first)) return 0;
                if (!parser_buf_append(out, item_path)) return 0;
            }
        }
        return 1;
    }

    if (parser_node_has(field, "is_collection") && !parser_node_has(field, "is_map")) {
        const char *inner_type = parser_node_string(field, "inner_type");
        size_t count = 0;
        if (!inner_type) return 1;
        if (!parser_node_size_value(field, "length_field", &count))
            count = parser_value_sequence_count(value);
        for (size_t i = 0; i < count; ++i) {
            char item_path[256];
            if (!parser_csv_index_path(item_path, sizeof(item_path), path, i)) return 0;
            if (parser_node_has(field, "collection_element_is_composite")) {
                Node *inner_record = parser_find_record(schema_root, inner_type);
                if (!parser_csv_emit_headers_for_record(schema_root, inner_record, item_path,
                                                        parser_null(), out, first))
                    return 0;
            } else if (parser_find_union(schema_root, inner_type)) {
                exprtk_value_t item = parser_value_sequence_item(value, i);
                if (!parser_csv_emit_headers_for_union(schema_root,
                                                       parser_find_union(schema_root, inner_type),
                                                       item_path, item, out, first))
                    return 0;
            } else {
                if (!parser_csv_emit_separator(out, first)) return 0;
                if (!parser_buf_append(out, item_path)) return 0;
            }
        }
        return 1;
    }

    if (parser_node_has(field, "is_composite_ref") && field_type) {
        Node *inner_record = parser_find_record(schema_root, field_type);
        if (inner_record)
            return parser_csv_emit_headers_for_record(schema_root, inner_record, path, value, out, first);
        return 1;
    }
    if (field_type && parser_find_union(schema_root, field_type))
        return parser_csv_emit_headers_for_union(schema_root, parser_find_union(schema_root, field_type),
                                                 path, value, out, first);

    if (parser_field_kind(schema_root, field) == PARSER_BIND_UNSUPPORTED) return 1;
    if (!parser_csv_emit_separator(out, first)) return 0;
    return parser_buf_append(out, path);
}

static int parser_csv_emit_headers_for_record(Node *schema_root, Node *record,
                                              const char *prefix, exprtk_value_t value,
                                              parser_filter_buf_t *out, int *first) {
    Node *fields = parser_node_child(record, "fields");
    if (!fields || fields->type != NODE_LIST) return 1;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        char path[256];
        exprtk_value_t field_value = parser_null();
        if (!name || !parser_csv_join_path(path, sizeof(path), prefix, name)) continue;
        if (exprtk_value_is_object_like(&value) && exprtk_map_has(&value, name))
            field_value = exprtk_map_get(&value, name);
        if (!parser_csv_emit_headers_for_field(schema_root, field, path, field_value, out, first))
            return 0;
    }
    return 1;
}

static int parser_csv_emit_headers_for_union(Node *schema_root, Node *union_node,
                                             const char *path, exprtk_value_t value,
                                             parser_filter_buf_t *out, int *first) {
    Node *fields = parser_node_child(union_node, "fields");

    if (!fields || fields->type != NODE_LIST || !exprtk_value_is_object_like(&value)) return 1;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        char item_path[256];
        exprtk_value_t item;

        if (!name || !variant_type || !exprtk_map_has(&value, name)) continue;
        if (!parser_csv_join_path(item_path, sizeof(item_path), path, name)) return 0;
        item = exprtk_map_get(&value, name);
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (variant_record) {
            if (!parser_csv_emit_headers_for_record(schema_root, variant_record, item_path,
                                                    item, out, first)) return 0;
        } else if (variant_union) {
            if (!parser_csv_emit_headers_for_union(schema_root, variant_union, item_path,
                                                   item, out, first)) return 0;
        } else if (parser_type_kind(schema_root, variant_type) != PARSER_BIND_UNSUPPORTED) {
            if (!parser_csv_emit_separator(out, first)) return 0;
            if (!parser_buf_append(out, item_path)) return 0;
        }
        return 1;
    }
    return 1;
}

static int parser_csv_emit_headers_for_union_schema(Node *schema_root, Node *union_node,
                                                    const char *path,
                                                    parser_filter_buf_t *out, int *first) {
    Node *fields = parser_node_child(union_node, "fields");

    if (!fields || fields->type != NODE_LIST) return 1;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        char item_path[256];

        if (!name || !variant_type ||
            !parser_csv_join_path(item_path, sizeof(item_path), path, name)) return 0;
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (variant_record) {
            if (!parser_csv_emit_headers_for_record(schema_root, variant_record, item_path,
                                                    parser_null(), out, first)) return 0;
        } else if (variant_union) {
            if (!parser_csv_emit_headers_for_union_schema(schema_root, variant_union, item_path,
                                                          out, first)) return 0;
        } else if (parser_type_kind(schema_root, variant_type) != PARSER_BIND_UNSUPPORTED) {
            if (!parser_csv_emit_separator(out, first)) return 0;
            if (!parser_buf_append(out, item_path)) return 0;
        }
    }
    return 1;
}

static int parser_csv_emit_values_for_field(Node *schema_root, Node *field, exprtk_value_t value,
                                            parser_filter_buf_t *out, int *first) {
    const char *field_type = parser_node_string(field, "type");

    if (parser_node_has(field, "is_group_field")) {
        const char *group_type = parser_node_string(field, "group_type");
        Node *group_record = parser_find_record(schema_root, group_type);

        if (!group_record || !parser_value_is_sequence(value)) return 1;
        for (size_t i = 0; i < value.data.list.count; ++i) {
            if (!parser_csv_emit_values_for_record(schema_root, group_record,
                                                   value.data.list.items[i], out, first))
                return 0;
        }
        return 1;
    }

    if (parser_node_has(field, "is_map")) {
        const char *value_type = parser_node_string(field, "value_type");
        Node *value_record = parser_find_record(schema_root, value_type);
        Node *value_union = parser_find_union(schema_root, value_type);
        exprtk_map_iter_t it;
        const char *key;
        exprtk_value_t item;

        (void)key;
        if (!exprtk_value_is_object_like(&value) || !value_type) return 1;
        it = exprtk_map_iter_begin(&value);
        while (exprtk_map_iter_next(&it, &key, &item)) {
            if (value_record) {
                if (!parser_csv_emit_values_for_record(schema_root, value_record, item, out, first))
                    return 0;
            } else if (value_union) {
                if (!parser_csv_emit_values_for_union(schema_root, value_union, item, out, first))
                    return 0;
            } else {
                if (!parser_csv_emit_separator(out, first)) return 0;
                if (!parser_csv_emit_cell(out, item)) return 0;
            }
        }
        return 1;
    }

    if (parser_node_has(field, "is_collection") && !parser_node_has(field, "is_map")) {
        const char *inner_type = parser_node_string(field, "inner_type");
        size_t count = 0;
        if (!inner_type) return 1;
        if (!parser_node_size_value(field, "length_field", &count))
            count = parser_value_sequence_count(value);
        for (size_t i = 0; i < count; ++i) {
            exprtk_value_t item = parser_value_sequence_item(value, i);
            if (parser_node_has(field, "collection_element_is_composite")) {
                if (!parser_csv_emit_values_for_record(schema_root, parser_find_record(schema_root, inner_type),
                                                       item, out, first)) return 0;
            } else if (parser_find_union(schema_root, inner_type)) {
                if (!parser_csv_emit_values_for_union(schema_root,
                                                      parser_find_union(schema_root, inner_type),
                                                      item, out, first)) return 0;
            } else {
                if (!parser_csv_emit_separator(out, first)) return 0;
                if (!parser_csv_emit_cell(out, item)) return 0;
            }
        }
        return 1;
    }

    if (parser_node_has(field, "is_composite_ref") && field_type) {
        Node *inner_record = parser_find_record(schema_root, field_type);
        if (inner_record)
            return parser_csv_emit_values_for_record(schema_root, inner_record, value, out, first);
        return 1;
    }
    if (field_type && parser_find_union(schema_root, field_type))
        return parser_csv_emit_values_for_union(schema_root, parser_find_union(schema_root, field_type),
                                                value, out, first);

    if (parser_field_kind(schema_root, field) == PARSER_BIND_UNSUPPORTED) return 1;
    if (!parser_csv_emit_separator(out, first)) return 0;
    return parser_csv_emit_cell(out, value);
}

static int parser_csv_emit_values_for_union(Node *schema_root, Node *union_node,
                                            exprtk_value_t value,
                                            parser_filter_buf_t *out, int *first) {
    Node *fields = parser_node_child(union_node, "fields");

    if (!fields || fields->type != NODE_LIST || !exprtk_value_is_object_like(&value)) return 1;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        exprtk_value_t item;

        if (!name || !variant_type || !exprtk_map_has(&value, name)) continue;
        item = exprtk_map_get(&value, name);
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (variant_record) {
            if (!parser_csv_emit_values_for_record(schema_root, variant_record, item, out, first))
                return 0;
        } else if (variant_union) {
            if (!parser_csv_emit_values_for_union(schema_root, variant_union, item, out, first))
                return 0;
        } else if (parser_type_kind(schema_root, variant_type) != PARSER_BIND_UNSUPPORTED) {
            if (!parser_csv_emit_separator(out, first)) return 0;
            if (!parser_csv_emit_cell(out, item)) return 0;
        }
        return 1;
    }
    return 1;
}

static int parser_csv_emit_values_for_record(Node *schema_root, Node *record, exprtk_value_t value,
                                             parser_filter_buf_t *out, int *first) {
    Node *fields = parser_node_child(record, "fields");
    if (!fields || fields->type != NODE_LIST) return 1;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        exprtk_value_t field_value = parser_null();
        if (exprtk_value_is_object_like(&value) && name && exprtk_map_has(&value, name))
            field_value = exprtk_map_get(&value, name);
        if (!parser_csv_emit_values_for_field(schema_root, field, field_value, out, first))
            return 0;
    }
    return 1;
}

static int parser_csv_emit_values_for_union_schema(Node *schema_root, Node *union_node,
                                                   exprtk_value_t value,
                                                   parser_filter_buf_t *out, int *first) {
    Node *fields = parser_node_child(union_node, "fields");

    if (!fields || fields->type != NODE_LIST) return 1;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        exprtk_value_t item = parser_null();

        if (!name || !variant_type) return 0;
        if (exprtk_value_is_object_like(&value) && exprtk_map_has(&value, name))
            item = exprtk_map_get(&value, name);
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (variant_record) {
            if (!parser_csv_emit_values_for_record(schema_root, variant_record, item, out, first))
                return 0;
        } else if (variant_union) {
            if (!parser_csv_emit_values_for_union_schema(schema_root, variant_union, item,
                                                         out, first)) return 0;
        } else if (parser_type_kind(schema_root, variant_type) != PARSER_BIND_UNSUPPORTED) {
            if (!parser_csv_emit_separator(out, first)) return 0;
            if (!parser_csv_emit_cell(out, item)) return 0;
        }
    }
    return 1;
}

static int parser_value_lookup_path(exprtk_value_t value, const char *path, exprtk_value_t *out) {
    exprtk_value_t current = value;
    const char *p = path;

    if (!path || !out) return 0;
    while (*p) {
        if (*p == '.') {
            ++p;
            continue;
        }
        if (*p == '[') {
            char *end = NULL;
            unsigned long index;
            if (!parser_value_is_sequence(current)) return 0;
            ++p;
            errno = 0;
            index = strtoul(p, &end, 10);
            if (errno != 0 || !end || end == p || *end != ']' ||
                index >= current.data.list.count) return 0;
            current = current.data.list.items[index];
            p = end + 1;
            continue;
        }

        {
            char key[128];
            size_t len = 0;
            while (p[len] && p[len] != '.' && p[len] != '[') ++len;
            if (len == 0 || len >= sizeof(key) || !exprtk_value_is_object_like(&current))
                return 0;
            memcpy(key, p, len);
            key[len] = '\0';
            if (!exprtk_map_has(&current, key)) return 0;
            current = exprtk_map_get(&current, key);
            p += len;
        }
    }
    *out = current;
    return 1;
}

static int parser_csv_value_lookup_header_path(exprtk_value_t value, const char *path,
                                               exprtk_value_t *out) {
    exprtk_value_t current = value;
    const char *p = path;

    if (!path || !out) return 0;
    while (*p) {
        char key[128];
        size_t len = 0;

        if (*p == '.' || *p == '_') {
            ++p;
            continue;
        }
        if (*p == '[') {
            char *end = NULL;
            unsigned long index;
            if (!parser_value_is_sequence(current)) return 0;
            ++p;
            errno = 0;
            index = strtoul(p, &end, 10);
            if (errno != 0 || !end || end == p || *end != ']' ||
                index >= current.data.list.count) return 0;
            current = current.data.list.items[index];
            p = end + 1;
            continue;
        }

        while (p[len] && p[len] != '.' && p[len] != '[' && p[len] != '_') ++len;
        if (len == 0 || len >= sizeof(key) || !exprtk_value_is_object_like(&current))
            return 0;
        memcpy(key, p, len);
        key[len] = '\0';
        if (!exprtk_map_has(&current, key)) return 0;
        current = exprtk_map_get(&current, key);
        p += len;

        if (*p == '_' && parser_value_is_sequence(current) && p[1] >= '0' && p[1] <= '9') {
            char *end = NULL;
            unsigned long index;
            ++p;
            errno = 0;
            index = strtoul(p, &end, 10);
            if (errno != 0 || !end || end == p || index >= current.data.list.count)
                return 0;
            current = current.data.list.items[index];
            p = end;
        }
    }
    *out = current;
    return 1;
}

static int parser_csv_emit_headers_from_list(const parser_csv_headers_t *headers,
                                             parser_filter_buf_t *out) {
    int first = 1;
    if (!headers || !out) return 0;
    for (size_t i = 0; i < headers->count; ++i) {
        if (!parser_csv_emit_separator(out, &first)) return 0;
        if (!parser_buf_append(out, headers->names[i])) return 0;
    }
    return 1;
}

static int parser_csv_emit_values_for_header_list(exprtk_value_t value,
                                                  const parser_csv_headers_t *headers,
                                                  parser_filter_buf_t *out) {
    int first = 1;
    if (!headers || !out) return 0;
    for (size_t i = 0; i < headers->count; ++i) {
        exprtk_value_t item = parser_null();
        if (!parser_csv_emit_separator(out, &first)) return 0;
        if (parser_csv_value_lookup_header_path(value, headers->names[i], &item) &&
            !parser_csv_emit_cell(out, item)) return 0;
    }
    return 1;
}

static int parser_csv_collect_record_headers_from_value(Node *schema_root, Node *record,
                                                        exprtk_value_t value,
                                                        parser_csv_headers_t *headers) {
    parser_filter_buf_t buf = {0};
    parser_csv_headers_t row_headers = {0};
    exprtk_value_t header_text;
    int first = 1;
    int ok;

    if (!headers) return 0;
    ok = parser_csv_emit_headers_for_record(schema_root, record, "", value, &buf, &first);
    if (!ok) {
        free(buf.data);
        return 0;
    }
    if (buf.len == 0) {
        free(buf.data);
        return 1;
    }
    header_text = exprtk_val_str(tstr_v_from_buf(buf.data, buf.len));
    ok = parser_csv_parse_header_names(header_text, &row_headers) &&
         parser_csv_headers_merge(headers, &row_headers);
    parser_csv_headers_free(&row_headers);
    free(buf.data);
    return ok;
}

static int parser_csv_emit_record_list_value(Node *schema_root, Node *record,
                                             exprtk_value_t value,
                                             parser_filter_buf_t *out) {
    parser_csv_headers_t headers = {0};
    int ok = 1;

    if (!parser_value_is_sequence(value)) return 0;
    if (value.data.list.count == 0) {
        int first = 1;
        return parser_csv_emit_headers_for_record(schema_root, record, "", parser_null(), out, &first);
    }

    for (size_t i = 0; i < value.data.list.count; ++i) {
        if (!parser_csv_collect_record_headers_from_value(schema_root, record,
                                                          value.data.list.items[i], &headers)) {
            ok = 0;
            break;
        }
    }
    if (ok) ok = parser_csv_emit_headers_from_list(&headers, out);
    for (size_t i = 0; ok && i < value.data.list.count; ++i) {
        if (!parser_buf_append_char(out, '\n') ||
            !parser_csv_emit_values_for_header_list(value.data.list.items[i], &headers, out)) {
            ok = 0;
        }
    }
    parser_csv_headers_free(&headers);
    return ok;
}

static int parser_csv_emit_value(Node *schema_root, Node *record, exprtk_value_t value,
                                 parser_filter_buf_t *out) {
    int first = 1;
    if (parser_value_is_sequence(value))
        return parser_csv_emit_record_list_value(schema_root, record, value, out);
    if (!parser_csv_emit_headers_for_record(schema_root, record, "", value, out, &first)) return 0;

    first = 1;
    if (!parser_buf_append_char(out, '\n')) return 0;
    if (!parser_csv_emit_values_for_record(schema_root, record, value, out, &first)) return 0;
    return 1;
}

static int parser_csv_emit_union_value(Node *schema_root, Node *union_node, exprtk_value_t value,
                                       parser_filter_buf_t *out) {
    int first = 1;
    if (parser_value_is_sequence(value)) {
        if (!parser_csv_emit_headers_for_union_schema(schema_root, union_node, "", out, &first))
            return 0;
    } else if (!parser_csv_emit_headers_for_union(schema_root, union_node, "", value, out, &first)) {
        return 0;
    }

    if (parser_value_is_sequence(value)) {
        for (size_t i = 0; i < value.data.list.count; ++i) {
            first = 1;
            if (!parser_buf_append_char(out, '\n')) return 0;
            if (!parser_csv_emit_values_for_union_schema(schema_root, union_node,
                                                         value.data.list.items[i], out, &first))
                return 0;
        }
    } else {
        first = 1;
        if (!parser_buf_append_char(out, '\n')) return 0;
        if (!parser_csv_emit_values_for_union(schema_root, union_node, value, out, &first))
            return 0;
    }
    return 1;
}

static int parser_csv_emit_scalar_value(exprtk_value_t value, parser_filter_buf_t *out) {
    if (!parser_buf_append(out, "value")) return 0;
    if (parser_value_is_sequence(value)) {
        for (size_t i = 0; i < value.data.list.count; ++i) {
            if (!parser_buf_append_char(out, '\n')) return 0;
            if (!parser_csv_emit_cell(out, value.data.list.items[i])) return 0;
        }
    } else {
        if (!parser_buf_append_char(out, '\n')) return 0;
        if (!parser_csv_emit_cell(out, value)) return 0;
    }
    return 1;
}

static exprtk_value_t parser_csv_emit_with_schema(parser_ud_t *ud, Node *schema_root,
                                                  exprtk_value_t value_arg,
                                                  exprtk_value_t type_arg) {
    parser_filter_buf_t out = {0};
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    char *type_name;
    Node *record;
    Node *union_node;
    parser_bind_kind_t scalar_kind;

    if (!ud || !ud->env || !schema_root || type_arg.type != EXPRTK_VAL_STRING)
        return result;
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    record = parser_find_record(schema_root, type_name);
    union_node = parser_find_union(schema_root, type_name);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) return result;

    if ((record && parser_csv_emit_value(schema_root, record, value_arg, &out)) ||
        (union_node && parser_csv_emit_union_value(schema_root, union_node, value_arg, &out)) ||
        (!record && !union_node && parser_csv_emit_scalar_value(value_arg, &out)))
        result = parser_buf_to_string(&out, ud->env);
    free(out.data);
    return result;
}

static int parser_text_value_valid(parser_bind_kind_t kind, const char *text) {
    char *end = NULL;
    uuid_t uuid;

    if (kind == PARSER_BIND_STRING || kind == PARSER_BIND_BYTES) return text != NULL;
    if (!text) return 0;

    errno = 0;
    switch (kind) {
        case PARSER_BIND_UUID:
            return uuid_from_s(text, &uuid) ? 1 : 0;
        case PARSER_BIND_DATETIME:
            return parser_datetime_text_valid(text);
        case PARSER_BIND_DATE:
            return parser_date_text_valid(text);
        case PARSER_BIND_TIME:
            return parser_time_text_valid(text);
        case PARSER_BIND_DURATION:
            return parser_duration_text_valid(text);
        case PARSER_BIND_DECIMAL:
            return parser_decimal_text_valid(text);
        case PARSER_BIND_BIGINT:
            (void)strtoll(text, &end, 10);
            return errno == 0 && end && end != text && *end == '\0';
        case PARSER_BIND_MONEY:
            return text != NULL && text[0] != '\0';
        case PARSER_BIND_BOOL:
            return strcmp(text, "true") == 0 || strcmp(text, "false") == 0 ||
                   strcmp(text, "1") == 0 || strcmp(text, "0") == 0 ||
                   strcmp(text, "yes") == 0;
        case PARSER_BIND_INTEGER:
            (void)strtoll(text, &end, 10);
            return errno == 0 && end && end != text && *end == '\0';
        case PARSER_BIND_NUMBER:
            (void)strtod(text, &end);
            return errno == 0 && end && end != text && *end == '\0';
        default:
            return 0;
    }
}

static int parser_schema_text_value_valid(Node *root, const char *type_name,
                                          parser_bind_kind_t kind, const char *text) {
    int64_t integer_value = 0;
    if (parser_is_enum_type(root, type_name))
        return parser_schema_text_value(root, type_name, kind, text, &integer_value);
    return parser_text_value_valid(kind, text);
}

static int parser_json_scalar_valid(parser_bind_kind_t kind, json_value_t *value) {
    if (!value) return 0;
    switch (kind) {
        case PARSER_BIND_UUID:
            return turbo_json_type(value) == TURBO_JSON_STRING &&
                   parser_text_value_valid(kind, turbo_json_string(value));
        case PARSER_BIND_DATETIME:
            return turbo_json_type(value) == TURBO_JSON_STRING &&
                   parser_text_value_valid(kind, turbo_json_string(value));
        case PARSER_BIND_DATE:
        case PARSER_BIND_TIME:
            return turbo_json_type(value) == TURBO_JSON_STRING &&
                   parser_text_value_valid(kind, turbo_json_string(value));
        case PARSER_BIND_DURATION:
            return turbo_json_type(value) == TURBO_JSON_NUMBER ||
                   (turbo_json_type(value) == TURBO_JSON_STRING &&
                    parser_text_value_valid(kind, turbo_json_string(value)));
        case PARSER_BIND_DECIMAL:
            return (turbo_json_type(value) == TURBO_JSON_STRING &&
                    parser_text_value_valid(kind, turbo_json_string(value))) ||
                   turbo_json_type(value) == TURBO_JSON_NUMBER;
        case PARSER_BIND_BIGINT:
            return turbo_json_type(value) == TURBO_JSON_STRING ||
                   turbo_json_type(value) == TURBO_JSON_NUMBER;
        case PARSER_BIND_MONEY:
            return turbo_json_type(value) == TURBO_JSON_STRING ||
                   turbo_json_type(value) == TURBO_JSON_OBJECT;
        case PARSER_BIND_BYTES:
            return turbo_json_type(value) == TURBO_JSON_STRING;
        case PARSER_BIND_STRING:
            return turbo_json_type(value) == TURBO_JSON_STRING ||
                   turbo_json_type(value) == TURBO_JSON_NUMBER ||
                   turbo_json_type(value) == TURBO_JSON_BOOL;
        case PARSER_BIND_BOOL:
            if (turbo_json_type(value) == TURBO_JSON_BOOL ||
                turbo_json_type(value) == TURBO_JSON_NUMBER) return 1;
            return turbo_json_type(value) == TURBO_JSON_STRING &&
                   parser_text_value_valid(kind, turbo_json_string(value));
        case PARSER_BIND_INTEGER:
        case PARSER_BIND_NUMBER:
            if (turbo_json_type(value) == TURBO_JSON_NUMBER ||
                turbo_json_type(value) == TURBO_JSON_BOOL) return 1;
            return turbo_json_type(value) == TURBO_JSON_STRING &&
                   parser_text_value_valid(kind, turbo_json_string(value));
        default:
            return 0;
    }
}

static int parser_json_schema_scalar_valid(Node *root, const char *type_name,
                                           parser_bind_kind_t kind, json_value_t *value) {
    int64_t integer_value = 0;
    if (parser_is_flags_type(root, type_name))
        return parser_json_flags_value(root, type_name, value, &integer_value);
    if (parser_is_enum_type(root, type_name))
        return parser_json_enum_value(root, type_name, value, &integer_value);
    return parser_json_scalar_valid(kind, value);
}

static const char *parser_record_name(Node *record, const char *kind) {
    const char *name = parser_node_string(record, "name");
    if (name) return name;
    if (kind && strcmp(kind, "message") == 0) return parser_node_string(record, "message_name");
    if (kind && strcmp(kind, "composite") == 0) return parser_node_string(record, "composite_name");
    if (kind && strcmp(kind, "group") == 0) return parser_node_string(record, "group_name");
    if (kind && strcmp(kind, "enum") == 0) return parser_node_string(record, "enum_name");
    if (kind && strcmp(kind, "union") == 0) return parser_node_string(record, "union_name");
    return NULL;
}

static void parser_map_set_string(exprtk_env_t *env, exprtk_value_t *map,
                                  const char *key, const char *value) {
    if (!map || !key || !value) return;
    exprtk_map_set(map, key, parser_string_value(env, value));
}

static void parser_map_set_flag(exprtk_value_t *map, const char *key, Node *node, const char *node_key) {
    if (!map || !key || !node_key) return;
    exprtk_map_set(map, key, exprtk_val_int(parser_node_has(node, node_key) ? 1 : 0));
}

static exprtk_value_t parser_node_to_value(exprtk_env_t *env, Node *node) {
    exprtk_value_t result;

    if (!node) return parser_null();
    if (node->type == NODE_STRING)
        return parser_string_value(env, node->data.string_val ? node->data.string_val : "");
    if (node->type == NODE_LIST) {
        result = exprtk_val_list_empty();
        for (size_t i = 0; i < node->data.list.count; ++i)
            exprtk_list_push(&result, parser_node_to_value(env, node->data.list.items[i]));
        return result;
    }
    if (node->type == NODE_MAP) {
        result = exprtk_val_object();
        for (size_t i = 0; i < node->data.map.count; ++i) {
            Node *child = node->data.map.items[i];
            if (child && child->name)
                exprtk_map_set(&result, child->name, parser_node_to_value(env, child));
        }
        return result;
    }
    return parser_null();
}

static exprtk_value_t parser_attributes_to_map(exprtk_env_t *env, Node *owner) {
    exprtk_value_t result = exprtk_val_object();
    Node *attrs = parser_node_child(owner, "attributes");

    if (!env || !attrs) return result;
    if (attrs->type == NODE_LIST) {
        for (size_t i = 0; i < attrs->data.list.count; ++i) {
            Node *attr = attrs->data.list.items[i];
            Node *name_node;
            Node *value;
            const char *name;
            if (!attr) continue;
            name_node = parser_node_child(attr, "name");
            value = parser_node_child(attr, "value");
            name = name_node && name_node->type == NODE_STRING ? name_node->data.string_val : attr->name;
            if (name) exprtk_map_set(&result, name, value ? parser_node_to_value(env, value)
                                                           : parser_node_to_value(env, attr));
        }
        return result;
    }
    if (attrs->type != NODE_MAP) return result;
    for (size_t i = 0; i < attrs->data.map.count; ++i) {
        Node *attr = attrs->data.map.items[i];
        Node *value;
        if (!attr || !attr->name) continue;
        value = parser_node_child(attr, "value");
        exprtk_map_set(&result, attr->name, value ? parser_node_to_value(env, value)
                                                  : parser_node_to_value(env, attr));
    }
    return result;
}

typedef struct {
    char path[256];
    char message[160];
} parser_validate_detail_t;

static void parser_validate_set_error(parser_validate_detail_t *detail,
                                      const char *path, const char *message) {
    if (!detail || detail->message[0] != '\0') return;
    snprintf(detail->path, sizeof(detail->path), "%s", path ? path : "");
    snprintf(detail->message, sizeof(detail->message), "%s", message ? message : "validation failed");
}

static int parser_path_join(char *out, size_t out_size, const char *prefix, const char *name) {
    return parser_csv_join_path(out, out_size, prefix, name);
}

static int parser_path_index(char *out, size_t out_size, const char *prefix, size_t index) {
    return parser_csv_index_path(out, out_size, prefix, index);
}

static const char *parser_json_type_name(json_value_t *value) {
    if (!value) return "missing";
    switch (turbo_json_type(value)) {
        case TURBO_JSON_OBJECT: return "object";
        case TURBO_JSON_ARRAY: return "array";
        case TURBO_JSON_STRING: return "string";
        case TURBO_JSON_NUMBER: return "number";
        case TURBO_JSON_BOOL: return "bool";
        case TURBO_JSON_NULL: return "null";
        default: return "unknown";
    }
}

static const char *parser_kind_name(parser_bind_kind_t kind) {
    switch (kind) {
        case PARSER_BIND_NUMBER: return "number";
        case PARSER_BIND_INTEGER: return "integer";
        case PARSER_BIND_STRING: return "string";
        case PARSER_BIND_BYTES: return "bytes";
        case PARSER_BIND_BOOL: return "bool";
        case PARSER_BIND_UUID: return "uuid";
        case PARSER_BIND_DATETIME: return "datetime";
        case PARSER_BIND_DATE: return "date";
        case PARSER_BIND_TIME: return "time";
        case PARSER_BIND_DURATION: return "duration";
        case PARSER_BIND_DECIMAL: return "decimal";
        case PARSER_BIND_BIGINT: return "bigint";
        case PARSER_BIND_MONEY: return "money";
        default: return "supported value";
    }
}

static int parser_json_validate_record_detail(Node *schema_root, Node *record, json_value_t *object,
                                              const char *path, parser_validate_detail_t *detail);
static int parser_json_validate_union_detail(Node *schema_root, Node *union_node, json_value_t *object,
                                             const char *path, parser_validate_detail_t *detail);

static int parser_json_validate_typed_detail(Node *schema_root, const char *type_name,
                                             json_value_t *value, const char *path,
                                             parser_validate_detail_t *detail) {
    Node *record = parser_find_record(schema_root, type_name);
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
        scalar_kind = parser_type_kind(schema_root, type_name);
        if (scalar_kind != PARSER_BIND_UNSUPPORTED) {
            if (parser_json_schema_scalar_valid(schema_root, type_name, scalar_kind, value))
                return 1;
            parser_validate_set_error(detail, path, parser_kind_name(scalar_kind));
            return 0;
        }
        parser_validate_set_error(detail, path, "expected object");
        return 0;
    }
    if (record)
        return parser_json_validate_record_detail(schema_root, record, value, path, detail);
    union_node = parser_find_union(schema_root, type_name);
    if (union_node)
        return parser_json_validate_union_detail(schema_root, union_node, value, path, detail);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (scalar_kind != PARSER_BIND_UNSUPPORTED) {
        if (parser_json_schema_scalar_valid(schema_root, type_name, scalar_kind, value))
            return 1;
        parser_validate_set_error(detail, path, parser_kind_name(scalar_kind));
        return 0;
    }
    parser_validate_set_error(detail, path, "unknown schema type");
    return 0;
}

static int parser_json_validate_union_detail(Node *schema_root, Node *union_node, json_value_t *object,
                                             const char *path, parser_validate_detail_t *detail) {
    const char *variant_name;
    json_value_t *payload;
    Node *variant;
    const char *variant_type;
    char variant_path[256];
    parser_bind_kind_t scalar_kind;

    if (!union_node || !object || turbo_json_type(object) != TURBO_JSON_OBJECT) {
        parser_validate_set_error(detail, path, "expected union object");
        return 0;
    }
    if (turbo_json_object_size(object) != 1) {
        parser_validate_set_error(detail, path, "expected exactly one union variant");
        return 0;
    }
    variant_name = turbo_json_object_key(object, 0);
    payload = turbo_json_object_value(object, 0);
    variant = parser_union_variant(union_node, variant_name);
    if (!parser_path_join(variant_path, sizeof(variant_path), path, variant_name ? variant_name : "")) {
        parser_validate_set_error(detail, path, "path too long");
        return 0;
    }
    if (!variant) {
        parser_validate_set_error(detail, variant_path, "unknown union variant");
        return 0;
    }
    variant_type = parser_node_string(variant, "type");
    if (!variant_type || !payload) {
        parser_validate_set_error(detail, variant_path, "unknown schema type");
        return 0;
    }
    if (parser_find_record(schema_root, variant_type) || parser_find_union(schema_root, variant_type))
        return parser_json_validate_typed_detail(schema_root, variant_type, payload,
                                                 variant_path, detail);
    scalar_kind = parser_type_kind(schema_root, variant_type);
    if (!parser_json_schema_scalar_valid(schema_root, variant_type, scalar_kind, payload)) {
        parser_validate_set_error(detail, variant_path, parser_kind_name(scalar_kind));
        return 0;
    }
    return 1;
}

static int parser_json_validate_array_detail(Node *schema_root, Node *field, json_value_t *value,
                                             const char *path, parser_validate_detail_t *detail) {
    const char *inner_type = parser_node_string(field, "inner_type");
    parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);
    size_t expected = 0;

    if (!inner_type) {
        parser_validate_set_error(detail, path, "missing array element type");
        return 0;
    }
    if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
        parser_validate_set_error(detail, path, "expected array");
        return 0;
    }
    if (parser_node_size_value(field, "length_field", &expected) &&
        turbo_json_array_size(value) != expected) {
        parser_validate_set_error(detail, path, "array length mismatch");
        return 0;
    }
    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        json_value_t *item = turbo_json_array_get(value, i);
        char item_path[256];
        if (!parser_path_index(item_path, sizeof(item_path), path, i)) {
            parser_validate_set_error(detail, path, "path too long");
            return 0;
        }
        if (parser_node_has(field, "collection_element_is_composite") ||
            parser_find_union(schema_root, inner_type)) {
            if (!parser_json_validate_typed_detail(schema_root, inner_type, item, item_path, detail))
                return 0;
        } else if (!parser_json_schema_scalar_valid(schema_root, inner_type, scalar_kind, item)) {
            parser_validate_set_error(detail, item_path, parser_kind_name(scalar_kind));
            return 0;
        }
    }
    return 1;
}

static int parser_json_validate_map_detail(Node *schema_root, Node *field, json_value_t *value,
                                           const char *path, parser_validate_detail_t *detail) {
    const char *value_type = parser_node_string(field, "value_type");
    parser_bind_kind_t value_kind = parser_type_kind(schema_root, value_type);

    if (!value_type) {
        parser_validate_set_error(detail, path, "missing map value type");
        return 0;
    }
    if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
        parser_validate_set_error(detail, path, "expected object");
        return 0;
    }
    for (size_t i = 0; i < turbo_json_object_size(value); ++i) {
        const char *key = turbo_json_object_key(value, i);
        json_value_t *item = turbo_json_object_value(value, i);
        char item_path[256];
        if (!parser_path_join(item_path, sizeof(item_path), path, key ? key : "")) {
            parser_validate_set_error(detail, path, "path too long");
            return 0;
        }
        if (parser_find_record(schema_root, value_type) || parser_find_union(schema_root, value_type)) {
            if (!parser_json_validate_typed_detail(schema_root, value_type, item, item_path, detail))
                return 0;
        } else if (!parser_json_schema_scalar_valid(schema_root, value_type, value_kind, item)) {
            parser_validate_set_error(detail, item_path, parser_kind_name(value_kind));
            return 0;
        }
    }
    return 1;
}

static int parser_json_validate_group_detail(Node *schema_root, const char *group_type,
                                             json_value_t *value, const char *path,
                                             parser_validate_detail_t *detail) {
    if (!group_type) {
        parser_validate_set_error(detail, path, "missing group type");
        return 0;
    }
    if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
        parser_validate_set_error(detail, path, "expected array");
        return 0;
    }
    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        char item_path[256];
        if (!parser_path_index(item_path, sizeof(item_path), path, i)) {
            parser_validate_set_error(detail, path, "path too long");
            return 0;
        }
        if (!parser_json_validate_typed_detail(schema_root, group_type,
                                               turbo_json_array_get(value, i), item_path, detail))
            return 0;
    }
    return 1;
}

static int parser_json_validate_record_detail(Node *schema_root, Node *record, json_value_t *object,
                                              const char *path, parser_validate_detail_t *detail) {
    Node *fields = parser_node_child(record, "fields");

    if (!fields || fields->type != NODE_LIST || !object || turbo_json_type(object) != TURBO_JSON_OBJECT) {
        parser_validate_set_error(detail, path, "expected object");
        return 0;
    }

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        const char *field_type = parser_node_string(field, "type");
        json_value_t *value;
        char field_path[256];

        if (!name) continue;
        if (!parser_path_join(field_path, sizeof(field_path), path, name)) {
            parser_validate_set_error(detail, path, "path too long");
            return 0;
        }
        value = turbo_json_object_get(object, name);
        if (!value) {
            if (parser_field_missing_allowed(field)) continue;
            parser_validate_set_error(detail, field_path, "missing required field");
            return 0;
        }

        if (parser_node_has(field, "is_group_field")) {
            if (!parser_json_validate_group_detail(schema_root, parser_node_string(field, "group_type"),
                                                   value, field_path, detail)) return 0;
        } else if (parser_node_has(field, "is_map")) {
            if (!parser_json_validate_map_detail(schema_root, field, value, field_path, detail))
                return 0;
        } else if (parser_node_has(field, "is_collection")) {
            if (!parser_json_validate_array_detail(schema_root, field, value, field_path, detail))
                return 0;
        } else if (parser_node_has(field, "is_composite_ref") && field_type) {
            if (!parser_json_validate_typed_detail(schema_root, field_type, value, field_path, detail))
                return 0;
        } else if (field_type && parser_find_union(schema_root, field_type)) {
            if (!parser_json_validate_typed_detail(schema_root, field_type, value, field_path, detail))
                return 0;
        } else if (!parser_json_schema_scalar_valid(schema_root, field_type,
                                                    parser_field_kind(schema_root, field), value)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "expected %s but got %s",
                     parser_kind_name(parser_field_kind(schema_root, field)),
                     parser_json_type_name(value));
            parser_validate_set_error(detail, field_path, msg);
            return 0;
        }
    }
    return 1;
}

static int parser_json_validate_value_detail(Node *schema_root, Node *record, json_value_t *json,
                                             parser_validate_detail_t *detail) {
    if (!json) {
        parser_validate_set_error(detail, "", "invalid json");
        return 0;
    }
    if (turbo_json_type(json) == TURBO_JSON_ARRAY) {
        for (size_t i = 0; i < turbo_json_array_size(json); ++i) {
            char item_path[256];
            if (!parser_path_index(item_path, sizeof(item_path), "", i)) {
                parser_validate_set_error(detail, "", "path too long");
                return 0;
            }
            if (!parser_json_validate_record_detail(schema_root, record,
                                                    turbo_json_array_get(json, i), item_path, detail))
                return 0;
        }
        return 1;
    }
    return parser_json_validate_record_detail(schema_root, record, json, "", detail);
}

static int parser_json_validate_typed_value_detail(Node *schema_root, const char *type_name,
                                                   json_value_t *json,
                                                   parser_validate_detail_t *detail) {
    if (!json) {
        parser_validate_set_error(detail, "", "invalid json");
        return 0;
    }
    if (turbo_json_type(json) == TURBO_JSON_ARRAY) {
        for (size_t i = 0; i < turbo_json_array_size(json); ++i) {
            char item_path[256];
            if (!parser_path_index(item_path, sizeof(item_path), "", i)) {
                parser_validate_set_error(detail, "", "path too long");
                return 0;
            }
            if (!parser_json_validate_typed_detail(schema_root, type_name,
                                                   turbo_json_array_get(json, i), item_path, detail))
                return 0;
        }
        return 1;
    }
    return parser_json_validate_typed_detail(schema_root, type_name, json, "", detail);
}

static int parser_csv_validate_record_detail(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                             size_t row, const parser_csv_headers_t *headers,
                                             const char *prefix,
                                             parser_validate_detail_t *detail);
static int parser_csv_validate_union_detail(Node *schema_root, Node *union_node, turbo_csv_doc_t *doc,
                                            size_t row, const parser_csv_headers_t *headers,
                                            const char *path,
                                            parser_validate_detail_t *detail);

static int parser_csv_validate_scalar_detail(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                             size_t row, const char *path,
                                             parser_validate_detail_t *detail) {
    size_t col = 0;
    const char *text;
    parser_bind_kind_t kind = parser_field_kind(schema_root, field);

    if (kind == PARSER_BIND_UNSUPPORTED) {
        parser_validate_set_error(detail, path, "unsupported CSV field");
        return 0;
    }
    if (!parser_csv_find_path_column(doc, path, &col)) {
        if (parser_field_missing_allowed(field)) return 1;
        parser_validate_set_error(detail, path, "missing column");
        return 0;
    }
    text = turbo_csv_get(doc, row, col);
    if (!parser_schema_text_value_valid(schema_root, parser_node_string(field, "type"), kind, text)) {
        parser_validate_set_error(detail, path, parser_kind_name(kind));
        return 0;
    }
    return 1;
}

static int parser_csv_validate_scalar_value_detail(Node *schema_root, const char *type_name,
                                                   parser_bind_kind_t kind,
                                                   turbo_csv_doc_t *doc, size_t row,
                                                   parser_validate_detail_t *detail) {
    size_t col = 0;
    const char *text;

    if (!doc || !type_name || kind == PARSER_BIND_UNSUPPORTED ||
        row >= turbo_csv_row_count(doc) || !parser_csv_find_value_column(doc, &col)) {
        parser_validate_set_error(detail, "value", "missing column");
        return 0;
    }
    text = turbo_csv_get(doc, row, col);
    if (!parser_schema_text_value_valid(schema_root, type_name, kind, text)) {
        parser_validate_set_error(detail, "value", parser_kind_name(kind));
        return 0;
    }
    return 1;
}

static int parser_csv_validate_union_detail(Node *schema_root, Node *union_node, turbo_csv_doc_t *doc,
                                            size_t row, const parser_csv_headers_t *headers,
                                            const char *path,
                                            parser_validate_detail_t *detail) {
    Node *fields = parser_node_child(union_node, "fields");
    int matches = 0;

    if (!fields || fields->type != NODE_LIST || !headers || !path) {
        parser_validate_set_error(detail, path, "unsupported CSV union");
        return 0;
    }
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        parser_bind_kind_t scalar_kind;
        char item_path[256];
        int ok = 0;

        if (!name || !variant_type || !parser_path_join(item_path, sizeof(item_path), path, name)) {
            parser_validate_set_error(detail, path, "path too long");
            return 0;
        }
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
        if (variant_record) {
            ok = parser_csv_validate_record_detail(schema_root, variant_record, doc, row,
                                                   headers, item_path, detail);
        } else if (variant_union) {
            ok = parser_csv_validate_union_detail(schema_root, variant_union, doc, row,
                                                  headers, item_path, detail);
        } else {
            size_t col = 0;
            const char *text;
            scalar_kind = parser_type_kind(schema_root, variant_type);
            if (scalar_kind != PARSER_BIND_UNSUPPORTED &&
                parser_csv_find_path_column(doc, item_path, &col)) {
                text = turbo_csv_get(doc, row, col);
                ok = parser_schema_text_value_valid(schema_root, variant_type, scalar_kind, text);
                if (!ok) {
                    parser_validate_set_error(detail, item_path, parser_kind_name(scalar_kind));
                    return 0;
                }
            }
        }
        if (ok) matches++;
    }
    if (matches != 1) {
        parser_validate_set_error(detail, path, matches == 0 ? "missing union variant"
                                                             : "multiple union variants");
        return 0;
    }
    return 1;
}

static int parser_csv_validate_field_detail(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                            size_t row, const parser_csv_headers_t *headers,
                                            const char *path,
                                            parser_validate_detail_t *detail) {
    const char *field_type = parser_node_string(field, "type");

    if (parser_node_has(field, "is_group_field")) {
        const char *group_type = parser_node_string(field, "group_type");
        Node *group_record = parser_find_record(schema_root, group_type);
        parser_index_list_t indexes = {0};
        int ok = 1;

        if (!group_record) {
            parser_validate_set_error(detail, path, "missing group type");
            return 0;
        }
        if (!parser_csv_collect_group_indexes(headers, path, &indexes)) {
            parser_index_list_free(&indexes);
            if (parser_field_missing_allowed(field)) return 1;
            parser_validate_set_error(detail, path, "missing column");
            return 0;
        }
        for (size_t i = 0; i < indexes.count; ++i) {
            char item_path[256];
            if (!parser_path_index(item_path, sizeof(item_path), path, indexes.values[i])) {
                parser_validate_set_error(detail, path, "path too long");
                ok = 0;
                break;
            }
            if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
            if (!parser_csv_validate_record_detail(schema_root, group_record, doc, row,
                                                   headers, item_path, detail)) {
                ok = 0;
                break;
            }
        }
        parser_index_list_free(&indexes);
        return ok;
    }

    if (parser_node_has(field, "is_map")) {
        const char *value_type = parser_node_string(field, "value_type");
        parser_bind_kind_t value_kind = parser_type_kind(schema_root, value_type);
        Node *value_record = parser_find_record(schema_root, value_type);
        Node *value_union = parser_find_union(schema_root, value_type);
        int any = 0;

        if (!value_type) {
            parser_validate_set_error(detail, path, "missing map value type");
            return 0;
        }
        if (!headers) {
            parser_validate_set_error(detail, path, "missing column");
            return 0;
        }
        for (size_t i = 0; i < headers->count; ++i) {
            char key[128];
            char item_path[256];
            if (!parser_csv_header_map_key(headers->names[i], path, key, sizeof(key))) continue;
            if (!parser_path_join(item_path, sizeof(item_path), path, key)) {
                parser_validate_set_error(detail, path, "path too long");
                return 0;
            }
            if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
            any = 1;
            if (value_record) {
                if (!parser_csv_validate_record_detail(schema_root, value_record, doc, row,
                                                       headers, item_path, detail)) return 0;
            } else if (value_union) {
                if (!parser_csv_validate_union_detail(schema_root, value_union, doc, row,
                                                      headers, item_path, detail)) return 0;
            } else {
                size_t col = 0;
                const char *text;
                if (value_kind == PARSER_BIND_UNSUPPORTED) {
                    parser_validate_set_error(detail, item_path, "unsupported CSV field");
                    return 0;
                }
                if (!parser_csv_find_path_column(doc, item_path, &col)) {
                    parser_validate_set_error(detail, item_path, "missing column");
                    return 0;
                }
                text = turbo_csv_get(doc, row, col);
                if (!parser_schema_text_value_valid(schema_root, value_type, value_kind, text)) {
                    parser_validate_set_error(detail, item_path, parser_kind_name(value_kind));
                    return 0;
                }
            }
        }
        if (!any) {
            if (parser_field_missing_allowed(field)) return 1;
            parser_validate_set_error(detail, path, "missing column");
            return 0;
        }
        return 1;
    }

    if (parser_node_has(field, "is_collection")) {
        const char *inner_type = parser_node_string(field, "inner_type");
        parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);
        size_t count = 0;
        parser_index_list_t indexes = {0};
        int fixed_count = 0;
        int ok = 1;

        if (!inner_type) {
            parser_validate_set_error(detail, path, "unsupported CSV array");
            return 0;
        }
        fixed_count = parser_node_size_value(field, "length_field", &count);
        if (fixed_count) {
            for (size_t i = 0; i < count; ++i)
                if (!parser_index_list_push(&indexes, i)) {
                    parser_validate_set_error(detail, path, "out of memory");
                    parser_index_list_free(&indexes);
                    return 0;
                }
        } else if (!parser_csv_collect_group_indexes(headers, path, &indexes)) {
            if (parser_field_missing_allowed(field)) {
                parser_index_list_free(&indexes);
                return 1;
            }
            parser_validate_set_error(detail, path, "missing column");
            parser_index_list_free(&indexes);
            return 0;
        }
        for (size_t i = 0; i < indexes.count; ++i) {
            char item_path[256];
            if (!parser_path_index(item_path, sizeof(item_path), path, indexes.values[i])) {
                parser_validate_set_error(detail, path, "path too long");
                ok = 0;
                break;
            }
            if (!fixed_count && !parser_csv_row_has_nonempty_path(doc, row, headers, item_path))
                continue;
            if (parser_node_has(field, "collection_element_is_composite")) {
                if (!parser_csv_validate_record_detail(schema_root, parser_find_record(schema_root, inner_type),
                                                       doc, row, headers, item_path, detail)) {
                    ok = 0;
                    break;
                }
            } else if (parser_find_union(schema_root, inner_type)) {
                if (!parser_csv_validate_union_detail(schema_root, parser_find_union(schema_root, inner_type),
                                                      doc, row, headers, item_path, detail)) {
                    ok = 0;
                    break;
                }
            } else {
                size_t col = 0;
                const char *text;
                if (!parser_csv_find_path_column(doc, item_path, &col)) {
                    parser_validate_set_error(detail, item_path, "missing column");
                    ok = 0;
                    break;
                }
                text = turbo_csv_get(doc, row, col);
                if (!parser_schema_text_value_valid(schema_root, inner_type, scalar_kind, text)) {
                    parser_validate_set_error(detail, item_path, parser_kind_name(scalar_kind));
                    ok = 0;
                    break;
                }
            }
        }
        parser_index_list_free(&indexes);
        return ok;
    }

    if (parser_node_has(field, "is_composite_ref") && field_type) {
        Node *inner_record = parser_find_record(schema_root, field_type);
        if (parser_field_missing_allowed(field) && !parser_csv_headers_have_path(headers, path))
            return 1;
        if (inner_record)
            return parser_csv_validate_record_detail(schema_root, inner_record, doc, row, headers,
                                                     path, detail);
        parser_validate_set_error(detail, path, "unknown schema type");
        return 0;
    }
    if (field_type && parser_find_union(schema_root, field_type)) {
        if (parser_field_missing_allowed(field) && !parser_csv_headers_have_path(headers, path))
            return 1;
        return parser_csv_validate_union_detail(schema_root, parser_find_union(schema_root, field_type),
                                                doc, row, headers, path, detail);
    }

    return parser_csv_validate_scalar_detail(schema_root, field, doc, row, path, detail);
}

static int parser_csv_validate_record_detail(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                             size_t row, const parser_csv_headers_t *headers,
                                             const char *prefix,
                                             parser_validate_detail_t *detail) {
    Node *fields = parser_node_child(record, "fields");

    if (!fields || fields->type != NODE_LIST || !doc || row >= turbo_csv_row_count(doc)) {
        parser_validate_set_error(detail, prefix, "invalid CSV row");
        return 0;
    }
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        char path[256];

        if (!name) continue;
        if (!parser_path_join(path, sizeof(path), prefix, name)) {
            parser_validate_set_error(detail, prefix, "path too long");
            return 0;
        }
        if (!parser_csv_validate_field_detail(schema_root, field, doc, row, headers, path, detail))
            return 0;
    }
    return 1;
}

static exprtk_value_t parser_validate_detail_map(parser_ud_t *ud, int ok,
                                                 const parser_validate_detail_t *detail) {
    exprtk_value_t result = exprtk_val_object();
    exprtk_map_set(&result, "ok", exprtk_val_int(ok ? 1 : 0));
    parser_map_set_string(ud ? ud->env : NULL, &result, "path",
                          ok || !detail ? "" : detail->path);
    parser_map_set_string(ud ? ud->env : NULL, &result, "message",
                          ok || !detail ? "" : detail->message);
    return result;
}

static exprtk_value_t parser_data_bind_validate_detail(
    parser_ud_t *ud, DataBind *codec, exprtk_value_t data_arg, exprtk_value_t type_arg,
    const char *arg_error,
    DataBindStatus (*validate_fn)(DataBind *, const char *, const char *, size_t,
                                  DataBindError *)) {
    char *type_name;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || !codec || data_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING || !validate_fn) {
        parser_validate_set_error(&detail, "", arg_error ? arg_error : "expected data and schema type");
        return parser_validate_detail_map(ud, 0, &detail);
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) {
        parser_validate_set_error(&detail, "", "out of memory");
        return parser_validate_detail_map(ud, 0, &detail);
    }

    status = validate_fn(codec, type_name, data_arg.data.string.data,
                         data_arg.data.string.len, &err);
    if (status == DATA_BIND_OK) {
        parser_set_ctx_error(ud->ctx, "");
        return parser_validate_detail_map(ud, 1, &detail);
    }
    parser_validate_set_error(&detail, err.path,
                              err.message[0] != '\0'
                                  ? err.message
                                  : data_bind_status_name(status));
    return parser_validate_detail_map(ud, 0, &detail);
}

static exprtk_value_t parser_data_bind_validate_xml_detail(parser_ud_t *ud, DataBind *codec,
                                                           exprtk_value_t xml_arg,
                                                           exprtk_value_t xpath_arg,
                                                           exprtk_value_t type_arg) {
    char *type_name;
    char *xpath = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || !codec || xml_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        parser_validate_set_error(&detail, "", "expected XML text and schema type");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    if (xpath_arg.type != EXPRTK_VAL_NULL && xpath_arg.type != EXPRTK_VAL_STRING) {
        parser_validate_set_error(&detail, "", "expected XPath string");
        return parser_validate_detail_map(ud, 0, &detail);
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) {
        parser_validate_set_error(&detail, "", "out of memory");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    if (xpath_arg.type == EXPRTK_VAL_STRING) {
        xpath = parser_arena_cstr(ud->scratch, xpath_arg.data.string);
        if (!xpath) {
            parser_validate_set_error(&detail, "", "out of memory");
            return parser_validate_detail_map(ud, 0, &detail);
        }
    }

    status = data_bind_validate_xml(codec, type_name, xml_arg.data.string.data,
                                    xml_arg.data.string.len, xpath, &err);
    if (status == DATA_BIND_OK) return parser_validate_detail_map(ud, 1, &detail);
    parser_validate_set_error(&detail, err.path, err.message[0] != '\0'
                                                   ? err.message
                                                   : data_bind_status_name(status));
    return parser_validate_detail_map(ud, 0, &detail);
}

static exprtk_value_t parser_json_validate_ex_with_schema(parser_ud_t *ud, Node *schema_root,
                                                          exprtk_value_t json_arg,
                                                          exprtk_value_t type_arg) {
    parser_validate_detail_t detail = {{0}, {0}};
    char *type_name;
    json_value_t *json = NULL;
    void *json_ptr;
    int ok = 0;

    if (!ud || !schema_root || json_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        parser_validate_set_error(&detail, "", "expected (schema, json, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name || !parser_type_supported(schema_root, type_name)) {
        parser_validate_set_error(&detail, "", "unknown schema type");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    if (turbo_parse_json((const uint8_t *)json_arg.data.string.data,
                         json_arg.data.string.len, &json) == 0 && json) {
        ok = parser_json_validate_typed_value_detail(schema_root, type_name, json, &detail);
    } else {
        parser_validate_set_error(&detail, "", "invalid json");
    }
    json_ptr = json;
    if (json_ptr) turbo_free_json(&json_ptr);
    return parser_validate_detail_map(ud, ok, &detail);
}

static exprtk_value_t parser_csv_validate_ex_with_schema(parser_ud_t *ud, Node *schema_root,
                                                         exprtk_value_t csv_arg,
                                                         exprtk_value_t type_arg) {
    parser_validate_detail_t detail = {{0}, {0}};
    char *type_name;
    Node *record;
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    turbo_csv_doc_t *doc;
    parser_csv_headers_t headers;
    int ok = 1;

    memset(&headers, 0, sizeof(headers));

    if (!ud || !schema_root || csv_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        parser_validate_set_error(&detail, "", "expected (schema, csv, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    record = type_name ? parser_find_record(schema_root, type_name) : NULL;
    union_node = type_name ? parser_find_union(schema_root, type_name) : NULL;
    scalar_kind = type_name ? parser_type_kind(schema_root, type_name) : PARSER_BIND_UNSUPPORTED;
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) {
        parser_validate_set_error(&detail, "", "unknown schema type");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    doc = parser_parse_csv_string(csv_arg);
    if (!doc) {
        parser_validate_set_error(&detail, "", "invalid csv");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    (void)parser_csv_parse_header_names(csv_arg, &headers);
    for (size_t row = 0; row < turbo_csv_row_count(doc); ++row) {
        int row_ok = record
                         ? parser_csv_validate_record_detail(schema_root, record, doc, row, &headers, "", &detail)
                         : (union_node
                                ? parser_csv_validate_union_detail(schema_root, union_node, doc, row, &headers, "", &detail)
                                : parser_csv_validate_scalar_value_detail(schema_root, type_name, scalar_kind,
                                                                          doc, row, &detail));
        if (!row_ok) {
            ok = 0;
            break;
        }
    }
    parser_csv_headers_free(&headers);
    parser_free_csv_doc(doc);
    return parser_validate_detail_map(ud, ok, &detail);
}

static const char *parser_reflect_field_kind(Node *schema_root, Node *field) {
    const char *field_type = parser_node_string(field, "type");
    if (parser_node_has(field, "is_group_field")) return "group";
    if (parser_node_has(field, "is_map")) return "map";
    if (parser_node_has(field, "is_collection")) return "array";
    if (field_type && parser_find_union(schema_root, field_type)) return "union";
    if (parser_node_has(field, "is_composite_ref")) return "composite";
    if (parser_field_kind(schema_root, field) != PARSER_BIND_UNSUPPORTED) return "scalar";
    return field_type ? "custom" : "unknown";
}

static exprtk_value_t parser_schema_types_with_schema(parser_ud_t *ud, Node *schema_root) {
    exprtk_value_t result = exprtk_val_list_empty();
    struct {
        const char *list_name;
        const char *kind;
    } lists[] = {
        {"messages", "message"},
        {"composites", "composite"},
        {"groups", "group"},
        {"enums", "enum"},
        {"unions", "union"},
    };

    if (!ud || !ud->env || !schema_root) return result;
    for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); ++l) {
        Node *records = parser_node_child(schema_root, lists[l].list_name);
        if (!records || records->type != NODE_LIST) continue;
        for (size_t i = 0; i < records->data.list.count; ++i) {
            Node *record = records->data.list.items[i];
            Node *fields = parser_node_child(record, "fields");
            const char *name = parser_record_name(record, lists[l].kind);
            exprtk_value_t item = exprtk_val_object();

            if (!name) continue;
            parser_map_set_string(ud->env, &item, "name", name);
            parser_map_set_string(ud->env, &item, "kind",
                                  parser_node_has(record, "is_flags") ? "flags" : lists[l].kind);
            exprtk_map_set(&item, "field_count",
                           exprtk_val_int(fields && fields->type == NODE_LIST
                                               ? (int64_t)fields->data.list.count
                                               : 0));
            exprtk_list_push(&result, item);
        }
    }
    return result;
}

static exprtk_value_t parser_schema_fields_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       exprtk_value_t type_arg) {
    exprtk_value_t result = exprtk_val_list_empty();
    char *type_name;
    Node *record;
    Node *fields;

    if (!ud || !ud->env || !schema_root || type_arg.type != EXPRTK_VAL_STRING)
        return result;
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    record = parser_find_record(schema_root, type_name);
    if (!record) record = parser_find_union(schema_root, type_name);
    if (!record) return result;
    fields = parser_node_child(record, "fields");
    if (!fields || fields->type != NODE_LIST) return result;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        const char *type = parser_node_string(field, "type");
        const char *inner_type = parser_node_string(field, "inner_type");
        const char *group_type = parser_node_string(field, "group_type");
        const char *value_type = parser_node_string(field, "value_type");
        const char *length = parser_node_string(field, "length_field");
        const char *collection_kind = parser_node_string(field, "collection_kind");
        exprtk_value_t item = exprtk_val_object();

        if (!name) continue;
        parser_map_set_string(ud->env, &item, "name", name);
        if (type) parser_map_set_string(ud->env, &item, "type", type);
        parser_map_set_string(ud->env, &item, "kind", parser_reflect_field_kind(schema_root, field));
        if (inner_type) parser_map_set_string(ud->env, &item, "inner_type", inner_type);
        if (group_type) parser_map_set_string(ud->env, &item, "group_type", group_type);
        if (value_type) parser_map_set_string(ud->env, &item, "value_type", value_type);
        if (length) parser_map_set_string(ud->env, &item, "length", length);
        if (collection_kind) parser_map_set_string(ud->env, &item, "collection_kind", collection_kind);
        parser_map_set_flag(&item, "is_collection", field, "is_collection");
        parser_map_set_flag(&item, "is_composite", field, "is_composite_ref");
        parser_map_set_flag(&item, "is_group", field, "is_group_field");
        parser_map_set_flag(&item, "is_map", field, "is_map");
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_enums_with_schema(parser_ud_t *ud, Node *schema_root) {
    exprtk_value_t result = exprtk_val_list_empty();
    Node *enums;

    if (!ud || !ud->env || !schema_root) return result;
    enums = parser_node_child(schema_root, "enums");
    if (!enums || enums->type != NODE_LIST) return result;

    for (size_t i = 0; i < enums->data.list.count; ++i) {
        Node *e = enums->data.list.items[i];
        Node *items = parser_node_child(e, "items");
        const char *name = parser_record_name(e, "enum");
        const char *underlying = parser_node_string(e, "underlying_type");
        exprtk_value_t item = exprtk_val_object();

        if (!name) continue;
        parser_map_set_string(ud->env, &item, "name", name);
        if (underlying) parser_map_set_string(ud->env, &item, "underlying_type", underlying);
        exprtk_map_set(&item, "is_flags", exprtk_val_int(parser_node_has(e, "is_flags") ? 1 : 0));
        exprtk_map_set(&item, "item_count",
                       exprtk_val_int(items && items->type == NODE_LIST
                                           ? (int64_t)items->data.list.count
                                           : 0));
        if (items) exprtk_map_set(&item, "items", parser_node_to_value(ud->env, items));
        exprtk_map_set(&item, "attributes", parser_attributes_to_map(ud->env, e));
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_flags_with_schema(parser_ud_t *ud, Node *schema_root) {
    exprtk_value_t result = exprtk_val_list_empty();
    Node *enums;

    if (!ud || !ud->env || !schema_root) return result;
    enums = parser_node_child(schema_root, "enums");
    if (!enums || enums->type != NODE_LIST) return result;

    for (size_t i = 0; i < enums->data.list.count; ++i) {
        Node *e = enums->data.list.items[i];
        Node *items;
        const char *name;
        const char *underlying;
        exprtk_value_t item;

        if (!parser_node_has(e, "is_flags")) continue;
        items = parser_node_child(e, "items");
        name = parser_record_name(e, "enum");
        underlying = parser_node_string(e, "underlying_type");
        item = exprtk_val_object();
        if (!name) continue;
        parser_map_set_string(ud->env, &item, "name", name);
        if (underlying) parser_map_set_string(ud->env, &item, "underlying_type", underlying);
        exprtk_map_set(&item, "item_count",
                       exprtk_val_int(items && items->type == NODE_LIST
                                           ? (int64_t)items->data.list.count
                                           : 0));
        if (items) exprtk_map_set(&item, "items", parser_node_to_value(ud->env, items));
        exprtk_map_set(&item, "attributes", parser_attributes_to_map(ud->env, e));
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_unions_with_schema(parser_ud_t *ud, Node *schema_root) {
    exprtk_value_t result = exprtk_val_list_empty();
    Node *unions;

    if (!ud || !ud->env || !schema_root) return result;
    unions = parser_node_child(schema_root, "unions");
    if (!unions || unions->type != NODE_LIST) return result;

    for (size_t i = 0; i < unions->data.list.count; ++i) {
        Node *u = unions->data.list.items[i];
        Node *fields = parser_node_child(u, "fields");
        const char *name = parser_record_name(u, "union");
        exprtk_value_t item = exprtk_val_object();

        if (!name) continue;
        parser_map_set_string(ud->env, &item, "name", name);
        exprtk_map_set(&item, "variant_count",
                       exprtk_val_int(fields && fields->type == NODE_LIST
                                           ? (int64_t)fields->data.list.count
                                           : 0));
        if (fields) exprtk_map_set(&item, "variants", parser_node_to_value(ud->env, fields));
        exprtk_map_set(&item, "attributes", parser_attributes_to_map(ud->env, u));
        exprtk_list_push(&result, item);
    }
    return result;
}

static Node *parser_schema_named_owner(Node *schema_root, const char *type_name) {
    Node *owner;
    if (!schema_root) return NULL;
    if (!type_name || type_name[0] == '\0') return parser_node_child(schema_root, "schema");
    owner = parser_find_record(schema_root, type_name);
    if (!owner) owner = parser_find_union(schema_root, type_name);
    if (owner) return owner;
    if (parser_is_enum_type(schema_root, type_name)) {
        Node *enums = parser_node_child(schema_root, "enums");
        if (enums && enums->type == NODE_LIST) {
            for (size_t i = 0; i < enums->data.list.count; ++i) {
                Node *e = enums->data.list.items[i];
                const char *name = parser_record_name(e, "enum");
                if (name && strcmp(name, type_name) == 0) return e;
            }
        }
    }
    return NULL;
}

static exprtk_value_t parser_schema_attributes_with_schema(parser_ud_t *ud, Node *schema_root,
                                                           exprtk_value_t *type_arg) {
    char *type_name = NULL;
    Node *owner;

    if (!ud || !ud->env || !schema_root) return exprtk_val_object();
    if (type_arg) {
        if (type_arg->type != EXPRTK_VAL_STRING) return exprtk_val_object();
        type_name = parser_arena_cstr(ud->scratch, type_arg->data.string);
        if (!type_name) return exprtk_val_object();
    }
    owner = parser_schema_named_owner(schema_root, type_name);
    return parser_attributes_to_map(ud->env, owner);
}

static exprtk_value_t parser_schema_layout_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       exprtk_value_t *type_arg) {
    exprtk_value_t result = exprtk_val_object();
    Node *owner;
    char *type_name = NULL;
    const char *name;
    const char *kind = "schema";
    const char *fixed_block_size;
    const char *wire_byte_order;
    Node *fields;

    if (!ud || !ud->env || !schema_root) return result;
    if (type_arg) {
        if (type_arg->type != EXPRTK_VAL_STRING) return result;
        type_name = parser_arena_cstr(ud->scratch, type_arg->data.string);
        if (!type_name) return result;
    }

    owner = parser_schema_named_owner(schema_root, type_name);
    if (!owner) return result;

    if (type_name && parser_find_record(schema_root, type_name)) {
        if (parser_node_child(owner, "message_name")) kind = "message";
        else if (parser_node_child(owner, "composite_name")) kind = "composite";
        else if (parser_node_child(owner, "group_name")) kind = "group";
    } else if (type_name && parser_find_union(schema_root, type_name)) {
        kind = "union";
    } else if (type_name && parser_is_enum_type(schema_root, type_name)) {
        kind = parser_node_has(owner, "is_flags") ? "flags" : "enum";
    }

    name = type_name && type_name[0] != '\0' ? type_name : parser_node_string(owner, "schema_name");
    fixed_block_size = parser_node_string(owner, "fixed_block_size");
    wire_byte_order = parser_node_string(owner, "wire_byte_order");
    fields = parser_node_child(owner, "fields");

    if (name) parser_map_set_string(ud->env, &result, "name", name);
    parser_map_set_string(ud->env, &result, "kind", kind);
    if (fixed_block_size)
        exprtk_map_set(&result, "fixed_block_size", exprtk_val_int((int64_t)strtoll(fixed_block_size, NULL, 10)));
    if (wire_byte_order) parser_map_set_string(ud->env, &result, "wire_byte_order", wire_byte_order);
    exprtk_map_set(&result, "has_fixed_block_size",
                   exprtk_val_int(parser_node_has(owner, "has_fixed_block_size") ? 1 : 0));
    exprtk_map_set(&result, "field_count",
                   exprtk_val_int(fields && fields->type == NODE_LIST
                                       ? (int64_t)fields->data.list.count
                                       : 0));
    exprtk_map_set(&result, "attributes", parser_attributes_to_map(ud->env, owner));
    return result;
}

static exprtk_value_t parser_schema_type_exists_with_schema(parser_ud_t *ud, Node *schema_root,
                                                            exprtk_value_t type_arg) {
    char *type_name;

    if (!ud || !schema_root || type_arg.type != EXPRTK_VAL_STRING)
        return exprtk_val_int(0);
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return exprtk_val_int(0);
    return exprtk_val_int((parser_find_record(schema_root, type_name) ||
                           parser_find_union(schema_root, type_name) ||
                           parser_is_enum_type(schema_root, type_name))
                              ? 1
                              : 0);
}

static Node *parser_schema_arg_root(parser_ud_t *ud, exprtk_value_t arg, int *owned) {
    int handle;
    if (owned) *owned = 0;
    if (!ud) return NULL;
    if (arg.type == EXPRTK_VAL_STRING) {
        if (owned) *owned = 1;
        return parser_parse_schema_value(arg);
    }
    if (parser_arg_int(arg, &handle))
        return parser_get_schema_handle(ud->ctx, handle);
    return NULL;
}

static DataBind *parser_schema_arg_codec(parser_ud_t *ud, exprtk_value_t arg, int *owned) {
    int handle;
    DataBind *codec = NULL;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus status;

    if (owned) *owned = 0;
    if (!ud) return NULL;
    if (arg.type == EXPRTK_VAL_STRING) {
        status = data_bind_create_from_text(arg.data.string.data, arg.data.string.len, &codec, &err);
        if (status != DATA_BIND_OK || !codec) {
            parser_data_bind_error(ud, status, &err, "schema parse failed");
            return NULL;
        }
        if (owned) *owned = 1;
        return codec;
    }
    if (parser_arg_int(arg, &handle))
        return parser_get_schema_codec(ud->ctx, handle);
    return NULL;
}

static exprtk_value_t parser_schema_types_with_codec(parser_ud_t *ud, DataBind *codec) {
    exprtk_value_t result = exprtk_val_list_empty();
    size_t count;

    if (!ud || !ud->env || !codec) return result;
    count = data_bind_schema_type_count(codec);
    for (size_t i = 0; i < count; ++i) {
        DataBindSchemaType type = DATA_BIND_SCHEMA_TYPE_INIT;
        exprtk_value_t item = exprtk_val_object();
        if (!data_bind_schema_type_at(codec, i, &type) || !type.name) continue;
        parser_map_set_string(ud->env, &item, "name", type.name);
        parser_map_set_string(ud->env, &item, "kind", data_bind_schema_kind_name(type.kind));
        if (type.underlying_type)
            parser_map_set_string(ud->env, &item, "underlying_type", type.underlying_type);
        exprtk_map_set(&item, "field_count", exprtk_val_int((int64_t)type.field_count));
        exprtk_map_set(&item, "item_count", exprtk_val_int((int64_t)type.item_count));
        exprtk_map_set(&item, "has_fixed_block_size",
                       exprtk_val_int(type.has_fixed_block_size ? 1 : 0));
        if (type.has_fixed_block_size)
            exprtk_map_set(&item, "fixed_block_size",
                           exprtk_val_int((int64_t)type.fixed_block_size));
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_fields_with_codec(parser_ud_t *ud, DataBind *codec,
                                                      exprtk_value_t type_arg) {
    exprtk_value_t result = exprtk_val_list_empty();
    char *type_name;
    size_t count;

    if (!ud || !ud->env || !codec || type_arg.type != EXPRTK_VAL_STRING)
        return result;
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    count = data_bind_schema_field_count(codec, type_name);
    for (size_t i = 0; i < count; ++i) {
        DataBindSchemaField field = DATA_BIND_SCHEMA_FIELD_INIT;
        exprtk_value_t item = exprtk_val_object();
        if (!data_bind_schema_field_at(codec, type_name, i, &field) || !field.name) continue;
        parser_map_set_string(ud->env, &item, "name", field.name);
        if (field.type) parser_map_set_string(ud->env, &item, "type", field.type);
        parser_map_set_string(ud->env, &item, "kind", field.kind ? field.kind : "unknown");
        if (field.inner_type) parser_map_set_string(ud->env, &item, "inner_type", field.inner_type);
        if (field.group_type) parser_map_set_string(ud->env, &item, "group_type", field.group_type);
        if (field.key_type) parser_map_set_string(ud->env, &item, "key_type", field.key_type);
        if (field.value_type) parser_map_set_string(ud->env, &item, "value_type", field.value_type);
        if (field.collection_kind)
            parser_map_set_string(ud->env, &item, "collection_kind", field.collection_kind);
        if (field.length) parser_map_set_string(ud->env, &item, "length", field.length);
        exprtk_map_set(&item, "is_optional", exprtk_val_int(field.is_optional ? 1 : 0));
        exprtk_map_set(&item, "has_default", exprtk_val_int(field.has_default ? 1 : 0));
        if (field.default_value) parser_map_set_string(ud->env, &item, "default_value", field.default_value);
        exprtk_map_set(&item, "is_collection", exprtk_val_int(field.is_collection ? 1 : 0));
        exprtk_map_set(&item, "is_composite", exprtk_val_int(field.is_composite ? 1 : 0));
        exprtk_map_set(&item, "is_group", exprtk_val_int(field.is_group ? 1 : 0));
        exprtk_map_set(&item, "is_map", exprtk_val_int(field.is_map ? 1 : 0));
        exprtk_map_set(&item, "is_enum", exprtk_val_int(field.is_enum ? 1 : 0));
        exprtk_map_set(&item, "is_variable_size", exprtk_val_int(field.is_variable_size ? 1 : 0));
        exprtk_map_set(&item, "is_fixed_size", exprtk_val_int(field.is_fixed_size ? 1 : 0));
        if (field.has_offset)
            exprtk_map_set(&item, "offset", exprtk_val_int((int64_t)field.offset));
        if (field.has_size_bytes)
            exprtk_map_set(&item, "size_bytes", exprtk_val_int((int64_t)field.size_bytes));
        if (field.has_field_size_bytes)
            exprtk_map_set(&item, "field_size_bytes",
                           exprtk_val_int((int64_t)field.field_size_bytes));
        if (field.format) parser_map_set_string(ud->env, &item, "format", field.format);
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_enum_items_with_codec(parser_ud_t *ud, DataBind *codec,
                                                          const char *enum_name) {
    exprtk_value_t result = exprtk_val_list_empty();
    size_t count;

    if (!ud || !ud->env || !codec || !enum_name) return result;
    count = data_bind_schema_enum_item_count(codec, enum_name);
    for (size_t i = 0; i < count; ++i) {
        DataBindSchemaEnumItem enum_item = DATA_BIND_SCHEMA_ENUM_ITEM_INIT;
        exprtk_value_t item = exprtk_val_object();
        if (!data_bind_schema_enum_item_at(codec, enum_name, i, &enum_item) || !enum_item.name)
            continue;
        parser_map_set_string(ud->env, &item, "name", enum_item.name);
        if (enum_item.value) parser_map_set_string(ud->env, &item, "value", enum_item.value);
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_enums_with_codec(parser_ud_t *ud, DataBind *codec,
                                                     int flags_only) {
    exprtk_value_t result = exprtk_val_list_empty();
    size_t count;

    if (!ud || !ud->env || !codec) return result;
    count = data_bind_schema_enum_count(codec);
    for (size_t i = 0; i < count; ++i) {
        DataBindSchemaType type = DATA_BIND_SCHEMA_TYPE_INIT;
        exprtk_value_t item = exprtk_val_object();
        if (!data_bind_schema_enum_at(codec, i, &type) || !type.name) continue;
        if (flags_only && type.kind != DATA_BIND_SCHEMA_FLAGS) continue;
        parser_map_set_string(ud->env, &item, "name", type.name);
        if (type.underlying_type)
            parser_map_set_string(ud->env, &item, "underlying_type", type.underlying_type);
        exprtk_map_set(&item, "is_flags",
                       exprtk_val_int(type.kind == DATA_BIND_SCHEMA_FLAGS ? 1 : 0));
        exprtk_map_set(&item, "item_count", exprtk_val_int((int64_t)type.item_count));
        exprtk_map_set(&item, "items", parser_schema_enum_items_with_codec(ud, codec, type.name));
        exprtk_map_set(&item, "attributes", exprtk_val_object());
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_unions_with_codec(parser_ud_t *ud, DataBind *codec) {
    exprtk_value_t result = exprtk_val_list_empty();
    size_t count;

    if (!ud || !ud->env || !codec) return result;
    count = data_bind_schema_type_count(codec);
    for (size_t i = 0; i < count; ++i) {
        DataBindSchemaType type = DATA_BIND_SCHEMA_TYPE_INIT;
        exprtk_value_t item = exprtk_val_object();
        if (!data_bind_schema_type_at(codec, i, &type) || type.kind != DATA_BIND_SCHEMA_UNION ||
            !type.name) continue;
        parser_map_set_string(ud->env, &item, "name", type.name);
        exprtk_map_set(&item, "variant_count", exprtk_val_int((int64_t)type.field_count));
        exprtk_map_set(&item, "variants",
                       parser_schema_fields_with_codec(ud, codec,
                                                       parser_string_value(ud->env, type.name)));
        exprtk_map_set(&item, "attributes", exprtk_val_object());
        exprtk_list_push(&result, item);
    }
    return result;
}

static exprtk_value_t parser_schema_type_exists_with_codec(parser_ud_t *ud, DataBind *codec,
                                                           exprtk_value_t type_arg) {
    char *type_name;
    DataBindSchemaType type = DATA_BIND_SCHEMA_TYPE_INIT;

    if (!ud || !codec || type_arg.type != EXPRTK_VAL_STRING)
        return exprtk_val_int(0);
    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return exprtk_val_int(0);
    return exprtk_val_int(data_bind_schema_find_type(codec, type_name, &type) ? 1 : 0);
}

/* =========================================================================
 * Schema Functions
 * ========================================================================= */

/**
 * schema.parse(schema: string) -> number
 * Parse a TBE schema once and return a module-owned schema handle.
 */
static exprtk_value_t fn_schema_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    tbe_error_t err;
    Node *schema_root;
    DataBind *codec;
    int handle;

    if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud ? ud->ctx : NULL, "expected schema string");
        return exprtk_val_int(-1);
    }

    if (!parser_create_schema_state(args[0], &schema_root, &codec, &err)) {
        parser_set_ctx_error(ud->ctx, parser_schema_error_message(&err));
        return exprtk_val_int(-1);
    }

    handle = parser_alloc_schema_handle(ud->ctx, schema_root, codec);
    if (handle < 0) {
        data_bind_free(codec);
        node_free(schema_root);
        parser_set_ctx_error(ud->ctx, "schema handle limit reached");
        return exprtk_val_int(-1);
    }
    parser_set_ctx_error(ud->ctx, "");
    return exprtk_val_int(handle);
}

static exprtk_value_t parser_schema_parse_detail_map(parser_ud_t *ud, int ok,
                                                     int handle,
                                                     const tbe_error_t *err) {
    exprtk_value_t result = exprtk_val_object();
    exprtk_map_set(&result, "ok", exprtk_val_int(ok ? 1 : 0));
    exprtk_map_set(&result, "handle", exprtk_val_int(handle));
    exprtk_map_set(&result, "line", exprtk_val_int(ok || !err ? -1 : err->line));
    exprtk_map_set(&result, "column", exprtk_val_int(ok || !err ? -1 : err->column));
    exprtk_map_set(&result, "code", exprtk_val_int(ok || !err ? 0 : (int)err->code));
    parser_map_set_string(ud ? ud->env : NULL, &result, "message",
                          ok || !err ? "" : parser_schema_error_message(err));
    return result;
}

/**
 * schema.parse_ex(schema: string) -> object
 */
static exprtk_value_t fn_schema_parse_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    tbe_error_t err;
    Node *schema_root;
    DataBind *codec;
    int handle;

    tbe_error_init(&err);
    if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        tbe_error_set(&err, TBE_ERR_INVALID_ARGUMENT, -1, -1, "expected schema string");
        parser_set_ctx_error(ud ? ud->ctx : NULL, parser_schema_error_message(&err));
        return parser_schema_parse_detail_map(ud, 0, -1, &err);
    }

    if (!parser_create_schema_state(args[0], &schema_root, &codec, &err)) {
        parser_set_ctx_error(ud->ctx, parser_schema_error_message(&err));
        return parser_schema_parse_detail_map(ud, 0, -1, &err);
    }

    handle = parser_alloc_schema_handle(ud->ctx, schema_root, codec);
    if (handle < 0) {
        data_bind_free(codec);
        node_free(schema_root);
        tbe_error_set(&err, TBE_ERR_OUT_OF_MEMORY, -1, -1, "schema handle limit reached");
        parser_set_ctx_error(ud->ctx, parser_schema_error_message(&err));
        return parser_schema_parse_detail_map(ud, 0, -1, &err);
    }
    parser_set_ctx_error(ud->ctx, "");
    return parser_schema_parse_detail_map(ud, 1, handle, NULL);
}

/**
 * schema.error() -> string
 */
static exprtk_value_t fn_schema_error(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    (void)args;
    if (!ud || !ud->ctx || argc != 0)
        return parser_string_value(ud ? ud->env : NULL, "");
    return parser_string_value(ud->env, ud->ctx->error_msg);
}

/**
 * schema.close(handle: number) -> number
 */
static exprtk_value_t fn_schema_close(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || !ud->ctx || argc != 1 || !parser_arg_int(args[0], &handle))
        return PARSER_ZERO;

    parser_free_schema_handle(ud->ctx, handle);
    return PARSER_ZERO;
}

/**
 * schema.types(schema_or_handle) -> list<object>
 */
static exprtk_value_t fn_schema_types(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    codec = parser_schema_arg_codec(ud, args[0], &owned);
    if (codec) result = parser_schema_types_with_codec(ud, codec);
    if (owned) data_bind_free(codec);
    return result;
}

/**
 * schema.fields(schema_or_handle, type: string) -> list<object>
 */
static exprtk_value_t fn_schema_fields(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 2 || args[1].type != EXPRTK_VAL_STRING) return result;
    codec = parser_schema_arg_codec(ud, args[0], &owned);
    if (codec) result = parser_schema_fields_with_codec(ud, codec, args[1]);
    if (owned) data_bind_free(codec);
    return result;
}

/**
 * schema.type_exists(schema_or_handle, type: string) -> number
 */
static exprtk_value_t fn_schema_type_exists(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result = exprtk_val_int(0);
    int owned = 0;

    if (!ud || argc != 2 || args[1].type != EXPRTK_VAL_STRING) return result;
    codec = parser_schema_arg_codec(ud, args[0], &owned);
    if (codec) result = parser_schema_type_exists_with_codec(ud, codec, args[1]);
    if (owned) data_bind_free(codec);
    return result;
}

/**
 * schema.enums(schema_or_handle) -> list<object>
 */
static exprtk_value_t fn_schema_enums(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    codec = parser_schema_arg_codec(ud, args[0], &owned);
    if (codec) result = parser_schema_enums_with_codec(ud, codec, 0);
    if (owned) data_bind_free(codec);
    return result;
}

/**
 * schema.flags(schema_or_handle) -> list<object>
 */
static exprtk_value_t fn_schema_flags(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    codec = parser_schema_arg_codec(ud, args[0], &owned);
    if (codec) result = parser_schema_enums_with_codec(ud, codec, 1);
    if (owned) data_bind_free(codec);
    return result;
}

/**
 * schema.unions(schema_or_handle) -> list<object>
 */
static exprtk_value_t fn_schema_unions(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    codec = parser_schema_arg_codec(ud, args[0], &owned);
    if (codec) result = parser_schema_unions_with_codec(ud, codec);
    if (owned) data_bind_free(codec);
    return result;
}

/**
 * schema.attributes(schema_or_handle[, type: string]) -> object
 */
static exprtk_value_t fn_schema_attributes(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_object();
    int owned = 0;

    if (!ud || (argc != 1 && argc != 2)) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root)
        result = parser_schema_attributes_with_schema(ud, schema_root, argc == 2 ? &args[1] : NULL);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.layout(schema_or_handle[, type: string]) -> object
 */
static exprtk_value_t fn_schema_layout(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_object();
    int owned = 0;

    if (!ud || (argc != 1 && argc != 2)) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root)
        result = parser_schema_layout_with_schema(ud, schema_root, argc == 2 ? &args[1] : NULL);
    if (owned) node_free(schema_root);
    return result;
}

/* =========================================================================
 * JSON Functions
 * ========================================================================= */

/**
 * parser.json_query(json: string, jsonpath: string) -> any
 * Query JSON with TurboNet JSONPath. Multiple matches are returned as a list.
 */
static exprtk_value_t fn_json_query(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_json_path_result_t *matches = NULL;
    exprtk_value_t result = parser_null();
    
    if (!ud || !ud->env || argc < 2 ||
        args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return result;
    }
    
    tstr_v json_sv = args[0].data.string;
    char *jsonpath = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!jsonpath) return result;
    
    json_value_t *root = NULL;
    int rc = turbo_parse_json((const uint8_t *)json_sv.data, json_sv.len, &root);
    
    if (rc != 0 || !root) {
        return result;
    }

    matches = turbo_json_path_query(root, jsonpath);
    if (matches) {
        size_t count = turbo_json_path_result_size(matches);
        if (count == 1) {
            result = parser_json_value_to_expr(ud->env, turbo_json_path_result_get(matches, 0));
        } else if (count > 1) {
            exprtk_value_t list = exprtk_val_list_empty();
            for (size_t i = 0; i < count; ++i) {
                exprtk_list_push(&list,
                                 parser_json_value_to_expr(
                                     ud->env, turbo_json_path_result_get(matches, i)));
            }
            result = list;
        }
        turbo_json_path_result_free(matches);
    }
    
    void *ptr = root;
    turbo_free_json(&ptr);
    
    return result;
}

/**
 * parser.json_query_num(json: string, jsonpath: string, [default: number]) -> number
 * Query the first JSONPath match as a number.
 */
static exprtk_value_t fn_json_query_num(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_json_path_result_t *matches = NULL;
    
    if (!ud || argc < 2 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING) {
        return PARSER_ZERO;
    }
    
    tstr_v json_sv = args[0].data.string;
    char *jsonpath = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!jsonpath) return PARSER_ZERO;
    
    double default_val = 0.0;
    if (argc >= 3) parser_value_as_number(args[2], &default_val);
    
    json_value_t *root = NULL;
    int rc = turbo_parse_json((const uint8_t *)json_sv.data, json_sv.len, &root);
    
    if (rc != 0 || !root) {
        return exprtk_val_num(default_val);
    }
    
    matches = turbo_json_path_query(root, jsonpath);
    json_value_t *val = matches && turbo_json_path_result_size(matches) > 0
                            ? turbo_json_path_result_get(matches, 0)
                            : NULL;
    double result_val = default_val;
    
    if (val) {
        switch (turbo_json_type(val)) {
            case TURBO_JSON_NUMBER:
                result_val = turbo_json_number(val);
                break;
            case TURBO_JSON_BOOL:
                result_val = turbo_json_bool(val) ? 1.0 : 0.0;
                break;
            default:
                break;
        }
    }

    if (matches) turbo_json_path_result_free(matches);
    
    void *ptr = root;
    turbo_free_json(&ptr);
    
    return exprtk_val_num(result_val);
}

/**
 * json.to_vec(json: string, key: string) -> vector
 * 从 JSON 对象数组中提取指定数值字段。
 */
static exprtk_value_t fn_json_to_vec(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;

    if (!ud || !ud->env || argc < 2 ||
        args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return exprtk_val_vec(NULL, 0);
    }

    tstr_v json_sv = args[0].data.string;
    char *key = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!key) return exprtk_val_vec(NULL, 0);

    json_value_t *root = NULL;
    int rc = turbo_parse_json((const uint8_t *)json_sv.data, json_sv.len, &root);
    if (rc != 0 || !root) {
        return exprtk_val_vec(NULL, 0);
    }

    exprtk_value_t result = exprtk_val_vec(NULL, 0);
    if (turbo_json_type(root) == TURBO_JSON_ARRAY) {
        size_t count = turbo_json_array_size(root);
        double *data = count ? (double *)mem_alloc(&ud->env->arena, count * sizeof(double)) : NULL;
        if (count == 0 || data) {
            size_t out_count = 0;
            for (size_t i = 0; i < count; ++i) {
                json_value_t *item = turbo_json_array_get(root, i);
                json_value_t *val = item && turbo_json_type(item) == TURBO_JSON_OBJECT
                                        ? turbo_json_object_get(item, key)
                                        : NULL;
                if (!val) continue;
                if (turbo_json_type(val) == TURBO_JSON_NUMBER) {
                    data[out_count++] = turbo_json_number(val);
                } else if (turbo_json_type(val) == TURBO_JSON_BOOL) {
                    data[out_count++] = turbo_json_bool(val) ? 1.0 : 0.0;
                }
            }
            result = exprtk_val_vec(data, out_count);
        }
    }

    void *ptr = root;
    turbo_free_json(&ptr);
    return result;
}

/**
 * json.parse(json: string) -> object|list|string|number|null
 * Parse JSON and return native TurboScript values.
 */
static exprtk_value_t fn_json_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    json_value_t *root = NULL;
    exprtk_value_t result;
    void *ptr;

    if (!ud || !ud->env || argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return parser_null();
    if (turbo_parse_json((const uint8_t *)args[0].data.string.data,
                         args[0].data.string.len, &root) != 0 || !root) {
        return parser_null();
    }
    result = parser_json_value_to_expr(ud->env, root);
    ptr = root;
    turbo_free_json(&ptr);
    return result;
}

/**
 * json.value(handle: int) -> object|list|string|number|null
 * Convert a parsed JSON document handle into native TurboScript values.
 */
static exprtk_value_t fn_json_value(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;
    json_value_t *root;

    if (!ud || !ud->env || !ud->ctx || argc != 1 || !parser_arg_int(args[0], &handle))
        return parser_null();
    root = parser_get_json_handle(ud->ctx, handle);
    if (!root) return parser_null();
    return parser_json_value_to_expr(ud->env, root);
}

/**
 * json.close(handle: int) -> number
 */
static exprtk_value_t fn_json_close(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || !ud->ctx || argc != 1 || !parser_arg_int(args[0], &handle))
        return PARSER_ZERO;
    parser_free_json_handle(ud->ctx, handle);
    return PARSER_ZERO;
}

/**
 * json.stringify(value: any) -> string
 * Serialize native TurboScript scalar/container values to JSON.
 */
static exprtk_value_t fn_json_stringify(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    parser_filter_buf_t out = {0};
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));

    if (!ud || !ud->env || argc < 1) return result;
    if (parser_json_emit_expr_value(&out, args[0], 0))
        result = parser_buf_to_string(&out, ud->env);
    free(out.data);
    return result;
}

/* =========================================================================
 * XML Functions
 * ========================================================================= */

static exprtk_value_t parser_xml_node_to_map(parser_ud_t *ud,
                                             const turbo_xml_xpath_node_t *node) {
    exprtk_value_t item = exprtk_val_object();
    const char *type;
    const char *name;
    const char *text;
    char *xml;

    if (!ud || !ud->env || !node) return item;

    type = turbo_xml_xpath_node_type_name(node);
    name = turbo_xml_xpath_node_name(node);
    text = turbo_xml_xpath_node_text(node);
    exprtk_map_set(&item, "type", parser_string_value(ud->env, type ? type : ""));
    exprtk_map_set(&item, "name", parser_string_value(ud->env, name ? name : ""));
    exprtk_map_set(&item, "text", parser_string_value(ud->env, text ? text : ""));

    xml = turbo_xml_xpath_node_xml_dup(node);
    exprtk_map_set(&item, "xml", parser_string_value(ud->env, xml ? xml : ""));
    turbo_xml_string_free(xml);
    return item;
}

/**
 * xml.query(xml: string, xpath: string) -> list<object>
 * Query XML nodes with XPath 1.0 and return native plain objects.
 */
static exprtk_value_t fn_xml_query(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t result = exprtk_val_list_empty();
    turbo_xml_doc_t *doc = NULL;
    turbo_xml_list_t nodes;
    char *xpath;

    if (!ud || !ud->env || argc != 2 ||
        args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return result;
    }

    xpath = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!xpath) return result;

    if (turbo_parse_xml((const uint8_t *)args[0].data.string.data,
                        args[0].data.string.len, &doc) != 0 || !doc) {
        return result;
    }

    turbo_xml_xpath_query(doc, xpath, &nodes);
    turbo_xml_for(node, &nodes) {
        exprtk_list_push(&result,
                         parser_xml_node_to_map(ud, (const turbo_xml_xpath_node_t *)node));
    }
    turbo_xml_list_free(&nodes);
    {
        void *ptr = doc;
        turbo_free_xml(&ptr);
    }
    return result;
}

/**
 * xml.text(xml: string, xpath: string) -> string
 * Return textual value of the first XPath match.
 */
static exprtk_value_t fn_xml_text(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_xml_doc_t *doc = NULL;
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    char *xpath;
    const char *text;

    if (!ud || !ud->env || argc != 2 ||
        args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return result;
    }

    xpath = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!xpath) return result;

    if (turbo_parse_xml((const uint8_t *)args[0].data.string.data,
                        args[0].data.string.len, &doc) != 0 || !doc) {
        return result;
    }

    text = turbo_xml_xpath_text(doc, xpath);
    result = parser_string_value(ud->env, text ? text : "");
    {
        void *ptr = doc;
        turbo_free_xml(&ptr);
    }
    return result;
}

/**
 * xml.count(xml: string, xpath: string) -> number
 */
static exprtk_value_t fn_xml_count(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_xml_doc_t *doc = NULL;
    char *xpath;
    size_t count;

    if (!ud || argc != 2 ||
        args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return PARSER_ZERO;
    }

    xpath = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!xpath) return PARSER_ZERO;

    if (turbo_parse_xml((const uint8_t *)args[0].data.string.data,
                        args[0].data.string.len, &doc) != 0 || !doc) {
        return PARSER_ZERO;
    }

    count = turbo_xml_xpath_count(doc, xpath);
    {
        void *ptr = doc;
        turbo_free_xml(&ptr);
    }
    return exprtk_val_num((double)count);
}

/* =========================================================================
 * Stream File Source
 * ========================================================================= */

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

static exprtk_value_t parser_stream_make(exprtk_value_t source) {
    exprtk_value_t stream = exprtk_val_map();
    exprtk_map_set(&stream, "__ts_stream_kind",
                   exprtk_val_str(tstr_v_from_cstr("TurboScript.Stream.v1")));
    exprtk_map_set(&stream, "source", source);
    return stream;
}

static exprtk_value_t parser_stream_make_text(exprtk_value_t text) {
    exprtk_value_t stream = parser_stream_make(text);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("text")));
    exprtk_map_set(&stream, "text_text", text);
    return stream;
}

static exprtk_value_t parser_stream_make_csv(exprtk_value_t source, exprtk_value_t csv_text,
                                             int has_header) {
    exprtk_value_t stream = parser_stream_make(source);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("csv")));
    exprtk_map_set(&stream, "csv_text", csv_text);
    exprtk_map_set(&stream, "csv_has_header", exprtk_val_num(has_header ? 1.0 : 0.0));
    return stream;
}

static exprtk_value_t parser_stream_make_file(exprtk_value_t path) {
    exprtk_value_t stream = parser_stream_make(path);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("file")));
    exprtk_map_set(&stream, "file_path", path);
    return stream;
}

static int parser_stream_path_value(exprtk_value_t stream, exprtk_value_t *out) {
    exprtk_value_t path;
    if (!out || stream.type != EXPRTK_VAL_MAP || !exprtk_map_has(&stream, "file_path"))
        return 0;
    path = exprtk_map_get(&stream, "file_path");
    if (path.type != EXPRTK_VAL_STRING) return 0;
    *out = path;
    return 1;
}

static exprtk_value_t parser_stream_read_file(parser_ud_t *ud, exprtk_value_t path_v) {
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));
    char *path;
    FILE *f;
    long size;
    char *buf;
    size_t nread;

    if (!ud || !ud->env || path_v.type != EXPRTK_VAL_STRING) return result;
    path = parser_arena_cstr(ud->scratch ? ud->scratch : &ud->env->arena, path_v.data.string);
    if (!path) return result;
    f = fopen(path, "rb");
    if (!f) return result;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return result;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return result;
    }
    buf = (char *)mem_alloc(&ud->env->arena, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return result;
    }
    nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, nread));
}

static exprtk_value_t parser_stream_lines_from_string(parser_ud_t *ud, exprtk_value_t text) {
    exprtk_value_t lines = exprtk_val_list_empty();
    size_t start = 0;

    if (!ud || !ud->env || text.type != EXPRTK_VAL_STRING) return lines;
    for (size_t i = 0; i <= text.data.string.len; ++i) {
        if (i == text.data.string.len || text.data.string.data[i] == '\n') {
            size_t end = i;
            char *buf;
            if (end > start && text.data.string.data[end - 1] == '\r') end--;
            buf = (char *)mem_alloc(&ud->env->arena, end - start + 1);
            if (!buf) return lines;
            memcpy(buf, text.data.string.data + start, end - start);
            buf[end - start] = '\0';
            exprtk_list_push(&lines, exprtk_val_str(tstr_v_from_buf(buf, end - start)));
            start = i + 1;
        }
    }
    return lines;
}

static exprtk_value_t parser_stream_source_from_value(exprtk_value_t value) {
    exprtk_value_t values = exprtk_val_list_empty();
    exprtk_map_iter_t it;
    exprtk_value_t item;

    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_VECTOR) return value;
    if (!exprtk_value_is_object_like(&value)) return values;
    it = exprtk_map_iter_begin(&value);
    while (exprtk_map_iter_next(&it, NULL, &item)) exprtk_list_push(&values, item);
    return values;
}

static exprtk_value_t parser_stream_source_from_query(exprtk_value_t value) {
    exprtk_value_t list;
    if (value.type == EXPRTK_VAL_NULL) return exprtk_val_list_empty();
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_VECTOR) return value;
    list = exprtk_val_list_empty();
    exprtk_list_push(&list, value);
    return list;
}

static exprtk_value_t parser_stream_csv_rows_from_text(parser_ud_t *ud, exprtk_value_t csv_text,
                                                       int has_header) {
    exprtk_value_t list = exprtk_val_list_empty();
    parser_csv_headers_t headers = {0};
    turbo_csv_doc_t *doc = NULL;
    turbo_csv_options_t opts = {has_header != 0, ',', '"', true};
    size_t rows;
    size_t cols;

    if (!ud || !ud->env || csv_text.type != EXPRTK_VAL_STRING) return list;
    if (turbo_parse_csv_opts((const uint8_t *)csv_text.data.string.data,
                             csv_text.data.string.len, &opts, &doc) != 0 || !doc) {
        return list;
    }
    if (has_header) (void)parser_csv_parse_header_names(csv_text, &headers);

    rows = turbo_csv_row_count(doc);
    cols = turbo_csv_column_count(doc);
    for (size_t row = 0; row < rows; ++row) {
        exprtk_value_t item = exprtk_val_object();
        for (size_t col = 0; col < cols; ++col) {
            char fallback[32];
            const char *key = NULL;
            const char *cell = turbo_csv_get(doc, row, col);
            if (has_header && col < headers.count) key = headers.names[col];
            snprintf(fallback, sizeof(fallback), "c%zu", col);
            if (!key || !key[0]) key = fallback;
            exprtk_map_set(&item, key, parser_string_value(ud->env, cell ? cell : ""));
        }
        exprtk_list_push(&list, item);
    }
    parser_csv_headers_free(&headers);
    parser_free_csv_doc(doc);
    return list;
}

static exprtk_value_t parser_stream_csv_with_synthetic_header(parser_ud_t *ud,
                                                              exprtk_value_t csv_text) {
    turbo_csv_doc_t *doc = NULL;
    turbo_csv_options_t opts = {false, ',', '"', true};
    size_t cols;
    size_t header_len = 0;
    size_t text_len;
    char *buf;
    char *p;

    if (!ud || !ud->env || csv_text.type != EXPRTK_VAL_STRING) return csv_text;
    if (turbo_parse_csv_opts((const uint8_t *)csv_text.data.string.data,
                             csv_text.data.string.len, &opts, &doc) != 0 || !doc) {
        return csv_text;
    }
    cols = turbo_csv_column_count(doc);
    parser_free_csv_doc(doc);
    if (cols == 0) return csv_text;

    for (size_t col = 0; col < cols; ++col) {
        char name[32];
        int n = snprintf(name, sizeof(name), "%s%zu", col > 0 ? ",c" : "c", col);
        if (n > 0) header_len += (size_t)n;
    }
    text_len = csv_text.data.string.len;
    buf = (char *)mem_alloc(&ud->env->arena, header_len + 1 + text_len + 1);
    if (!buf) return csv_text;
    p = buf;
    for (size_t col = 0; col < cols; ++col) {
        int n = sprintf(p, "%s%zu", col > 0 ? ",c" : "c", col);
        p += n;
    }
    *p++ = '\n';
    memcpy(p, csv_text.data.string.data, text_len);
    p[text_len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, header_len + 1 + text_len));
}

static exprtk_value_t fn_stream_file(size_t argc, exprtk_value_t *args, void *user_data) {
    (void)user_data;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return parser_stream_make(exprtk_val_list_empty());
    return parser_stream_make_file(args[0]);
}

static exprtk_value_t fn_stream_file_text(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t path;
    exprtk_value_t text;
    if (argc != 1 || !parser_stream_path_value(args[0], &path))
        return parser_stream_make_text(exprtk_val_str(tstr_v_from_buf("", 0)));
    text = parser_stream_read_file(ud, path);
    return parser_stream_make_text(text);
}

static exprtk_value_t fn_stream_file_lines(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t path;
    exprtk_value_t text;
    if (argc != 1 || !parser_stream_path_value(args[0], &path))
        return parser_stream_make(exprtk_val_list_empty());
    text = parser_stream_read_file(ud, path);
    return parser_stream_make(parser_stream_lines_from_string(ud, text));
}

static exprtk_value_t fn_stream_file_csv(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t path;
    exprtk_value_t csv_text;
    int has_header = 1;
    if ((argc != 1 && argc != 2) || !parser_stream_path_value(args[0], &path))
        return parser_stream_make(exprtk_val_list_empty());
    if (argc == 2) has_header = parser_value_truthy(args[1]);
    csv_text = parser_stream_read_file(ud, path);
    return parser_stream_make_csv(parser_stream_csv_rows_from_text(ud, csv_text, has_header),
                                  csv_text, has_header);
}

static exprtk_value_t fn_stream_file_json(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t path;
    exprtk_value_t json_text;
    if ((argc != 1 && argc != 2) || !parser_stream_path_value(args[0], &path) ||
        (argc == 2 && args[1].type != EXPRTK_VAL_STRING)) {
        return parser_stream_make(exprtk_val_list_empty());
    }
    json_text = parser_stream_read_file(ud, path);
    if (json_text.type != EXPRTK_VAL_STRING) return parser_stream_make(exprtk_val_list_empty());
    if (argc == 2) {
        exprtk_value_t query_args[2] = {json_text, args[1]};
        return parser_stream_make(parser_stream_source_from_query(
            fn_json_query(2, query_args, user_data)));
    }
    return parser_stream_make(parser_stream_source_from_value(
        fn_json_parse(1, &json_text, user_data)));
}

static exprtk_value_t fn_stream_file_xml(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t path;
    exprtk_value_t xml_text;
    exprtk_value_t query_args[2];
    exprtk_value_t rows;
    if (argc != 2 || !parser_stream_path_value(args[0], &path) ||
        args[1].type != EXPRTK_VAL_STRING) {
        return parser_stream_make(exprtk_val_list_empty());
    }
    xml_text = parser_stream_read_file(ud, path);
    query_args[0] = xml_text;
    query_args[1] = args[1];
    rows = fn_xml_query(2, query_args, user_data);
    return parser_stream_make(rows.type == EXPRTK_VAL_LIST ? rows : exprtk_val_list_empty());
}

static exprtk_value_t fn_stream_csv_filter_expr(size_t argc, exprtk_value_t *args,
                                                void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    exprtk_value_t csv_text;
    exprtk_value_t has_header_v;
    exprtk_value_t table_text;
    exprtk_value_t filter_args[2];
    exprtk_value_t filtered;
    int has_header;

    if (!ud || !ud->env || argc != 2 || !exprtk_value_is_object_like(&args[0]) ||
        args[1].type != EXPRTK_VAL_STRING || !exprtk_map_has(&args[0], "csv_text") ||
        !exprtk_map_has(&args[0], "csv_has_header")) {
        return parser_stream_make(exprtk_val_list_empty());
    }

    csv_text = exprtk_map_get(&args[0], "csv_text");
    has_header_v = exprtk_map_get(&args[0], "csv_has_header");
    has_header = parser_value_truthy(has_header_v);
    table_text = has_header ? csv_text : parser_stream_csv_with_synthetic_header(ud, csv_text);
    filter_args[0] = table_text;
    filter_args[1] = args[1];
    filtered = fn_csv_filter_table_inline(2, filter_args, user_data);
    if (filtered.type != EXPRTK_VAL_STRING)
        return parser_stream_make_csv(exprtk_val_list_empty(), filtered, 1);
    return parser_stream_make_csv(parser_stream_csv_rows_from_text(ud, filtered, 1),
                                  filtered, 1);
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
static exprtk_value_t fn_datetime_parse(size_t argc, exprtk_value_t *args, void *user_data) {
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
static exprtk_value_t fn_datetime_to_time(size_t argc, exprtk_value_t *args, void *user_data) {
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
static exprtk_value_t fn_datetime_format_rfc822(size_t argc, exprtk_value_t *args, void *user_data) {
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

/**
 * json.bind(schema: string, json: string, type: string) -> object
 * Bind a JSON document string to a TBE schema string.
 */
static exprtk_value_t fn_json_bind(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING)
        return parser_null();
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return parser_null();
    result = parser_data_bind_json_text(ud, codec, args[1], args[2], 0);
    data_bind_free(codec);
    return result;
}

/**
 * json.bind_schema(schema_handle: number, json: string, type: string) -> object
 */
static exprtk_value_t fn_json_bind_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int schema_handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &schema_handle))
        return parser_null();
    return parser_data_bind_json_text(
        ud, parser_get_schema_codec(ud->ctx, schema_handle), args[1], args[2], 0);
}

/**
 * json.bind_all(schema: string, json: string, type: string) -> list<object>
 */
static exprtk_value_t fn_json_bind_all(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_list_empty();
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return exprtk_val_list_empty();
    result = parser_data_bind_json_text(ud, codec, args[1], args[2], 1);
    data_bind_free(codec);
    return result;
}

/**
 * json.bind_all_schema(schema_handle: number, json: string, type: string) -> list<object>
 */
static exprtk_value_t fn_json_bind_all_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int schema_handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &schema_handle))
        return exprtk_val_list_empty();
    return parser_data_bind_json_text(
        ud, parser_get_schema_codec(ud->ctx, schema_handle), args[1], args[2], 1);
}

/**
 * json.emit(schema: string, value: object|map|list, type: string) -> string
 */
static exprtk_value_t fn_json_emit(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[2].type != EXPRTK_VAL_STRING) {
        return result;
    }

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_json_emit_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * json.emit_schema(schema_handle: number, value: object|map|list, type: string) -> string
 */
static exprtk_value_t fn_json_emit_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return exprtk_val_str(tstr_v_from_buf("", 0));
    return parser_json_emit_with_schema(ud, parser_get_schema_handle(ud->ctx, handle), args[1], args[2]);
}

/**
 * json.validate(schema: string, json: string, type: string) -> number
 */
static exprtk_value_t fn_json_validate(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        return exprtk_val_int(0);
    }

    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return exprtk_val_int(0);
    result = parser_data_bind_validate_text(ud, codec, args[1], args[2],
                                            data_bind_validate_json);
    data_bind_free(codec);
    return result;
}

/**
 * json.validate_schema(schema_handle: number, json: string, type: string) -> number
 */
static exprtk_value_t fn_json_validate_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return exprtk_val_int(0);
    return parser_data_bind_validate_text(ud, parser_get_schema_codec(ud->ctx, handle),
                                          args[1], args[2], data_bind_validate_json);
}

/**
 * json.validate_ex(schema: string, json: string, type: string) -> object
 */
static exprtk_value_t fn_json_validate_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_object();
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        parser_validate_set_error(&detail, "", "expected (schema, json, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) {
        parser_validate_set_error(&detail, "", "invalid schema");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    result = parser_json_validate_ex_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * json.validate_ex_schema(schema_handle: number, json: string, type: string) -> object
 */
static exprtk_value_t fn_json_validate_ex_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;
    Node *schema_root;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle)) {
        parser_validate_set_error(&detail, "", "expected (schema_handle, json, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    schema_root = parser_get_schema_handle(ud->ctx, handle);
    if (!schema_root) {
        parser_validate_set_error(&detail, "", "unknown schema handle");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    return parser_json_validate_ex_with_schema(ud, schema_root, args[1], args[2]);
}

/**
 * csv.bind(schema: string, csv: string, row: number, type: string) -> object
 */
static exprtk_value_t fn_csv_bind(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 4 || args[0].type != EXPRTK_VAL_STRING)
        return parser_null();
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return parser_null();
    result = parser_data_bind_csv_text(ud, codec, args[1], args[2], args[3], 0);
    data_bind_free(codec);
    return result;
}

/**
 * csv.bind_schema(schema_handle: number, csv: string, row: number, type: string) -> object
 */
static exprtk_value_t fn_csv_bind_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int schema_handle;

    if (!ud || argc != 4 || !parser_arg_int(args[0], &schema_handle))
        return parser_null();
    return parser_data_bind_csv_text(
        ud, parser_get_schema_codec(ud->ctx, schema_handle), args[1], args[2], args[3], 0);
}

/**
 * csv.bind_all(schema: string, csv: string, type: string) -> list<object>
 */
static exprtk_value_t fn_csv_bind_all(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_list_empty();
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return exprtk_val_list_empty();
    result = parser_data_bind_csv_text(ud, codec, args[1], PARSER_ZERO, args[2], 1);
    data_bind_free(codec);
    return result;
}

/**
 * csv.bind_all_schema(schema_handle: number, csv: string, type: string) -> list<object>
 */
static exprtk_value_t fn_csv_bind_all_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int schema_handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &schema_handle))
        return exprtk_val_list_empty();
    return parser_data_bind_csv_text(
        ud, parser_get_schema_codec(ud->ctx, schema_handle), args[1], PARSER_ZERO, args[2], 1);
}

/**
 * csv.emit(schema: string, value: object|map|list, type: string) -> string
 */
static exprtk_value_t fn_csv_emit(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_str(tstr_v_from_buf("", 0));

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[2].type != EXPRTK_VAL_STRING) {
        return result;
    }

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_csv_emit_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * csv.emit_schema(schema_handle: number, value: object|map|list, type: string) -> string
 */
static exprtk_value_t fn_csv_emit_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return exprtk_val_str(tstr_v_from_buf("", 0));
    return parser_csv_emit_with_schema(ud, parser_get_schema_handle(ud->ctx, handle), args[1], args[2]);
}

/**
 * csv.validate(schema: string, csv: string, type: string) -> number
 */
static exprtk_value_t fn_csv_validate(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        return exprtk_val_int(0);
    }

    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return exprtk_val_int(0);
    result = parser_data_bind_validate_text(ud, codec, args[1], args[2], data_bind_validate_csv);
    data_bind_free(codec);
    return result;
}

/**
 * csv.validate_schema(schema_handle: number, csv: string, type: string) -> number
 */
static exprtk_value_t fn_csv_validate_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return exprtk_val_int(0);
    return parser_data_bind_validate_text(
        ud, parser_get_schema_codec(ud->ctx, handle), args[1], args[2],
        data_bind_validate_csv);
}

/**
 * csv.validate_ex(schema: string, csv: string, type: string) -> object
 */
static exprtk_value_t fn_csv_validate_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_object();
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        parser_validate_set_error(&detail, "", "expected (schema, csv, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) {
        parser_validate_set_error(&detail, "", "invalid schema");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    result = parser_csv_validate_ex_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * csv.validate_ex_schema(schema_handle: number, csv: string, type: string) -> object
 */
static exprtk_value_t fn_csv_validate_ex_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle)) {
        parser_validate_set_error(&detail, "", "expected (schema_handle, csv, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    return parser_csv_validate_ex_with_schema(ud, parser_get_schema_handle(ud->ctx, handle),
                                              args[1], args[2]);
}

/**
 * xml.bind(schema: string, xml: string, type: string) -> object
 */
static exprtk_value_t fn_xml_bind(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING)
        return parser_null();
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return parser_null();
    result = parser_data_bind_xml_text(ud, codec, args[1], parser_null(), args[2], 0);
    data_bind_free(codec);
    return result;
}

/**
 * xml.bind_schema(schema_handle: number, xml: string, type: string) -> object
 */
static exprtk_value_t fn_xml_bind_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int schema_handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &schema_handle))
        return parser_null();
    return parser_data_bind_xml_text(
        ud, parser_get_schema_codec(ud->ctx, schema_handle), args[1], parser_null(), args[2], 0);
}

/**
 * xml.bind_all(schema: string, xml: string, xpath: string, type: string) -> list<object>
 */
static exprtk_value_t fn_xml_bind_all(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;

    if (!ud || argc != 4 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_list_empty();
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return exprtk_val_list_empty();
    result = parser_data_bind_xml_text(ud, codec, args[1], args[2], args[3], 1);
    data_bind_free(codec);
    return result;
}

/**
 * xml.bind_all_schema(schema_handle: number, xml: string, xpath: string, type: string) -> list<object>
 */
static exprtk_value_t fn_xml_bind_all_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int schema_handle;

    if (!ud || argc != 4 || !parser_arg_int(args[0], &schema_handle))
        return exprtk_val_list_empty();
    return parser_data_bind_xml_text(
        ud, parser_get_schema_codec(ud->ctx, schema_handle), args[1], args[2], args[3], 1);
}

/**
 * xml.validate(schema: string, xml: string, type: string) -> number
 * xml.validate(schema: string, xml: string, xpath: string, type: string) -> number
 */
static exprtk_value_t fn_xml_validate(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;
    exprtk_value_t xpath = parser_null();
    exprtk_value_t type_arg;

    if (!ud || (argc != 3 && argc != 4) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        (argc == 4 && args[3].type != EXPRTK_VAL_STRING)) {
        return exprtk_val_int(0);
    }
    type_arg = argc == 4 ? args[3] : args[2];
    if (argc == 4) xpath = args[2];
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) return exprtk_val_int(0);
    result = parser_data_bind_validate_xml_text(ud, codec, args[1], xpath, type_arg);
    data_bind_free(codec);
    return result;
}

/**
 * xml.validate_schema(schema_handle: number, xml: string, type: string) -> number
 * xml.validate_schema(schema_handle: number, xml: string, xpath: string, type: string) -> number
 */
static exprtk_value_t fn_xml_validate_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;
    exprtk_value_t xpath = parser_null();
    exprtk_value_t type_arg;

    if (!ud || (argc != 3 && argc != 4) || !parser_arg_int(args[0], &handle) ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        (argc == 4 && args[3].type != EXPRTK_VAL_STRING))
        return exprtk_val_int(0);
    type_arg = argc == 4 ? args[3] : args[2];
    if (argc == 4) xpath = args[2];
    return parser_data_bind_validate_xml_text(
        ud, parser_get_schema_codec(ud->ctx, handle), args[1], xpath, type_arg);
}

/**
 * xml.validate_ex(schema: string, xml: string, type: string) -> object
 * xml.validate_ex(schema: string, xml: string, xpath: string, type: string) -> object
 */
static exprtk_value_t fn_xml_validate_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    DataBind *codec;
    exprtk_value_t result;
    exprtk_value_t xpath = parser_null();
    exprtk_value_t type_arg;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || (argc != 3 && argc != 4) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        (argc == 4 && args[3].type != EXPRTK_VAL_STRING)) {
        parser_validate_set_error(&detail, "", "expected (schema, xml, type) or (schema, xml, xpath, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    type_arg = argc == 4 ? args[3] : args[2];
    if (argc == 4) xpath = args[2];
    codec = parser_data_bind_from_schema_text(ud, args[0]);
    if (!codec) {
        parser_validate_set_error(&detail, "", "invalid schema");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    result = parser_data_bind_validate_xml_detail(ud, codec, args[1], xpath, type_arg);
    data_bind_free(codec);
    return result;
}

/**
 * xml.validate_ex_schema(schema_handle: number, xml: string, type: string) -> object
 * xml.validate_ex_schema(schema_handle: number, xml: string, xpath: string, type: string) -> object
 */
static exprtk_value_t fn_xml_validate_ex_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;
    exprtk_value_t xpath = parser_null();
    exprtk_value_t type_arg;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || (argc != 3 && argc != 4) || !parser_arg_int(args[0], &handle) ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING ||
        (argc == 4 && args[3].type != EXPRTK_VAL_STRING)) {
        parser_validate_set_error(&detail, "", "expected (schema_handle, xml, type) or (schema_handle, xml, xpath, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    type_arg = argc == 4 ? args[3] : args[2];
    if (argc == 4) xpath = args[2];
    return parser_data_bind_validate_xml_detail(
        ud, parser_get_schema_codec(ud->ctx, handle), args[1], xpath, type_arg);
}

/* =========================================================================
 * Module Registration
 * ========================================================================= */

void parser_load(void *p, void *e, void *s) {
    parser_ctx_t *ctx = (parser_ctx_t *)p;
    exprtk_env_t *env = (exprtk_env_t *)e;
    mem_pool_t *scratch = (mem_pool_t *)s;
    
    if (!ctx || !env) return;
    
    /* Allocate user data */
    parser_ud_t *ud = (parser_ud_t *)mem_alloc(&env->arena, sizeof(parser_ud_t));
    if (!ud) return;
    
    ud->ctx = ctx;
    ud->env = env;
    ud->scratch = scratch;

    exprtk_env_register_func(env, "schema.parse", fn_schema_parse, ud);
    exprtk_env_register_func(env, "schema.parse_ex", fn_schema_parse_ex, ud);
    exprtk_env_register_func(env, "schema.error", fn_schema_error, ud);
    exprtk_env_register_func(env, "schema.close", fn_schema_close, ud);
    exprtk_env_register_func(env, "schema.types", fn_schema_types, ud);
    exprtk_env_register_func(env, "schema.fields", fn_schema_fields, ud);
    exprtk_env_register_func(env, "schema.type_exists", fn_schema_type_exists, ud);
    exprtk_env_register_func(env, "schema.enums", fn_schema_enums, ud);
    exprtk_env_register_func(env, "schema.flags", fn_schema_flags, ud);
    exprtk_env_register_func(env, "schema.unions", fn_schema_unions, ud);
    exprtk_env_register_func(env, "schema.attributes", fn_schema_attributes, ud);
    exprtk_env_register_func(env, "schema.layout", fn_schema_layout, ud);
    
    /* Register CSV functions */
    exprtk_env_register_func(env, "parser.csv_parse", fn_csv_parse, ud);
    exprtk_env_register_func(env, "parser.csv_parse_file", fn_csv_parse_file, ud);
    exprtk_env_register_func(env, "parser.csv_rows", fn_csv_rows, ud);
    exprtk_env_register_func(env, "parser.csv_cols", fn_csv_cols, ud);
    exprtk_env_register_func(env, "parser.csv_get", fn_csv_get, ud);
    exprtk_env_register_func(env, "parser.csv_get_num", fn_csv_get_num, ud);
    exprtk_env_register_func(env, "parser.csv_close", fn_csv_close, ud);
    exprtk_env_register_func(env, "csv.rows", fn_csv_rows_inline, ud);
    exprtk_env_register_func(env, "csv.cols", fn_csv_cols_inline, ud);
    exprtk_env_register_func(env, "csv.get", fn_csv_get_inline, ud);
    exprtk_env_register_func(env, "csv.get_num", fn_csv_get_num_inline, ud);
    exprtk_env_register_func(env, "csv.ts_get_num", fn_csv_get_num_inline, ud);
    exprtk_env_register_func(env, "csv.col", fn_csv_col_inline, ud);
    exprtk_env_register_func(env, "csv.filter_count", fn_csv_filter_count_inline, ud);
    exprtk_env_register_func(env, "csv.filter", fn_csv_filter_inline, ud);
    exprtk_env_register_func(env, "csv.filter_table", fn_csv_filter_table_inline, ud);
    exprtk_env_register_func(env, "csv.write", fn_csv_write_inline, ud);
    exprtk_env_register_func(env, "csv.bind", fn_csv_bind, ud);
    exprtk_env_register_func(env, "csv.bind_all", fn_csv_bind_all, ud);
    exprtk_env_register_func(env, "csv.bind_schema", fn_csv_bind_schema, ud);
    exprtk_env_register_func(env, "csv.bind_all_schema", fn_csv_bind_all_schema, ud);
    exprtk_env_register_func(env, "csv.emit", fn_csv_emit, ud);
    exprtk_env_register_func(env, "csv.emit_schema", fn_csv_emit_schema, ud);
    exprtk_env_register_func(env, "csv.validate", fn_csv_validate, ud);
    exprtk_env_register_func(env, "csv.validate_schema", fn_csv_validate_schema, ud);
    exprtk_env_register_func(env, "csv.validate_ex", fn_csv_validate_ex, ud);
    exprtk_env_register_func(env, "csv.validate_ex_schema", fn_csv_validate_ex_schema, ud);
    
    /* Register JSON functions */
    exprtk_env_register_func(env, "parser.json_query", fn_json_query, ud);
    exprtk_env_register_func(env, "parser.json_query_num", fn_json_query_num, ud);
    exprtk_env_register_func(env, "parser.json_to_vec", fn_json_to_vec, ud);
    exprtk_env_register_func(env, "parser.json_parse", fn_json_parse, ud);
    exprtk_env_register_func(env, "parser.json_stringify", fn_json_stringify, ud);
    exprtk_env_register_func(env, "parser.json_value", fn_json_value, ud);
    exprtk_env_register_func(env, "parser.json_close", fn_json_close, ud);
    exprtk_env_register_func(env, "json.query", fn_json_query, ud);
    exprtk_env_register_func(env, "json.query_num", fn_json_query_num, ud);
    exprtk_env_register_func(env, "json.to_vec", fn_json_to_vec, ud);
    exprtk_env_register_func(env, "json.parse", fn_json_parse, ud);
    exprtk_env_register_func(env, "json.stringify", fn_json_stringify, ud);
    exprtk_env_register_func(env, "json.value", fn_json_value, ud);
    exprtk_env_register_func(env, "json.close", fn_json_close, ud);
    exprtk_env_register_func(env, "json.bind", fn_json_bind, ud);
    exprtk_env_register_func(env, "json.bind_all", fn_json_bind_all, ud);
    exprtk_env_register_func(env, "json.bind_schema", fn_json_bind_schema, ud);
    exprtk_env_register_func(env, "json.bind_all_schema", fn_json_bind_all_schema, ud);
    exprtk_env_register_func(env, "json.emit", fn_json_emit, ud);
    exprtk_env_register_func(env, "json.emit_schema", fn_json_emit_schema, ud);
    exprtk_env_register_func(env, "json.validate", fn_json_validate, ud);
    exprtk_env_register_func(env, "json.validate_schema", fn_json_validate_schema, ud);
    exprtk_env_register_func(env, "json.validate_ex", fn_json_validate_ex, ud);
    exprtk_env_register_func(env, "json.validate_ex_schema", fn_json_validate_ex_schema, ud);

    /* Register XML functions */
    exprtk_env_register_func(env, "parser.xml_query", fn_xml_query, ud);
    exprtk_env_register_func(env, "parser.xml_text", fn_xml_text, ud);
    exprtk_env_register_func(env, "parser.xml_count", fn_xml_count, ud);
    exprtk_env_register_func(env, "xml.query", fn_xml_query, ud);
    exprtk_env_register_func(env, "xml.text", fn_xml_text, ud);
    exprtk_env_register_func(env, "xml.count", fn_xml_count, ud);
    exprtk_env_register_func(env, "xml.bind", fn_xml_bind, ud);
    exprtk_env_register_func(env, "xml.bind_all", fn_xml_bind_all, ud);
    exprtk_env_register_func(env, "xml.bind_schema", fn_xml_bind_schema, ud);
    exprtk_env_register_func(env, "xml.bind_all_schema", fn_xml_bind_all_schema, ud);
    exprtk_env_register_func(env, "xml.validate", fn_xml_validate, ud);
    exprtk_env_register_func(env, "xml.validate_schema", fn_xml_validate_schema, ud);
    exprtk_env_register_func(env, "xml.validate_ex", fn_xml_validate_ex, ud);
    exprtk_env_register_func(env, "xml.validate_ex_schema", fn_xml_validate_ex_schema, ud);

    /* Register parser-owned stream file sources */
    exprtk_env_register_func(env, "stream.file", fn_stream_file, ud);
    exprtk_env_register_func(env, "stream.file.text", fn_stream_file_text, ud);
    exprtk_env_register_func(env, "stream.file.lines", fn_stream_file_lines, ud);
    exprtk_env_register_func(env, "stream.file.csv", fn_stream_file_csv, ud);
    exprtk_env_register_func(env, "stream.file.json", fn_stream_file_json, ud);
    exprtk_env_register_func(env, "stream.file.xml", fn_stream_file_xml, ud);
    exprtk_env_register_func(env, "stream.csv.filterExpr", fn_stream_csv_filter_expr, ud);
    exprtk_env_register_func(env, "stream.csv.where", fn_stream_csv_filter_expr, ud);

    /* Register datetime parser functions */
    exprtk_env_register_func(env, "parser.datetime_parse", fn_datetime_parse, ud);
    exprtk_env_register_func(env, "parser.datetime_to_time", fn_datetime_to_time, ud);
    exprtk_env_register_func(env, "parser.datetime_format_rfc822", fn_datetime_format_rfc822, ud);
    exprtk_env_register_func(env, "datetime.parse", fn_datetime_parse, ud);
    exprtk_env_register_func(env, "datetime.to_time", fn_datetime_to_time, ud);
    exprtk_env_register_func(env, "datetime.timestamp", fn_datetime_to_time, ud);
    exprtk_env_register_func(env, "datetime.format_rfc822", fn_datetime_format_rfc822, ud);
}
