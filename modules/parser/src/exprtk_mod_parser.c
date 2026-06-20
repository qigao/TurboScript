/**
 * @file exprtk_mod_parser.c
 * @brief Parser module functions for TurboScript
 */
#include "parser_ctx.h"
#include "node_tree.h"
#include "schema_parser_dsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

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
static int parser_alloc_csv_handle(parser_ctx_t *ctx, turbo_csv_doc_t *doc) {
    for (int i = 0; i < PARSER_MAX_DOCS; i++) {
        if (!ctx->csv_docs[i]) {
            ctx->csv_docs[i] = doc;
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
}

static int parser_alloc_schema_handle(parser_ctx_t *ctx, Node *schema_root) {
    if (!ctx || !schema_root) return -1;
    for (int i = 0; i < PARSER_MAX_SCHEMAS; i++) {
        if (!ctx->schemas[i]) {
            ctx->schemas[i] = (struct turbo_node_s *)schema_root;
            return i;
        }
    }
    return -1;
}

static Node *parser_get_schema_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_SCHEMAS) return NULL;
    return (Node *)ctx->schemas[handle];
}

static void parser_free_schema_handle(parser_ctx_t *ctx, int handle) {
    if (!ctx || handle < 0 || handle >= PARSER_MAX_SCHEMAS) return;
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
    
    int handle = parser_alloc_csv_handle(ud->ctx, doc);
    if (handle < 0) {
        void *ptr = doc;
        turbo_free_csv(&ptr);
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
    free(data);
    
    if (rc != 0 || !doc) {
        return PARSER_ZERO;
    }
    
    int handle = parser_alloc_csv_handle(ud->ctx, doc);
    if (handle < 0) {
        void *ptr = doc;
        turbo_free_csv(&ptr);
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
    char **names;
    size_t count;
    size_t cap;
} parser_csv_headers_t;

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
    return map && key && map->type == EXPRTK_VAL_MAP && exprtk_map_has(map, key);
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
    PARSER_BIND_BOOL,
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
    if (strcmp(type, "string") == 0 || strcmp(type, "bytes") == 0) return PARSER_BIND_STRING;
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

static int parser_parse_bool_text(const char *text) {
    if (!text) return 0;
    return strcmp(text, "true") == 0 || strcmp(text, "1") == 0 || strcmp(text, "yes") == 0;
}

static exprtk_value_t parser_bind_text_value(parser_bind_kind_t kind, const char *text,
                                             exprtk_env_t *env) {
    char *end = NULL;
    errno = 0;
    switch (kind) {
        case PARSER_BIND_STRING:
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
        parser_json_flags_value(root, type_name, value, &integer_value))
        return exprtk_val_int(integer_value);
    if (parser_is_enum_type(root, type_name) &&
        parser_json_enum_value(root, type_name, value, &integer_value))
        return exprtk_val_int(integer_value);
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
    exprtk_value_t result = exprtk_val_list_empty();
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
    exprtk_value_t result = exprtk_val_map();
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
    exprtk_value_t result = exprtk_val_map();
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
    exprtk_value_t result = exprtk_val_map();
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
    exprtk_value_t result = exprtk_val_map();
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
    exprtk_value_t result = exprtk_val_map();
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
        const char *inner_type = parser_node_string(field, "inner_type");
        parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);
        size_t count = 0;
        int fixed_count = 0;
        parser_index_list_t indexes = {0};
        exprtk_value_t list = exprtk_val_list_empty();
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

static int parser_bind_csv_record_at_path(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                          size_t row, exprtk_env_t *env,
                                          const parser_csv_headers_t *headers,
                                          const char *prefix, exprtk_value_t *out) {
    exprtk_value_t result = exprtk_val_map();
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

static int parser_bind_args_valid(size_t argc, exprtk_value_t *args, size_t type_index) {
    return argc > type_index && args[0].type == EXPRTK_VAL_STRING &&
           args[1].type == EXPRTK_VAL_STRING && args[type_index].type == EXPRTK_VAL_STRING;
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

static exprtk_value_t parser_json_bind_with_schema(parser_ud_t *ud, Node *schema_root,
                                                   exprtk_value_t json_arg,
                                                   exprtk_value_t type_arg) {
    json_value_t *json = NULL;
    void *json_ptr = NULL;
    exprtk_value_t result = parser_null();
    char *type_name;

    if (!ud || !ud->env || !schema_root || json_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        return result;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    if (!parser_type_supported(schema_root, type_name))
        return result;

    if (turbo_parse_json((const uint8_t *)json_arg.data.string.data,
                         json_arg.data.string.len, &json) != 0 || !json) {
        return result;
    }
    result = parser_bind_json_typed_value(schema_root, type_name, json, ud->env);

    json_ptr = json;
    turbo_free_json(&json_ptr);
    return result;
}

static exprtk_value_t parser_json_bind_all_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       exprtk_value_t json_arg,
                                                       exprtk_value_t type_arg) {
    json_value_t *json = NULL;
    void *json_ptr = NULL;
    exprtk_value_t result = exprtk_val_list_empty();
    char *type_name;

    if (!ud || !ud->env || !schema_root || json_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        return result;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    if (!parser_type_supported(schema_root, type_name))
        return result;

    if (turbo_parse_json((const uint8_t *)json_arg.data.string.data,
                         json_arg.data.string.len, &json) != 0 || !json) {
        return result;
    }

    if (turbo_json_type(json) == TURBO_JSON_ARRAY) {
        size_t count = turbo_json_array_size(json);
        for (size_t i = 0; i < count; ++i) {
            json_value_t *item = turbo_json_array_get(json, i);
            exprtk_value_t bound = parser_bind_json_typed_value(schema_root, type_name, item, ud->env);
            if (bound.type != EXPRTK_VAL_NULL) exprtk_list_push(&result, bound);
        }
    } else {
        exprtk_value_t bound = parser_bind_json_typed_value(schema_root, type_name, json, ud->env);
        if (bound.type != EXPRTK_VAL_NULL) exprtk_list_push(&result, bound);
    }

    json_ptr = json;
    turbo_free_json(&json_ptr);
    return result;
}

static exprtk_value_t parser_csv_bind_with_schema(parser_ud_t *ud, Node *schema_root,
                                                  exprtk_value_t csv_arg, exprtk_value_t row_arg,
                                                  exprtk_value_t type_arg) {
    Node *record;
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    turbo_csv_doc_t *doc = NULL;
    parser_csv_headers_t headers;
    exprtk_value_t result = parser_null();
    char *type_name;
    int row;

    memset(&headers, 0, sizeof(headers));

    if (!ud || !ud->env || !schema_root || csv_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING || !parser_arg_int(row_arg, &row) || row < 0) {
        return result;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    record = parser_find_record(schema_root, type_name);
    union_node = parser_find_union(schema_root, type_name);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) return result;

    doc = parser_parse_csv_string(csv_arg);
    if (!doc) return result;
    (void)parser_csv_parse_header_names(csv_arg, &headers);
    if (record) {
        result = parser_bind_csv_row(schema_root, record, doc, (size_t)row, ud->env, &headers);
    } else if (union_node) {
        (void)parser_bind_csv_union_at_path(schema_root, union_node, doc, (size_t)row,
                                            ud->env, &headers, "", &result);
    } else {
        (void)parser_bind_csv_scalar_value(schema_root, doc, (size_t)row, type_name,
                                           scalar_kind, ud->env, &result);
    }
    parser_csv_headers_free(&headers);
    parser_free_csv_doc(doc);
    return result;
}

static exprtk_value_t parser_csv_bind_all_with_schema(parser_ud_t *ud, Node *schema_root,
                                                      exprtk_value_t csv_arg,
                                                      exprtk_value_t type_arg) {
    Node *record;
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    turbo_csv_doc_t *doc = NULL;
    parser_csv_headers_t headers;
    exprtk_value_t result = exprtk_val_list_empty();
    char *type_name;

    memset(&headers, 0, sizeof(headers));

    if (!ud || !ud->env || !schema_root || csv_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        return result;
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return result;
    record = parser_find_record(schema_root, type_name);
    union_node = parser_find_union(schema_root, type_name);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) return result;

    doc = parser_parse_csv_string(csv_arg);
    if (!doc) return result;
    (void)parser_csv_parse_header_names(csv_arg, &headers);
    for (size_t i = 0; i < turbo_csv_row_count(doc); ++i) {
        if (record) {
            exprtk_value_t item = parser_bind_csv_row(schema_root, record, doc, i, ud->env, &headers);
            if (item.type != EXPRTK_VAL_NULL) exprtk_list_push(&result, item);
        } else {
            exprtk_value_t item = parser_null();
            if (union_node) {
                if (parser_bind_csv_union_at_path(schema_root, union_node, doc, i, ud->env,
                                                  &headers, "", &item))
                    exprtk_list_push(&result, item);
            } else if (parser_bind_csv_scalar_value(schema_root, doc, i, type_name,
                                                    scalar_kind, ud->env, &item)) {
                exprtk_list_push(&result, item);
            }
        }
    }
    parser_csv_headers_free(&headers);
    parser_free_csv_doc(doc);
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

    switch (kind) {
        case PARSER_BIND_STRING:
            if (value.type == EXPRTK_VAL_STRING)
                return parser_json_emit_string(out, value.data.string.data, value.data.string.len);
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
            return exprtk_val_num(turbo_json_bool(value) ? 1.0 : 0.0);
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
            exprtk_value_t map = exprtk_val_map();
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
        case EXPRTK_VAL_STRING:
            return parser_json_emit_string(out, value.data.string.data, value.data.string.len);
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
        case EXPRTK_VAL_LIST:
            if (!parser_buf_append_char(out, '[')) return 0;
            for (size_t i = 0; i < value.data.list.count; ++i) {
                if (i > 0 && !parser_buf_append_char(out, ',')) return 0;
                if (!parser_json_emit_expr_value(out, value.data.list.items[i], depth + 1)) return 0;
            }
            return parser_buf_append_char(out, ']');
        case EXPRTK_VAL_MAP:
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
    if (value.type == EXPRTK_VAL_LIST) {
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
    if (value.type == EXPRTK_VAL_MAP && value_type) {
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
    if (value.type != EXPRTK_VAL_MAP) return parser_buf_append(out, "{}");

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

    if (!fields || fields->type != NODE_LIST || value.type != EXPRTK_VAL_MAP)
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
    const char *text = "";
    size_t len = 0;
    int needs_quote = 0;

    if (value.type == EXPRTK_VAL_STRING) {
        text = value.data.string.data ? value.data.string.data : "";
        len = value.data.string.len;
    } else if (value.type == EXPRTK_VAL_INTEGER) {
        snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value.data.integer);
        text = num_buf;
        len = strlen(num_buf);
    } else if (value.type == EXPRTK_VAL_NUMBER) {
        snprintf(num_buf, sizeof(num_buf), "%.17g", value.data.number);
        text = num_buf;
        len = strlen(num_buf);
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

        if (!group_record || value.type != EXPRTK_VAL_LIST) return 1;
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

        if (value.type != EXPRTK_VAL_MAP || !value_type) return 1;
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
            count = value.type == EXPRTK_VAL_LIST ? value.data.list.count : 0;
        for (size_t i = 0; i < count; ++i) {
            char item_path[256];
            if (!parser_csv_index_path(item_path, sizeof(item_path), path, i)) return 0;
            if (parser_node_has(field, "collection_element_is_composite")) {
                Node *inner_record = parser_find_record(schema_root, inner_type);
                if (!parser_csv_emit_headers_for_record(schema_root, inner_record, item_path,
                                                        parser_null(), out, first))
                    return 0;
            } else if (parser_find_union(schema_root, inner_type)) {
                exprtk_value_t item = (value.type == EXPRTK_VAL_LIST && i < value.data.list.count)
                                          ? value.data.list.items[i]
                                          : parser_null();
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
        if (value.type == EXPRTK_VAL_MAP && exprtk_map_has(&value, name))
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

    if (!fields || fields->type != NODE_LIST || value.type != EXPRTK_VAL_MAP) return 1;
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

        if (!group_record || value.type != EXPRTK_VAL_LIST) return 1;
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
        if (value.type != EXPRTK_VAL_MAP || !value_type) return 1;
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
            count = value.type == EXPRTK_VAL_LIST ? value.data.list.count : 0;
        for (size_t i = 0; i < count; ++i) {
            exprtk_value_t item = (value.type == EXPRTK_VAL_LIST && i < value.data.list.count)
                                      ? value.data.list.items[i]
                                      : parser_null();
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

    if (!fields || fields->type != NODE_LIST || value.type != EXPRTK_VAL_MAP) return 1;
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
        if (value.type == EXPRTK_VAL_MAP && name && exprtk_map_has(&value, name))
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
        if (value.type == EXPRTK_VAL_MAP && exprtk_map_has(&value, name))
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
            if (current.type != EXPRTK_VAL_LIST) return 0;
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
            if (len == 0 || len >= sizeof(key) || current.type != EXPRTK_VAL_MAP)
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
        if (parser_value_lookup_path(value, headers->names[i], &item) &&
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

    if (value.type != EXPRTK_VAL_LIST) return 0;
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
    if (value.type == EXPRTK_VAL_LIST)
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
    if (value.type == EXPRTK_VAL_LIST) {
        if (!parser_csv_emit_headers_for_union_schema(schema_root, union_node, "", out, &first))
            return 0;
    } else if (!parser_csv_emit_headers_for_union(schema_root, union_node, "", value, out, &first)) {
        return 0;
    }

    if (value.type == EXPRTK_VAL_LIST) {
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
    if (value.type == EXPRTK_VAL_LIST) {
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

    if (kind == PARSER_BIND_STRING) return text != NULL;
    if (!text) return 0;

    errno = 0;
    switch (kind) {
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

static int parser_json_validate_record(Node *schema_root, Node *record, json_value_t *object);
static int parser_json_validate_union(Node *schema_root, Node *union_node, json_value_t *object);

static int parser_json_validate_typed(Node *schema_root, const char *type_name, json_value_t *value) {
    Node *record = parser_find_record(schema_root, type_name);
    Node *union_node;
    parser_bind_kind_t scalar_kind;
    if (!value) return 0;
    if (record)
        return turbo_json_type(value) == TURBO_JSON_OBJECT &&
               parser_json_validate_record(schema_root, record, value);
    union_node = parser_find_union(schema_root, type_name);
    if (union_node)
        return turbo_json_type(value) == TURBO_JSON_OBJECT &&
               parser_json_validate_union(schema_root, union_node, value);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (scalar_kind != PARSER_BIND_UNSUPPORTED)
        return parser_json_schema_scalar_valid(schema_root, type_name, scalar_kind, value);
    return 0;
}

static int parser_json_validate_union(Node *schema_root, Node *union_node, json_value_t *object) {
    const char *variant_name;
    json_value_t *payload;
    Node *variant;
    const char *variant_type;
    parser_bind_kind_t scalar_kind;

    if (!union_node || !object || turbo_json_type(object) != TURBO_JSON_OBJECT ||
        turbo_json_object_size(object) != 1) return 0;
    variant_name = turbo_json_object_key(object, 0);
    payload = turbo_json_object_value(object, 0);
    variant = parser_union_variant(union_node, variant_name);
    variant_type = parser_node_string(variant, "type");
    if (!variant || !variant_type || !payload) return 0;
    if (parser_find_record(schema_root, variant_type) || parser_find_union(schema_root, variant_type))
        return parser_json_validate_typed(schema_root, variant_type, payload);
    scalar_kind = parser_type_kind(schema_root, variant_type);
    return parser_json_schema_scalar_valid(schema_root, variant_type, scalar_kind, payload);
}

static int parser_json_validate_array(Node *schema_root, Node *field, json_value_t *value) {
    const char *inner_type = parser_node_string(field, "inner_type");
    parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);
    size_t expected = 0;

    if (!inner_type || !value || turbo_json_type(value) != TURBO_JSON_ARRAY) return 0;
    if (parser_node_size_value(field, "length_field", &expected) &&
        turbo_json_array_size(value) != expected) {
        return 0;
    }

    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        json_value_t *item = turbo_json_array_get(value, i);
        if (parser_node_has(field, "collection_element_is_composite") ||
            parser_find_union(schema_root, inner_type)) {
            if (!parser_json_validate_typed(schema_root, inner_type, item)) return 0;
        } else if (!parser_json_schema_scalar_valid(schema_root, inner_type, scalar_kind, item)) {
            return 0;
        }
    }
    return 1;
}

static int parser_json_validate_map(Node *schema_root, Node *field, json_value_t *value) {
    const char *value_type = parser_node_string(field, "value_type");
    parser_bind_kind_t value_kind = parser_type_kind(schema_root, value_type);

    if (!value_type || !value || turbo_json_type(value) != TURBO_JSON_OBJECT) return 0;
    for (size_t i = 0; i < turbo_json_object_size(value); ++i) {
        json_value_t *item = turbo_json_object_value(value, i);
        if (parser_find_record(schema_root, value_type) || parser_find_union(schema_root, value_type)) {
            if (!parser_json_validate_typed(schema_root, value_type, item)) return 0;
        } else if (!parser_json_schema_scalar_valid(schema_root, value_type, value_kind, item)) {
            return 0;
        }
    }
    return 1;
}

static int parser_json_validate_group(Node *schema_root, const char *group_type, json_value_t *value) {
    if (!group_type || !value || turbo_json_type(value) != TURBO_JSON_ARRAY) return 0;
    for (size_t i = 0; i < turbo_json_array_size(value); ++i) {
        if (!parser_json_validate_typed(schema_root, group_type, turbo_json_array_get(value, i)))
            return 0;
    }
    return 1;
}

static int parser_json_validate_record(Node *schema_root, Node *record, json_value_t *object) {
    Node *fields = parser_node_child(record, "fields");
    if (!fields || fields->type != NODE_LIST || !object || turbo_json_type(object) != TURBO_JSON_OBJECT)
        return 0;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        const char *field_type = parser_node_string(field, "type");
        json_value_t *value;

        if (!name) continue;
        value = turbo_json_object_get(object, name);
        if (!value) {
            if (parser_field_missing_allowed(field)) continue;
            return 0;
        }

        if (parser_node_has(field, "is_group_field")) {
            if (!parser_json_validate_group(schema_root, parser_node_string(field, "group_type"), value))
                return 0;
        } else if (parser_node_has(field, "is_map")) {
            if (!parser_json_validate_map(schema_root, field, value)) return 0;
        } else if (parser_node_has(field, "is_collection")) {
            if (!parser_json_validate_array(schema_root, field, value)) return 0;
        } else if (parser_node_has(field, "is_composite_ref") && field_type) {
            if (!parser_json_validate_typed(schema_root, field_type, value)) return 0;
        } else if (field_type && parser_find_union(schema_root, field_type)) {
            if (!parser_json_validate_typed(schema_root, field_type, value)) return 0;
        } else if (!parser_json_schema_scalar_valid(schema_root, field_type,
                                                    parser_field_kind(schema_root, field), value)) {
            return 0;
        }
    }
    return 1;
}

static int parser_json_validate_value(Node *schema_root, Node *record, json_value_t *json) {
    if (!json) return 0;
    if (turbo_json_type(json) == TURBO_JSON_ARRAY) {
        for (size_t i = 0; i < turbo_json_array_size(json); ++i) {
            if (!parser_json_validate_record(schema_root, record, turbo_json_array_get(json, i)))
                return 0;
        }
        return 1;
    }
    return parser_json_validate_record(schema_root, record, json);
}

static int parser_json_validate_typed_value(Node *schema_root, const char *type_name, json_value_t *json) {
    if (!json) return 0;
    if (turbo_json_type(json) == TURBO_JSON_ARRAY) {
        for (size_t i = 0; i < turbo_json_array_size(json); ++i) {
            if (!parser_json_validate_typed(schema_root, type_name, turbo_json_array_get(json, i)))
                return 0;
        }
        return 1;
    }
    return parser_json_validate_typed(schema_root, type_name, json);
}

static exprtk_value_t parser_json_validate_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       exprtk_value_t json_arg,
                                                       exprtk_value_t type_arg) {
    char *type_name;
    json_value_t *json = NULL;
    void *json_ptr;
    int ok = 0;

    if (!ud || !schema_root || json_arg.type != EXPRTK_VAL_STRING ||
        type_arg.type != EXPRTK_VAL_STRING) {
        return exprtk_val_int(0);
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return exprtk_val_int(0);
    if (!parser_type_supported(schema_root, type_name))
        return exprtk_val_int(0);

    if (turbo_parse_json((const uint8_t *)json_arg.data.string.data,
                         json_arg.data.string.len, &json) == 0 && json) {
        ok = parser_json_validate_typed_value(schema_root, type_name, json);
    }
    json_ptr = json;
    if (json_ptr) turbo_free_json(&json_ptr);
    return exprtk_val_int(ok ? 1 : 0);
}

static int parser_csv_validate_field(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                     size_t row, const parser_csv_headers_t *headers,
                                     const char *path);

static int parser_csv_validate_record_at_path(Node *schema_root, Node *record, turbo_csv_doc_t *doc,
                                              size_t row, const parser_csv_headers_t *headers,
                                              const char *prefix) {
    Node *fields = parser_node_child(record, "fields");
    if (!fields || fields->type != NODE_LIST || !doc || row >= turbo_csv_row_count(doc))
        return 0;

    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *field = fields->data.list.items[i];
        const char *name = parser_node_string(field, "name");
        char path[256];

        if (!name || !parser_csv_join_path(path, sizeof(path), prefix, name)) continue;
        if (!parser_csv_validate_field(schema_root, field, doc, row, headers, path)) return 0;
    }
    return 1;
}

static int parser_csv_validate_union_at_path(Node *schema_root, Node *union_node, turbo_csv_doc_t *doc,
                                             size_t row, const parser_csv_headers_t *headers,
                                             const char *path) {
    Node *fields = parser_node_child(union_node, "fields");
    int matches = 0;

    if (!fields || fields->type != NODE_LIST || !headers || !path) return 0;
    for (size_t i = 0; i < fields->data.list.count; ++i) {
        Node *variant = fields->data.list.items[i];
        const char *name = parser_node_string(variant, "name");
        const char *variant_type = parser_node_string(variant, "type");
        Node *variant_record;
        Node *variant_union;
        parser_bind_kind_t scalar_kind;
        char item_path[256];
        int ok = 0;

        if (!name || !variant_type ||
            !parser_csv_join_path(item_path, sizeof(item_path), path, name)) return 0;
        variant_record = parser_find_record(schema_root, variant_type);
        variant_union = parser_find_union(schema_root, variant_type);
        if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
        if (variant_record) {
            ok = parser_csv_validate_record_at_path(schema_root, variant_record, doc, row,
                                                    headers, item_path);
        } else if (variant_union) {
            ok = parser_csv_validate_union_at_path(schema_root, variant_union, doc, row,
                                                   headers, item_path);
        } else {
            size_t col = 0;
            const char *text;
            scalar_kind = parser_type_kind(schema_root, variant_type);
            if (scalar_kind != PARSER_BIND_UNSUPPORTED &&
                parser_csv_find_path_column(doc, item_path, &col)) {
                text = turbo_csv_get(doc, row, col);
                ok = parser_schema_text_value_valid(schema_root, variant_type, scalar_kind, text);
            }
        }
        if (ok) matches++;
    }
    return matches == 1;
}

static int parser_csv_validate_scalar(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                      size_t row, const char *path) {
    size_t col = 0;
    const char *text;
    parser_bind_kind_t kind = parser_field_kind(schema_root, field);

    if (kind == PARSER_BIND_UNSUPPORTED) return 0;
    if (!parser_csv_find_path_column(doc, path, &col))
        return parser_field_missing_allowed(field) ? 1 : 0;
    text = turbo_csv_get(doc, row, col);
    return parser_schema_text_value_valid(schema_root, parser_node_string(field, "type"), kind, text);
}

static int parser_csv_validate_scalar_value(Node *schema_root, const char *type_name,
                                            parser_bind_kind_t kind,
                                            turbo_csv_doc_t *doc, size_t row) {
    size_t col = 0;
    const char *text;

    if (!doc || !type_name || kind == PARSER_BIND_UNSUPPORTED ||
        row >= turbo_csv_row_count(doc) || !parser_csv_find_value_column(doc, &col)) {
        return 0;
    }
    text = turbo_csv_get(doc, row, col);
    return parser_schema_text_value_valid(schema_root, type_name, kind, text);
}

static int parser_csv_validate_field(Node *schema_root, Node *field, turbo_csv_doc_t *doc,
                                     size_t row, const parser_csv_headers_t *headers,
                                     const char *path) {
    const char *field_type = parser_node_string(field, "type");

    if (parser_node_has(field, "is_group_field")) {
        const char *group_type = parser_node_string(field, "group_type");
        Node *group_record = parser_find_record(schema_root, group_type);
        parser_index_list_t indexes = {0};
        int ok = 1;

        if (!group_record || !parser_csv_collect_group_indexes(headers, path, &indexes)) {
            parser_index_list_free(&indexes);
            return parser_field_missing_allowed(field) ? 1 : 0;
        }
        for (size_t i = 0; i < indexes.count; ++i) {
            char item_path[256];
            if (!parser_csv_index_path(item_path, sizeof(item_path), path, indexes.values[i])) {
                ok = 0;
                break;
            }
            if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
            if (!parser_csv_validate_record_at_path(schema_root, group_record, doc, row,
                                                    headers, item_path)) {
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

        if (!value_type || !headers) return 0;
        for (size_t i = 0; i < headers->count; ++i) {
            char key[128];
            char item_path[256];
            if (!parser_csv_header_map_key(headers->names[i], path, key, sizeof(key))) continue;
            if (!parser_csv_join_path(item_path, sizeof(item_path), path, key)) return 0;
            if (!parser_csv_row_has_nonempty_path(doc, row, headers, item_path)) continue;
            any = 1;
            if (value_record) {
                if (!parser_csv_validate_record_at_path(schema_root, value_record, doc, row,
                                                        headers, item_path)) return 0;
            } else if (value_union) {
                if (!parser_csv_validate_union_at_path(schema_root, value_union, doc, row,
                                                       headers, item_path)) return 0;
            } else {
                size_t col = 0;
                const char *text;
                if (value_kind == PARSER_BIND_UNSUPPORTED ||
                    !parser_csv_find_path_column(doc, item_path, &col)) return 0;
                text = turbo_csv_get(doc, row, col);
                if (!parser_schema_text_value_valid(schema_root, value_type, value_kind, text))
                    return 0;
            }
        }
        return any || parser_field_missing_allowed(field);
    }

    if (parser_node_has(field, "is_collection")) {
        const char *inner_type = parser_node_string(field, "inner_type");
        parser_bind_kind_t scalar_kind = parser_type_kind(schema_root, inner_type);
        size_t count = 0;
        parser_index_list_t indexes = {0};
        int fixed_count = 0;
        int ok = 1;

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
            return parser_field_missing_allowed(field) ? 1 : 0;
        }
        for (size_t i = 0; i < indexes.count; ++i) {
            char item_path[256];
            if (!parser_csv_index_path(item_path, sizeof(item_path), path, indexes.values[i])) {
                ok = 0;
                break;
            }
            if (!fixed_count && !parser_csv_row_has_nonempty_path(doc, row, headers, item_path))
                continue;
            if (parser_node_has(field, "collection_element_is_composite")) {
                if (!parser_csv_validate_record_at_path(schema_root, parser_find_record(schema_root, inner_type),
                                                        doc, row, headers, item_path)) {
                    ok = 0;
                    break;
                }
            } else if (parser_find_union(schema_root, inner_type)) {
                if (!parser_csv_validate_union_at_path(schema_root, parser_find_union(schema_root, inner_type),
                                                       doc, row, headers, item_path)) {
                    ok = 0;
                    break;
                }
            } else {
                size_t col = 0;
                const char *text;
                if (!parser_csv_find_path_column(doc, item_path, &col)) {
                    ok = 0;
                    break;
                }
                text = turbo_csv_get(doc, row, col);
                if (!parser_schema_text_value_valid(schema_root, inner_type, scalar_kind, text)) {
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
            return parser_csv_validate_record_at_path(schema_root, inner_record, doc, row, headers, path);
        return 0;
    }
    if (field_type && parser_find_union(schema_root, field_type)) {
        if (parser_field_missing_allowed(field) && !parser_csv_headers_have_path(headers, path))
            return 1;
        return parser_csv_validate_union_at_path(schema_root, parser_find_union(schema_root, field_type),
                                                 doc, row, headers, path);
    }

    return parser_csv_validate_scalar(schema_root, field, doc, row, path);
}

static exprtk_value_t parser_csv_validate_with_schema(parser_ud_t *ud, Node *schema_root,
                                                      exprtk_value_t csv_arg,
                                                      exprtk_value_t type_arg) {
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
        return exprtk_val_int(0);
    }

    type_name = parser_arena_cstr(ud->scratch, type_arg.data.string);
    if (!type_name) return exprtk_val_int(0);
    record = parser_find_record(schema_root, type_name);
    union_node = parser_find_union(schema_root, type_name);
    scalar_kind = parser_type_kind(schema_root, type_name);
    if (!record && !union_node && scalar_kind == PARSER_BIND_UNSUPPORTED) return exprtk_val_int(0);

    doc = parser_parse_csv_string(csv_arg);
    if (!doc) return exprtk_val_int(0);
    (void)parser_csv_parse_header_names(csv_arg, &headers);
    for (size_t row = 0; row < turbo_csv_row_count(doc); ++row) {
        int row_ok = record
                         ? parser_csv_validate_record_at_path(schema_root, record, doc, row, &headers, "")
                         : (union_node
                                ? parser_csv_validate_union_at_path(schema_root, union_node, doc, row, &headers, "")
                                : parser_csv_validate_scalar_value(schema_root, type_name, scalar_kind, doc, row));
        if (!row_ok) {
            ok = 0;
            break;
        }
    }
    parser_csv_headers_free(&headers);
    parser_free_csv_doc(doc);
    return exprtk_val_int(ok ? 1 : 0);
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
        result = exprtk_val_map();
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
    exprtk_value_t result = exprtk_val_map();
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
        case PARSER_BIND_BOOL: return "bool";
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
    exprtk_value_t result = exprtk_val_map();
    exprtk_map_set(&result, "ok", exprtk_val_int(ok ? 1 : 0));
    parser_map_set_string(ud ? ud->env : NULL, &result, "path",
                          ok || !detail ? "" : detail->path);
    parser_map_set_string(ud ? ud->env : NULL, &result, "message",
                          ok || !detail ? "" : detail->message);
    return result;
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
            exprtk_value_t item = exprtk_val_map();

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
        exprtk_value_t item = exprtk_val_map();

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
        exprtk_value_t item = exprtk_val_map();

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
        item = exprtk_val_map();
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
        exprtk_value_t item = exprtk_val_map();

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

    if (!ud || !ud->env || !schema_root) return exprtk_val_map();
    if (type_arg) {
        if (type_arg->type != EXPRTK_VAL_STRING) return exprtk_val_map();
        type_name = parser_arena_cstr(ud->scratch, type_arg->data.string);
        if (!type_name) return exprtk_val_map();
    }
    owner = parser_schema_named_owner(schema_root, type_name);
    return parser_attributes_to_map(ud->env, owner);
}

static exprtk_value_t parser_schema_layout_with_schema(parser_ud_t *ud, Node *schema_root,
                                                       exprtk_value_t *type_arg) {
    exprtk_value_t result = exprtk_val_map();
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
    int handle;

    if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        parser_set_ctx_error(ud ? ud->ctx : NULL, "expected schema string");
        return exprtk_val_int(-1);
    }

    schema_root = parser_parse_schema_value_ex(args[0], &err);
    if (!schema_root) {
        parser_set_ctx_error(ud->ctx, parser_schema_error_message(&err));
        return exprtk_val_int(-1);
    }

    handle = parser_alloc_schema_handle(ud->ctx, schema_root);
    if (handle < 0) {
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
    exprtk_value_t result = exprtk_val_map();
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
 * schema.parse_ex(schema: string) -> map
 */
static exprtk_value_t fn_schema_parse_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    tbe_error_t err;
    Node *schema_root;
    int handle;

    tbe_error_init(&err);
    if (!ud || !ud->ctx || argc != 1 || args[0].type != EXPRTK_VAL_STRING) {
        tbe_error_set(&err, TBE_ERR_INVALID_ARGUMENT, -1, -1, "expected schema string");
        parser_set_ctx_error(ud ? ud->ctx : NULL, parser_schema_error_message(&err));
        return parser_schema_parse_detail_map(ud, 0, -1, &err);
    }

    schema_root = parser_parse_schema_value_ex(args[0], &err);
    if (!schema_root) {
        parser_set_ctx_error(ud->ctx, parser_schema_error_message(&err));
        return parser_schema_parse_detail_map(ud, 0, -1, &err);
    }

    handle = parser_alloc_schema_handle(ud->ctx, schema_root);
    if (handle < 0) {
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
 * schema.types(schema_or_handle) -> list<map>
 */
static exprtk_value_t fn_schema_types(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root) result = parser_schema_types_with_schema(ud, schema_root);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.fields(schema_or_handle, type: string) -> list<map>
 */
static exprtk_value_t fn_schema_fields(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 2 || args[1].type != EXPRTK_VAL_STRING) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root) result = parser_schema_fields_with_schema(ud, schema_root, args[1]);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.type_exists(schema_or_handle, type: string) -> number
 */
static exprtk_value_t fn_schema_type_exists(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_int(0);
    int owned = 0;

    if (!ud || argc != 2 || args[1].type != EXPRTK_VAL_STRING) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root) result = parser_schema_type_exists_with_schema(ud, schema_root, args[1]);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.enums(schema_or_handle) -> list<map>
 */
static exprtk_value_t fn_schema_enums(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root) result = parser_schema_enums_with_schema(ud, schema_root);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.flags(schema_or_handle) -> list<map>
 */
static exprtk_value_t fn_schema_flags(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root) result = parser_schema_flags_with_schema(ud, schema_root);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.unions(schema_or_handle) -> list<map>
 */
static exprtk_value_t fn_schema_unions(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_list_empty();
    int owned = 0;

    if (!ud || argc != 1) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root) result = parser_schema_unions_with_schema(ud, schema_root);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.attributes(schema_or_handle[, type: string]) -> map
 */
static exprtk_value_t fn_schema_attributes(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_map();
    int owned = 0;

    if (!ud || (argc != 1 && argc != 2)) return result;
    schema_root = parser_schema_arg_root(ud, args[0], &owned);
    if (schema_root)
        result = parser_schema_attributes_with_schema(ud, schema_root, argc == 2 ? &args[1] : NULL);
    if (owned) node_free(schema_root);
    return result;
}

/**
 * schema.layout(schema_or_handle[, type: string]) -> map
 */
static exprtk_value_t fn_schema_layout(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root;
    exprtk_value_t result = exprtk_val_map();
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
 * parser.json_query(json: string, key: string) -> string
 * 查询JSON对象字段
 */
static exprtk_value_t fn_json_query(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return PARSER_ZERO;
    }
    
    tstr_v json_sv = args[0].data.string;
    char *key = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!key) return PARSER_ZERO;
    
    json_value_t *root = NULL;
    int rc = turbo_parse_json((const uint8_t *)json_sv.data, json_sv.len, &root);
    
    if (rc != 0 || !root) {
        return PARSER_ZERO;
    }
    
    json_value_t *val = turbo_json_object_get(root, key);
    exprtk_value_t result = PARSER_ZERO;
    
    if (val) {
        switch (turbo_json_type(val)) {
            case TURBO_JSON_STRING: {
                const char *s = turbo_json_string(val);
                if (s) {
                    size_t len = strlen(s);
                    char *buf = (char *)mem_alloc(ud->scratch, len + 1);
                    if (buf) {
                        memcpy(buf, s, len);
                        buf[len] = '\0';
                        result = exprtk_val_str(tstr_v_from_buf(buf, len));
                    }
                }
                break;
            }
            case TURBO_JSON_NUMBER:
                result = exprtk_val_num(turbo_json_number(val));
                break;
            case TURBO_JSON_BOOL:
                result = exprtk_val_num(turbo_json_bool(val) ? 1.0 : 0.0);
                break;
            default:
                break;
        }
    }
    
    void *ptr = root;
    turbo_free_json(&ptr);
    
    return result;
}

/**
 * parser.json_query_num(json: string, key: string, [default: number]) -> number
 * 查询JSON数值字段
 */
static exprtk_value_t fn_json_query_num(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    
    if (argc < 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING) {
        return PARSER_ZERO;
    }
    
    tstr_v json_sv = args[0].data.string;
    char *key = parser_arena_cstr(ud->scratch, args[1].data.string);
    if (!key) return PARSER_ZERO;
    
    double default_val = (argc >= 3 && args[2].type == EXPRTK_VAL_NUMBER) 
                         ? args[2].data.number : 0.0;
    
    json_value_t *root = NULL;
    int rc = turbo_parse_json((const uint8_t *)json_sv.data, json_sv.len, &root);
    
    if (rc != 0 || !root) {
        return exprtk_val_num(default_val);
    }
    
    json_value_t *val = turbo_json_object_get(root, key);
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
 * json.parse(json: string) -> map|list|string|number|null
 * Parse JSON into native TurboScript container values.
 */
static exprtk_value_t fn_json_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    json_value_t *root = NULL;
    void *ptr;
    exprtk_value_t result = parser_null();

    if (!ud || !ud->env || argc < 1 || args[0].type != EXPRTK_VAL_STRING) return result;
    if (turbo_parse_json((const uint8_t *)args[0].data.string.data,
                         args[0].data.string.len, &root) == 0 && root) {
        result = parser_json_value_to_expr(ud->env, root);
    }
    ptr = root;
    if (ptr) turbo_free_json(&ptr);
    return result;
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

static exprtk_value_t parser_datetime_to_map(exprtk_env_t *env, const turbo_datetime_t *dt) {
    exprtk_value_t result = exprtk_val_map();
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
    if (!map || map->type != EXPRTK_VAL_MAP || !key || !out) return 0;
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
    if (!map || map->type != EXPRTK_VAL_MAP || !dt) return 0;
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
 * datetime.parse(text: string) -> map|null
 * Parse common datetime strings into structured fields.
 */
static exprtk_value_t fn_datetime_parse(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    turbo_datetime_t dt;

    if (!ud || !ud->env || argc != 1 || args[0].type != EXPRTK_VAL_STRING) return parser_null();
    if (turbo_parse_datetime(args[0].data.string.data, args[0].data.string.len, &dt) != 0)
        return parser_null();
    return parser_datetime_to_map(ud->env, &dt);
}

/**
 * datetime.to_time(value: string|map) -> number
 * Convert parsed datetime data to seconds since Unix epoch.
 */
static exprtk_value_t fn_datetime_to_time(size_t argc, exprtk_value_t *args, void *user_data) {
    turbo_datetime_t dt;
    time_t ts;
    (void)user_data;

    if (argc != 1) return exprtk_val_num(-1.0);
    if (args[0].type == EXPRTK_VAL_STRING) {
        if (turbo_parse_datetime(args[0].data.string.data, args[0].data.string.len, &dt) != 0)
            return exprtk_val_num(-1.0);
    } else if (args[0].type == EXPRTK_VAL_MAP) {
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
    else
        return exprtk_val_str(tstr_v_from_buf("", 0));

    if (turbo_datetime_format_rfc822(ts, buf, sizeof(buf)) < 0)
        return exprtk_val_str(tstr_v_from_buf("", 0));
    return parser_string_value(ud->env, buf);
}

/**
 * json.bind(schema: string, json: string, type: string) -> map
 * Bind a JSON object to a TBE message/composite shape.
 */
static exprtk_value_t fn_json_bind(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = parser_null();

    if (!ud || !ud->env || !parser_bind_args_valid(argc, args, 2)) return result;

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_json_bind_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * json.bind_schema(schema_handle: number, json: string, type: string) -> map
 */
static exprtk_value_t fn_json_bind_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return parser_null();
    return parser_json_bind_with_schema(ud, parser_get_schema_handle(ud->ctx, handle), args[1], args[2]);
}

/**
 * json.bind_all(schema: string, json_array: string, type: string) -> list<map>
 */
static exprtk_value_t fn_json_bind_all(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_list_empty();

    if (!ud || !ud->env || !parser_bind_args_valid(argc, args, 2)) return result;

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_json_bind_all_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * json.bind_all_schema(schema_handle: number, json_array: string, type: string) -> list<map>
 */
static exprtk_value_t fn_json_bind_all_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return exprtk_val_list_empty();
    return parser_json_bind_all_with_schema(ud, parser_get_schema_handle(ud->ctx, handle), args[1], args[2]);
}

/**
 * json.emit(schema: string, value: map|list, type: string) -> string
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
 * json.emit_schema(schema_handle: number, value: map|list, type: string) -> string
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
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_int(0);

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        return result;
    }

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_json_validate_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
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
    return parser_json_validate_with_schema(ud, parser_get_schema_handle(ud->ctx, handle),
                                            args[1], args[2]);
}

/**
 * json.validate_ex(schema: string, json: string, type: string) -> map
 */
static exprtk_value_t fn_json_validate_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_map();
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
 * json.validate_ex_schema(schema_handle: number, json: string, type: string) -> map
 */
static exprtk_value_t fn_json_validate_ex_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;
    parser_validate_detail_t detail = {{0}, {0}};

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle)) {
        parser_validate_set_error(&detail, "", "expected (schema_handle, json, type)");
        return parser_validate_detail_map(ud, 0, &detail);
    }
    return parser_json_validate_ex_with_schema(ud, parser_get_schema_handle(ud->ctx, handle),
                                               args[1], args[2]);
}

/**
 * csv.bind(schema: string, csv: string, row: number, type: string) -> map
 */
static exprtk_value_t fn_csv_bind(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = parser_null();

    if (!ud || !ud->env || !parser_bind_args_valid(argc, args, 3)) return result;

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_csv_bind_with_schema(ud, schema_root, args[1], args[2], args[3]);
    node_free(schema_root);
    return result;
}

/**
 * csv.bind_schema(schema_handle: number, csv: string, row: number, type: string) -> map
 */
static exprtk_value_t fn_csv_bind_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 4 || !parser_arg_int(args[0], &handle))
        return parser_null();
    return parser_csv_bind_with_schema(ud, parser_get_schema_handle(ud->ctx, handle),
                                       args[1], args[2], args[3]);
}

/**
 * csv.bind_all(schema: string, csv: string, type: string) -> list<map>
 */
static exprtk_value_t fn_csv_bind_all(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_list_empty();

    if (!ud || !ud->env || !parser_bind_args_valid(argc, args, 2)) return result;

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_csv_bind_all_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
    return result;
}

/**
 * csv.bind_all_schema(schema_handle: number, csv: string, type: string) -> list<map>
 */
static exprtk_value_t fn_csv_bind_all_schema(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    int handle;

    if (!ud || argc != 3 || !parser_arg_int(args[0], &handle))
        return exprtk_val_list_empty();
    return parser_csv_bind_all_with_schema(ud, parser_get_schema_handle(ud->ctx, handle),
                                           args[1], args[2]);
}

/**
 * csv.emit(schema: string, value: map|list, type: string) -> string
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
 * csv.emit_schema(schema_handle: number, value: map|list, type: string) -> string
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
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_int(0);

    if (!ud || argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING) {
        return result;
    }

    schema_root = parser_parse_schema_value(args[0]);
    if (!schema_root) return result;
    result = parser_csv_validate_with_schema(ud, schema_root, args[1], args[2]);
    node_free(schema_root);
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
    return parser_csv_validate_with_schema(ud, parser_get_schema_handle(ud->ctx, handle),
                                           args[1], args[2]);
}

/**
 * csv.validate_ex(schema: string, csv: string, type: string) -> map
 */
static exprtk_value_t fn_csv_validate_ex(size_t argc, exprtk_value_t *args, void *user_data) {
    parser_ud_t *ud = (parser_ud_t *)user_data;
    Node *schema_root = NULL;
    exprtk_value_t result = exprtk_val_map();
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
 * csv.validate_ex_schema(schema_handle: number, csv: string, type: string) -> map
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
    exprtk_env_register_func(env, "json.query", fn_json_query, ud);
    exprtk_env_register_func(env, "json.query_num", fn_json_query_num, ud);
    exprtk_env_register_func(env, "json.to_vec", fn_json_to_vec, ud);
    exprtk_env_register_func(env, "json.parse", fn_json_parse, ud);
    exprtk_env_register_func(env, "json.stringify", fn_json_stringify, ud);
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

    /* Register datetime parser functions */
    exprtk_env_register_func(env, "parser.datetime_parse", fn_datetime_parse, ud);
    exprtk_env_register_func(env, "parser.datetime_to_time", fn_datetime_to_time, ud);
    exprtk_env_register_func(env, "parser.datetime_format_rfc822", fn_datetime_format_rfc822, ud);
    exprtk_env_register_func(env, "datetime.parse", fn_datetime_parse, ud);
    exprtk_env_register_func(env, "datetime.to_time", fn_datetime_to_time, ud);
    exprtk_env_register_func(env, "datetime.timestamp", fn_datetime_to_time, ud);
    exprtk_env_register_func(env, "datetime.format_rfc822", fn_datetime_format_rfc822, ud);
}
