/**
 * @file mod_core.c
 * @brief Core language utility module: typeof, type predicates, range.
 *
 * These are universal functions that don't belong to math/string/stats
 * but are fundamental to the language itself.
 */

#include "exprtk_module.h"
#include "exprtk_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int exprtk_is_numeric(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_NUMBER || value.type == EXPRTK_VAL_INTEGER;
}

static double exprtk_numeric_value(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_INTEGER) return (double)value.data.integer;
    if (value.type == EXPRTK_VAL_NUMBER) return value.data.number;
    return 0.0;
}

static exprtk_value_t stream_null_value(void) {
    exprtk_value_t value;
    value.type = EXPRTK_VAL_NULL;
    memset(&value.data, 0, sizeof(value.data));
    return value;
}

static int exprtk_value_truthy(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_NUMBER) return fabs(value.data.number) > 1e-9;
    if (value.type == EXPRTK_VAL_INTEGER) return value.data.integer != 0;
    if (value.type == EXPRTK_VAL_STRING) return value.data.string.len > 0;
    return 0;
}

static exprtk_value_t exprtk_call_callable(exprtk_value_t callable, size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, mem_pool_t *arena) {
    static const char *temp_name = "__ts_pipeline_fn";

    if (!env) return exprtk_val_num(0);

    if (callable.type == EXPRTK_VAL_STRING) {
        char fn_name[256];
        size_t len = callable.data.string.len < sizeof(fn_name) - 1 ? callable.data.string.len
                                                                    : sizeof(fn_name) - 1;
        memcpy(fn_name, callable.data.string.data, len);
        fn_name[len] = '\0';
        return exprtk_call_internal(fn_name, argc, args, env, arena);
    }

    if (callable.type == EXPRTK_VAL_FUNCTION) {
        exprtk_env_set_local(env, temp_name, callable);
        return exprtk_call_internal(temp_name, argc, args, env, arena);
    }

    return exprtk_val_num(0);
}

static exprtk_value_t stream_make(exprtk_value_t source) {
    exprtk_value_t stream = exprtk_val_map();
    exprtk_map_set(&stream, "__ts_stream_kind",
                   exprtk_val_str(tstr_v_from_cstr("TurboScript.Stream.v1")));
    exprtk_map_set(&stream, "source", source);
    return stream;
}

static exprtk_value_t stream_make_csv(exprtk_value_t source, exprtk_value_t csv_text,
                                      int has_header) {
    exprtk_value_t stream = stream_make(source);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("csv")));
    exprtk_map_set(&stream, "csv_text", csv_text);
    exprtk_map_set(&stream, "csv_has_header", exprtk_val_num(has_header ? 1.0 : 0.0));
    return stream;
}

static exprtk_value_t stream_make_text(exprtk_value_t text) {
    exprtk_value_t stream = stream_make(text);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("text")));
    exprtk_map_set(&stream, "text_text", text);
    return stream;
}

static exprtk_value_t stream_make_file(exprtk_value_t path) {
    exprtk_value_t stream = stream_make(path);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("file")));
    exprtk_map_set(&stream, "file_path", path);
    return stream;
}

static int stream_is_value(exprtk_value_t stream) {
    exprtk_value_t kind;
    const char *marker = "TurboScript.Stream.v1";
    size_t marker_len = strlen(marker);

    if (stream.type != EXPRTK_VAL_MAP || !exprtk_map_has(&stream, "__ts_stream_kind") ||
        !exprtk_map_has(&stream, "source")) {
        return 0;
    }
    kind = exprtk_map_get(&stream, "__ts_stream_kind");
    return kind.type == EXPRTK_VAL_STRING &&
           kind.data.string.len == marker_len &&
           memcmp(kind.data.string.data, marker, marker_len) == 0;
}

static exprtk_value_t stream_map_entries(exprtk_value_t source) {
    exprtk_value_t values = exprtk_val_list_empty();
    exprtk_map_iter_t it;
    exprtk_value_t value;

    if (source.type != EXPRTK_VAL_MAP) return values;
    it = exprtk_map_iter_begin(&source);
    while (exprtk_map_iter_next(&it, NULL, &value)) {
        exprtk_list_push(&values, value);
    }
    return values;
}

static exprtk_value_t stream_source_from_value(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_MAP) return stream_map_entries(value);
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_VECTOR) return value;
    return exprtk_val_list_empty();
}

static exprtk_value_t stream_source_from_query_value(exprtk_value_t value) {
    exprtk_value_t list;

    if (value.type == EXPRTK_VAL_NULL) return exprtk_val_list_empty();
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_VECTOR) return value;
    list = exprtk_val_list_empty();
    exprtk_list_push(&list, value);
    return list;
}

static exprtk_value_t stream_of_value(exprtk_value_t value) {
    return stream_make(stream_source_from_value(value));
}

static exprtk_value_t stream_lines_from_string(exprtk_value_t text, mem_pool_t *arena) {
    exprtk_value_t lines = exprtk_val_list_empty();
    size_t start = 0;

    if (text.type != EXPRTK_VAL_STRING || !arena) return lines;
    for (size_t i = 0; i <= text.data.string.len; ++i) {
        if (i == text.data.string.len || text.data.string.data[i] == '\n') {
            size_t end = i;
            if (end > start && text.data.string.data[end - 1] == '\r') end--;
            char *buf = (char *)mem_alloc(arena, end - start + 1);
            if (!buf) return lines;
            memcpy(buf, text.data.string.data + start, end - start);
            buf[end - start] = '\0';
            exprtk_list_push(&lines, exprtk_val_str(tstr_v_from_buf(buf, end - start)));
            start = i + 1;
        }
    }
    return lines;
}

static int stream_list_append_string(exprtk_value_t *list, mem_pool_t *arena,
                                     const char *data, size_t len) {
    char *buf;

    if (!list || !arena || !data) return 0;
    buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return 0;
    memcpy(buf, data, len);
    buf[len] = '\0';
    exprtk_list_push(list, exprtk_val_str(tstr_v_from_buf(buf, len)));
    return 1;
}

static exprtk_value_t stream_split_string(exprtk_value_t text, exprtk_value_t sep,
                                          mem_pool_t *arena) {
    exprtk_value_t parts = exprtk_val_list_empty();
    tstr_v s;
    tstr_v delimiter;
    size_t cursor = 0;

    if (text.type != EXPRTK_VAL_STRING || sep.type != EXPRTK_VAL_STRING || !arena)
        return parts;
    s = text.data.string;
    delimiter = sep.data.string;

    if (delimiter.len == 0) {
        for (size_t i = 0; i < s.len; ++i) {
            if (!stream_list_append_string(&parts, arena, s.data + i, 1)) return exprtk_val_list_empty();
        }
        return parts;
    }

    while (cursor <= s.len) {
        size_t found = TSTR_V_NPOS;
        for (size_t i = cursor; i + delimiter.len <= s.len; ++i) {
            if (memcmp(s.data + i, delimiter.data, delimiter.len) == 0) {
                found = i;
                break;
            }
        }
        if (found == TSTR_V_NPOS) {
            if (!stream_list_append_string(&parts, arena, s.data + cursor, s.len - cursor))
                return exprtk_val_list_empty();
            break;
        }
        if (!stream_list_append_string(&parts, arena, s.data + cursor, found - cursor))
            return exprtk_val_list_empty();
        cursor = found + delimiter.len;
    }
    return parts;
}

static int stream_text_value(exprtk_value_t stream, exprtk_value_t *out) {
    exprtk_value_t text;

    if (!out || !stream_is_value(stream)) return 0;
    if (exprtk_map_has(&stream, "text_text")) {
        text = exprtk_map_get(&stream, "text_text");
        if (text.type == EXPRTK_VAL_STRING) {
            *out = text;
            return 1;
        }
    }
    if (exprtk_map_has(&stream, "source")) {
        text = exprtk_map_get(&stream, "source");
        if (text.type == EXPRTK_VAL_STRING) {
            *out = text;
            return 1;
        }
    }
    return 0;
}

static int stream_file_path_value(exprtk_value_t stream, exprtk_value_t *out) {
    exprtk_value_t path;

    if (!out || !stream_is_value(stream) || !exprtk_map_has(&stream, "file_path"))
        return 0;
    path = exprtk_map_get(&stream, "file_path");
    if (path.type != EXPRTK_VAL_STRING) return 0;
    *out = path;
    return 1;
}

static char *stream_value_to_cstr(exprtk_value_t value, mem_pool_t *arena) {
    char *out;
    if (value.type != EXPRTK_VAL_STRING || !arena) return NULL;
    out = (char *)mem_alloc(arena, value.data.string.len + 1);
    if (!out) return NULL;
    memcpy(out, value.data.string.data, value.data.string.len);
    out[value.data.string.len] = '\0';
    return out;
}

static exprtk_value_t stream_read_file(exprtk_value_t path, exprtk_env_t *env, mem_pool_t *arena) {
    return exprtk_call_internal("read_file", 1, &path, env, arena);
}

static exprtk_value_t stream_json_source(exprtk_value_t path, exprtk_value_t jsonpath,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t json_text = stream_read_file(path, env, arena);
    exprtk_value_t parsed;
    exprtk_value_t value;

    if (json_text.type != EXPRTK_VAL_STRING) return exprtk_val_list_empty();
    if (jsonpath.type == EXPRTK_VAL_STRING) {
        exprtk_value_t query_args[2];
        query_args[0] = json_text;
        query_args[1] = jsonpath;
        value = exprtk_call_internal("json.query", 2, query_args, env, arena);
        return stream_source_from_query_value(value);
    }

    parsed = exprtk_call_internal("json.parse", 1, &json_text, env, arena);
    return stream_source_from_value(parsed);
}

static exprtk_value_t stream_xml_source(exprtk_value_t path, exprtk_value_t xpath,
                                        exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t xml_text;
    exprtk_value_t query_args[2];
    exprtk_value_t rows;

    if (!env || !arena || path.type != EXPRTK_VAL_STRING || xpath.type != EXPRTK_VAL_STRING)
        return exprtk_val_list_empty();
    xml_text = stream_read_file(path, env, arena);
    if (xml_text.type != EXPRTK_VAL_STRING) return exprtk_val_list_empty();
    query_args[0] = xml_text;
    query_args[1] = xpath;
    rows = exprtk_call_internal("xml.query", 2, query_args, env, arena);
    return rows.type == EXPRTK_VAL_LIST ? rows : exprtk_val_list_empty();
}

static exprtk_value_t stream_csv_text_source(exprtk_value_t csv_text, int has_header,
                                             exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t list = exprtk_val_list_empty();
    exprtk_value_t parse_args[2];
    exprtk_value_t handle;
    exprtk_value_t rows_v;
    exprtk_value_t cols_v;
    int rows;
    int cols;
    int start_row;

    if (!env || !arena || csv_text.type != EXPRTK_VAL_STRING) return list;
    parse_args[0] = csv_text;
    parse_args[1] = exprtk_val_num(0.0);
    handle = exprtk_call_internal("parser.csv_parse", 2, parse_args, env, arena);
    rows_v = exprtk_call_internal("parser.csv_rows", 1, &handle, env, arena);
    cols_v = exprtk_call_internal("parser.csv_cols", 1, &handle, env, arena);
    rows = (int)exprtk_numeric_value(rows_v);
    cols = (int)exprtk_numeric_value(cols_v);
    start_row = has_header ? 1 : 0;
    if (rows <= start_row || cols <= 0) {
        (void)exprtk_call_internal("parser.csv_close", 1, &handle, env, arena);
        return list;
    }

    for (int row = start_row; row < rows; ++row) {
        exprtk_value_t item = exprtk_val_map();
        for (int col = 0; col < cols; ++col) {
            exprtk_value_t get_args[3];
            exprtk_value_t header;
            exprtk_value_t cell;
            char fallback[32];
            char *key;

            get_args[0] = handle;
            get_args[1] = exprtk_val_num(0.0);
            get_args[2] = exprtk_val_num((double)col);
            header = has_header ? exprtk_call_internal("parser.csv_get", 3, get_args, env, arena)
                                : exprtk_val_num(0);
            snprintf(fallback, sizeof(fallback), "c%d", col);
            key = header.type == EXPRTK_VAL_STRING ? stream_value_to_cstr(header, arena) : NULL;
            if (!key || key[0] == '\0') key = fallback;

            get_args[1] = exprtk_val_num((double)row);
            cell = exprtk_call_internal("parser.csv_get", 3, get_args, env, arena);
            exprtk_map_set(&item, key, cell);
        }
        exprtk_list_push(&list, item);
    }

    (void)exprtk_call_internal("parser.csv_close", 1, &handle, env, arena);
    return list;
}

static exprtk_value_t stream_csv_source(exprtk_value_t path, int has_header,
                                        exprtk_env_t *env, mem_pool_t *arena,
                                        exprtk_value_t *out_text) {
    exprtk_value_t csv_text;
    if (out_text) *out_text = exprtk_val_str(tstr_v_from_buf("", 0));
    if (!env || !arena || path.type != EXPRTK_VAL_STRING) return exprtk_val_list_empty();
    csv_text = stream_read_file(path, env, arena);
    if (csv_text.type != EXPRTK_VAL_STRING) return exprtk_val_list_empty();
    if (out_text) *out_text = csv_text;
    return stream_csv_text_source(csv_text, has_header, env, arena);
}

static exprtk_value_t stream_csv_with_synthetic_header(exprtk_value_t csv_text,
                                                       exprtk_env_t *env,
                                                       mem_pool_t *arena) {
    exprtk_value_t parse_args[2];
    exprtk_value_t handle;
    exprtk_value_t cols_v;
    int cols;
    size_t header_len = 0;
    size_t text_len;
    char *buf;
    char *p;

    if (!env || !arena || csv_text.type != EXPRTK_VAL_STRING) return csv_text;
    parse_args[0] = csv_text;
    parse_args[1] = exprtk_val_num(0.0);
    handle = exprtk_call_internal("parser.csv_parse", 2, parse_args, env, arena);
    cols_v = exprtk_call_internal("parser.csv_cols", 1, &handle, env, arena);
    cols = (int)exprtk_numeric_value(cols_v);
    (void)exprtk_call_internal("parser.csv_close", 1, &handle, env, arena);
    if (cols <= 0) return csv_text;

    for (int col = 0; col < cols; ++col) {
        char name[32];
        int n = snprintf(name, sizeof(name), "%s%d", col > 0 ? ",c" : "c", col);
        if (n > 0) header_len += (size_t)n;
    }
    text_len = csv_text.data.string.len;
    buf = (char *)mem_alloc(arena, header_len + 1 + text_len + 1);
    if (!buf) return csv_text;
    p = buf;
    for (int col = 0; col < cols; ++col) {
        int n = sprintf(p, "%s%d", col > 0 ? ",c" : "c", col);
        p += n;
    }
    *p++ = '\n';
    memcpy(p, csv_text.data.string.data, text_len);
    p[text_len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, header_len + 1 + text_len));
}

static exprtk_value_t stream_collect_value(exprtk_value_t stream) {
    if (stream.type == EXPRTK_VAL_MAP && exprtk_map_has(&stream, "source"))
        return exprtk_map_get(&stream, "source");
    return exprtk_val_list_empty();
}

static size_t stream_count_source(exprtk_value_t source) {
    if (source.type == EXPRTK_VAL_LIST) return source.data.list.count;
    if (source.type == EXPRTK_VAL_VECTOR) return source.data.vector.size;
    if (source.type == EXPRTK_VAL_MAP) return exprtk_map_count(&source);
    return 0;
}

static exprtk_value_t stream_filter_value(exprtk_value_t stream, exprtk_value_t predicate,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t source = stream_collect_value(stream);

    if (source.type == EXPRTK_VAL_VECTOR) {
        size_t n = source.data.vector.size;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        size_t kept = 0;
        if (!out && n > 0) return exprtk_val_num(0);
        for (size_t i = 0; i < n; ++i) {
            exprtk_value_t arg = exprtk_val_num(source.data.vector.data[i]);
            exprtk_value_t pred = exprtk_call_callable(predicate, 1, &arg, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            if (exprtk_value_truthy(pred)) out[kept++] = source.data.vector.data[i];
        }
        return stream_make(exprtk_val_vec(out, kept));
    }

    if (source.type == EXPRTK_VAL_LIST) {
        exprtk_value_t out = exprtk_val_list_empty();
        for (size_t i = 0; i < source.data.list.count; ++i) {
            exprtk_value_t item = source.data.list.items[i];
            exprtk_value_t pred = exprtk_call_callable(predicate, 1, &item, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            if (exprtk_value_truthy(pred)) exprtk_list_push(&out, item);
        }
        return stream_make(out);
    }

    return stream_make(exprtk_val_list_empty());
}

static exprtk_value_t stream_filter_expr_value(exprtk_value_t stream, exprtk_value_t expr,
                                               exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t csv_text;
    exprtk_value_t has_header_v;
    exprtk_value_t table_text;
    exprtk_value_t filter_args[2];
    exprtk_value_t filtered;
    int has_header;

    if (expr.type != EXPRTK_VAL_STRING || !stream_is_value(stream) ||
        !exprtk_map_has(&stream, "csv_text") || !exprtk_map_has(&stream, "csv_has_header")) {
        return stream_make(exprtk_val_list_empty());
    }

    csv_text = exprtk_map_get(&stream, "csv_text");
    has_header_v = exprtk_map_get(&stream, "csv_has_header");
    has_header = exprtk_value_truthy(has_header_v);
    table_text = has_header ? csv_text : stream_csv_with_synthetic_header(csv_text, env, arena);

    filter_args[0] = table_text;
    filter_args[1] = expr;
    filtered = exprtk_call_internal("csv.filter_table", 2, filter_args, env, arena);
    if (filtered.type != EXPRTK_VAL_STRING) return stream_make_csv(exprtk_val_list_empty(), filtered, 1);
    return stream_make_csv(stream_csv_text_source(filtered, 1, env, arena), filtered, 1);
}

static exprtk_value_t stream_map_value(exprtk_value_t stream, exprtk_value_t mapper,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t source = stream_collect_value(stream);
    exprtk_value_t out = exprtk_val_list_empty();

    if (source.type == EXPRTK_VAL_VECTOR) {
        for (size_t i = 0; i < source.data.vector.size; ++i) {
            exprtk_value_t item = exprtk_val_num(source.data.vector.data[i]);
            exprtk_value_t mapped = exprtk_call_callable(mapper, 1, &item, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            exprtk_list_push(&out, mapped);
        }
        return stream_make(out);
    }

    if (source.type == EXPRTK_VAL_LIST) {
        for (size_t i = 0; i < source.data.list.count; ++i) {
            exprtk_value_t mapped = exprtk_call_callable(mapper, 1, &source.data.list.items[i], env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            exprtk_list_push(&out, mapped);
        }
        return stream_make(out);
    }

    return stream_make(out);
}

static exprtk_value_t stream_reduce_value(exprtk_value_t stream, exprtk_value_t init,
                                          exprtk_value_t reducer, exprtk_env_t *env,
                                          mem_pool_t *arena) {
    exprtk_value_t source = stream_collect_value(stream);
    exprtk_value_t acc = init;
    exprtk_value_t call_args[2];

    if (source.type == EXPRTK_VAL_VECTOR) {
        for (size_t i = 0; i < source.data.vector.size; ++i) {
            call_args[0] = acc;
            call_args[1] = exprtk_val_num(source.data.vector.data[i]);
            acc = exprtk_call_callable(reducer, 2, call_args, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
        }
    } else if (source.type == EXPRTK_VAL_LIST) {
        for (size_t i = 0; i < source.data.list.count; ++i) {
            call_args[0] = acc;
            call_args[1] = source.data.list.items[i];
            acc = exprtk_call_callable(reducer, 2, call_args, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
        }
    }
    return acc;
}

static exprtk_value_t stream_to_list_value(exprtk_value_t stream) {
    exprtk_value_t source = stream_collect_value(stream);
    exprtk_value_t out = exprtk_val_list_empty();

    if (source.type == EXPRTK_VAL_LIST) return source;
    if (source.type == EXPRTK_VAL_VECTOR) {
        for (size_t i = 0; i < source.data.vector.size; ++i)
            exprtk_list_push(&out, exprtk_val_num(source.data.vector.data[i]));
    }
    return out;
}

static exprtk_value_t stream_to_vector_value(exprtk_value_t stream, mem_pool_t *arena) {
    exprtk_value_t source = stream_collect_value(stream);

    if (source.type == EXPRTK_VAL_VECTOR) return source;
    if (source.type == EXPRTK_VAL_LIST) {
        size_t n = source.data.list.count;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        if (!out && n > 0) return exprtk_val_vec(NULL, 0);
        for (size_t i = 0; i < n; ++i)
            out[i] = exprtk_numeric_value(source.data.list.items[i]);
        return exprtk_val_vec(out, n);
    }
    return exprtk_val_vec(NULL, 0);
}

exprtk_value_t exprtk_stream_member_call(exprtk_value_t stream, const char *method, size_t argc,
                                         exprtk_value_t *args, exprtk_env_t *env,
                                         mem_pool_t *arena) {
    exprtk_value_t source;
    exprtk_value_t text;
    exprtk_value_t path;
    if (!method || !stream_is_value(stream))
        return exprtk_val_num(0);

    if (stream_file_path_value(stream, &path)) {
        if (strcmp(method, "text") == 0 && argc == 0) {
            text = stream_read_file(path, env, arena);
            if (text.type != EXPRTK_VAL_STRING)
                text = exprtk_val_str(tstr_v_from_buf("", 0));
            return stream_make_text(text);
        }
        if (strcmp(method, "lines") == 0 && argc == 0) {
            text = stream_read_file(path, env, arena);
            if (text.type != EXPRTK_VAL_STRING) return stream_make(exprtk_val_list_empty());
            return stream_make(stream_lines_from_string(text, arena));
        }
        if (strcmp(method, "csv") == 0 && argc <= 1) {
            int has_header = argc == 1 ? exprtk_value_truthy(args[0]) : 1;
            exprtk_value_t csv_text;
            source = stream_csv_source(path, has_header, env, arena, &csv_text);
            return stream_make_csv(source, csv_text, has_header);
        }
        if (strcmp(method, "json") == 0 && argc <= 1) {
            exprtk_value_t jsonpath = stream_null_value();
            if (argc == 1 && args[0].type != EXPRTK_VAL_STRING)
                return stream_make(exprtk_val_list_empty());
            if (argc == 1) jsonpath = args[0];
            return stream_make(stream_json_source(path, jsonpath, env, arena));
        }
        if (strcmp(method, "xml") == 0 && argc == 1 && args[0].type == EXPRTK_VAL_STRING)
            return stream_make(stream_xml_source(path, args[0], env, arena));
        if (strcmp(method, "xml") == 0)
            return stream_make(exprtk_val_list_empty());
    }

    if (strcmp(method, "lines") == 0 && argc == 0) {
        if (!stream_text_value(stream, &text)) return stream_make(exprtk_val_list_empty());
        return stream_make(stream_lines_from_string(text, arena));
    }
    if (strcmp(method, "split") == 0 && argc == 1) {
        if (!stream_text_value(stream, &text)) return stream_make(exprtk_val_list_empty());
        return stream_make(stream_split_string(text, args[0], arena));
    }
    if (strcmp(method, "filter") == 0 && argc == 1)
        return stream_filter_value(stream, args[0], env, arena);
    if (strcmp(method, "filterExpr") == 0 && argc == 1)
        return stream_filter_expr_value(stream, args[0], env, arena);
    if (strcmp(method, "where") == 0 && argc == 1)
        return stream_filter_expr_value(stream, args[0], env, arena);
    if (strcmp(method, "map") == 0 && argc == 1)
        return stream_map_value(stream, args[0], env, arena);
    if (strcmp(method, "reduce") == 0 && argc == 2)
        return stream_reduce_value(stream, args[0], args[1], env, arena);
    if (strcmp(method, "collect") == 0)
        return stream_collect_value(stream);
    if (strcmp(method, "toList") == 0)
        return stream_to_list_value(stream);
    if (strcmp(method, "toVector") == 0)
        return stream_to_vector_value(stream, arena);
    if (strcmp(method, "count") == 0) {
        source = stream_collect_value(stream);
        return exprtk_val_num((double)stream_count_source(source));
    }
    if (strcmp(method, "forEach") == 0 && argc == 1) {
        source = stream_collect_value(stream);
        if (source.type == EXPRTK_VAL_VECTOR) {
            for (size_t i = 0; i < source.data.vector.size; ++i) {
                exprtk_value_t item = exprtk_val_num(source.data.vector.data[i]);
                (void)exprtk_call_callable(args[0], 1, &item, env, arena);
                if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            }
            return exprtk_val_num((double)source.data.vector.size);
        }
        if (source.type == EXPRTK_VAL_LIST) {
            for (size_t i = 0; i < source.data.list.count; ++i) {
                (void)exprtk_call_callable(args[0], 1, &source.data.list.items[i], env, arena);
                if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            }
            return exprtk_val_num((double)source.data.list.count);
        }
    }

    return exprtk_val_num(0);
}

/* =========================================================================
 * 1. typeof(x) → string
 * ========================================================================= */

static exprtk_value_t fn_typeof(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    const char *name = "null";
    size_t len = 4;

    if (argc > 0) {
        switch (args[0].type) {
            case EXPRTK_VAL_NUMBER: name = "number"; len = 6; break;
            case EXPRTK_VAL_INTEGER: name = "number"; len = 6; break;
            case EXPRTK_VAL_STRING: name = "string"; len = 6; break;
            case EXPRTK_VAL_VECTOR: name = "vector"; len = 6; break;
            case EXPRTK_VAL_MAP:    name = "map";    len = 3; break;
            case EXPRTK_VAL_NULL:   name = "null";   len = 4; break;
            case EXPRTK_VAL_LIST:   name = "list";   len = 4; break;
            case EXPRTK_VAL_FUNCTION: name = "function"; len = 8; break;
            case EXPRTK_VAL_CLASS:  name = "class";  len = 5; break;
            case EXPRTK_VAL_INSTANCE: name = "instance"; len = 8; break;
            case EXPRTK_VAL_BOUND_METHOD: name = "function"; len = 8; break;
            default:                name = "unknown"; len = 7; break;
        }
    }

    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, name, len);
    buf[len] = '\0';
    tstr_v sv;
    sv.data = buf;
    sv.len = len;
    return exprtk_val_str(sv);
}

/* =========================================================================
 * 2. Type predicates: is_number, is_string, is_vector, is_map, is_null
 * ========================================================================= */

static exprtk_value_t fn_is_number(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 &&
                          (args[0].type == EXPRTK_VAL_NUMBER ||
                           args[0].type == EXPRTK_VAL_INTEGER) ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_string(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_STRING ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_vector(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_VECTOR ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_map(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_MAP ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_null(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_NULL ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_list(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_LIST ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_function(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 &&
                          (args[0].type == EXPRTK_VAL_FUNCTION ||
                           args[0].type == EXPRTK_VAL_BOUND_METHOD) ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_class(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_CLASS ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_instance(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_INSTANCE ? 1.0 : 0.0);
}

static int exprtk_string_equals(exprtk_value_t value, const char *text) {
    size_t len = text ? strlen(text) : 0;
    return value.type == EXPRTK_VAL_STRING &&
           value.data.string.len == len &&
           value.data.string.data &&
           memcmp(value.data.string.data, text, len) == 0;
}

static exprtk_value_t fn_await(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc < 1) return exprtk_val_num(0);

    if (args[0].type == EXPRTK_VAL_MAP && exprtk_map_has(&args[0], "status") &&
        exprtk_map_has(&args[0], "value")) {
        exprtk_value_t status = exprtk_map_get(&args[0], "status");
        if (exprtk_string_equals(status, "suspended") ||
            exprtk_string_equals(status, "dead")) {
            return exprtk_map_get(&args[0], "value");
        }
    }

    return args[0];
}

/* =========================================================================
 * 3. range(start, end [, step]) → vector
 * ========================================================================= */

static exprtk_value_t fn_range(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc < 2) return exprtk_val_num(0);
    if (!exprtk_is_numeric(args[0]) || !exprtk_is_numeric(args[1])) return exprtk_val_num(0);

    double start = exprtk_numeric_value(args[0]);
    double end   = exprtk_numeric_value(args[1]);
    double step  = 1.0;

    if (argc >= 3) {
        if (!exprtk_is_numeric(args[2])) return exprtk_val_num(0);
        step = exprtk_numeric_value(args[2]);
        if (fabs(step) < 1e-15) return exprtk_val_num(0);
    }

    /* Validate direction */
    if ((end > start && step < 0) || (end < start && step > 0))
        return exprtk_val_num(0);

    /* Count elements */
    size_t n = 0;
    n = (size_t)(fabs((end - start) / step)) + 1;
    if (n > 100000) n = 100000; /* safety cap */

    double *data = MEM_ALLOC_ARRAY(arena, double, n);
    if (!data) return exprtk_val_num(0);

    for (size_t i = 0; i < n; ++i) {
        data[i] = start + (double)i * step;
    }
    return exprtk_val_vec(data, n);
}

/* =========================================================================
 * 4. print(...) → prints to stdout, returns last arg
 * ========================================================================= */

static exprtk_value_t fn_print(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    for (size_t i = 0; i < argc; ++i) {
        if (i > 0) printf(" ");
        switch (args[i].type) {
            case EXPRTK_VAL_NUMBER: printf("%g", args[i].data.number); break;
            case EXPRTK_VAL_INTEGER: printf("%lld", (long long)args[i].data.integer); break;
            case EXPRTK_VAL_STRING: printf("%.*s", (int)args[i].data.string.len, args[i].data.string.data); break;
            case EXPRTK_VAL_VECTOR:
                printf("[");
                for (size_t j = 0; j < args[i].data.vector.size; ++j) {
                    if (j > 0) printf(", ");
                    printf("%g", args[i].data.vector.data[j]);
                    if (j >= 9 && args[i].data.vector.size > 10) { printf(", ...(%zu more)", args[i].data.vector.size - 10); break; }
                }
                printf("]");
                break;
            case EXPRTK_VAL_MAP:   printf("{map:%zu}", exprtk_map_count(&args[i])); break;
            case EXPRTK_VAL_NULL:  printf("null"); break;
            default:               printf("?"); break;
        }
    }
    printf("\n");
    return (argc > 0) ? args[argc - 1] : exprtk_val_num(0);
}

/* =========================================================================
 * 5. assert(cond [, msg]) → aborts if cond is falsy
 * ========================================================================= */

static exprtk_value_t fn_assert(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc < 1) return exprtk_val_num(0);
    double cond = exprtk_is_numeric(args[0]) ? exprtk_numeric_value(args[0]) : 0;
    if (fabs(cond) < 1e-9) {
        if (argc >= 2 && args[1].type == EXPRTK_VAL_STRING) {
            fprintf(stderr, "Assertion failed: %.*s\n", (int)args[1].data.string.len, args[1].data.string.data);
        } else {
            fprintf(stderr, "Assertion failed\n");
        }
        if (env) env->aborted = 1;
        return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

/* =========================================================================
 * 6. list(...) → creates a heterogeneous list from arguments
 * ========================================================================= */

static exprtk_value_t fn_list(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_value_t result = exprtk_val_list_empty();
    for (size_t i = 0; i < argc; ++i) {
        exprtk_list_push(&result, args[i]);
    }
    return result;
}

static exprtk_value_t fn_stream_of(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1) return stream_make(exprtk_val_list_empty());
    return stream_of_value(args[0]);
}

static exprtk_value_t fn_stream_lines(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t text;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return stream_make(exprtk_val_list_empty());
    text = stream_read_file(args[0], env, arena);
    if (text.type != EXPRTK_VAL_STRING) return stream_make(exprtk_val_list_empty());
    return stream_make(stream_lines_from_string(text, arena));
}

static exprtk_value_t fn_stream_text(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return stream_make_text(exprtk_val_str(tstr_v_from_buf("", 0)));
    return stream_make_text(args[0]);
}

static exprtk_value_t fn_stream_file(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return stream_make(exprtk_val_list_empty());
    return stream_make_file(args[0]);
}

static exprtk_value_t fn_stream_json(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t jsonpath = stream_null_value();
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING ||
        (argc == 2 && args[1].type != EXPRTK_VAL_STRING))
        return stream_make(exprtk_val_list_empty());
    if (argc == 2) jsonpath = args[1];
    return stream_make(stream_json_source(args[0], jsonpath, env, arena));
}

static exprtk_value_t fn_stream_xml(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return stream_make(exprtk_val_list_empty());
    return stream_make(stream_xml_source(args[0], args[1], env, arena));
}

static exprtk_value_t fn_stream_csv(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    int has_header = 1;
    exprtk_value_t csv_text;
    exprtk_value_t source;
    if (argc < 1 || argc > 2 || args[0].type != EXPRTK_VAL_STRING)
        return stream_make(exprtk_val_list_empty());
    if (argc == 2) has_header = exprtk_value_truthy(args[1]);
    source = stream_csv_source(args[0], has_header, env, arena, &csv_text);
    return stream_make_csv(source, csv_text, has_header);
}

/* =========================================================================
 * 7. Pipeline Primitives (vector-first)
 * ========================================================================= */

static exprtk_value_t fn_map(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, mem_pool_t *arena) {
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR &&
        (args[1].type == EXPRTK_VAL_STRING || args[1].type == EXPRTK_VAL_FUNCTION)) {
        size_t n = args[0].data.vector.size;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        if (!out) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i) {
            exprtk_value_t in = exprtk_val_num(args[0].data.vector.data[i]);
            exprtk_value_t mapped = exprtk_call_callable(args[1], 1, &in, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            out[i] = exprtk_numeric_value(mapped);
        }
        return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_filter(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR &&
        (args[1].type == EXPRTK_VAL_STRING || args[1].type == EXPRTK_VAL_FUNCTION)) {
        size_t n = args[0].data.vector.size;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        size_t kept = 0;
        if (!out && n > 0) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i) {
            exprtk_value_t in = exprtk_val_num(args[0].data.vector.data[i]);
            exprtk_value_t pred = exprtk_call_callable(args[1], 1, &in, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
            if (fabs(exprtk_numeric_value(pred)) > 1e-9) {
                out[kept++] = args[0].data.vector.data[i];
            }
        }
        return exprtk_val_vec(out, kept);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_reduce(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    if (argc == 3 && args[0].type == EXPRTK_VAL_VECTOR &&
        (args[2].type == EXPRTK_VAL_STRING || args[2].type == EXPRTK_VAL_FUNCTION)) {
        exprtk_value_t acc = args[1];
        exprtk_value_t call_args[2];

        for (size_t i = 0; i < args[0].data.vector.size; ++i) {
            call_args[0] = acc;
            call_args[1] = exprtk_val_num(args[0].data.vector.data[i]);
            acc = exprtk_call_callable(args[2], 2, call_args, env, arena);
            if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
        }
        return acc;
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_take(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && exprtk_is_numeric(args[1])) {
        size_t n = args[0].data.vector.size;
        int count = (int)exprtk_numeric_value(args[1]);
        size_t out_n = 0;
        double *out = NULL;

        if (count < 0) count = 0;
        out_n = (size_t)count < n ? (size_t)count : n;
        out = MEM_ALLOC_ARRAY(arena, double, out_n);
        if (!out && out_n > 0) return exprtk_val_num(0);
        if (out_n > 0) memcpy(out, args[0].data.vector.data, out_n * sizeof(double));
        return exprtk_val_vec(out, out_n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_drop(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_VECTOR && exprtk_is_numeric(args[1])) {
        size_t n = args[0].data.vector.size;
        int count = (int)exprtk_numeric_value(args[1]);
        size_t start = 0;
        size_t out_n = 0;
        double *out = NULL;

        if (count < 0) count = 0;
        start = (size_t)count < n ? (size_t)count : n;
        out_n = n - start;
        out = MEM_ALLOC_ARRAY(arena, double, out_n);
        if (!out && out_n > 0) return exprtk_val_num(0);
        if (out_n > 0) memcpy(out, args[0].data.vector.data + start, out_n * sizeof(double));
        return exprtk_val_vec(out, out_n);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_lag(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_VECTOR && exprtk_is_numeric(args[1])) {
        size_t n = args[0].data.vector.size;
        int shift = (int)exprtk_numeric_value(args[1]);
        double fill = (argc == 3 && exprtk_is_numeric(args[2])) ? exprtk_numeric_value(args[2]) : 0.0;
        double *out = MEM_ALLOC_ARRAY(arena, double, n);
        if (!out && n > 0) return exprtk_val_num(0);

        for (size_t i = 0; i < n; ++i) {
            int src = (int)i - shift;
            out[i] = (src >= 0 && src < (int)n) ? args[0].data.vector.data[src] : fill;
        }
        return exprtk_val_vec(out, n);
    }
    return exprtk_val_num(0);
}

static int table_key_from_value(exprtk_value_t value, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    if (value.type == EXPRTK_VAL_STRING) {
        size_t n = value.data.string.len < cap - 1 ? value.data.string.len : cap - 1;
        memcpy(buf, value.data.string.data, n);
        buf[n] = '\0';
        return 1;
    }
    if (exprtk_is_numeric(value)) {
        snprintf(buf, cap, "%.17g", exprtk_numeric_value(value));
        return 1;
    }
    return 0;
}

static int table_arg_key(exprtk_value_t value, char *buf, size_t cap) {
    if (value.type != EXPRTK_VAL_STRING || !buf || cap == 0) return 0;
    size_t n = value.data.string.len < cap - 1 ? value.data.string.len : cap - 1;
    memcpy(buf, value.data.string.data, n);
    buf[n] = '\0';
    return 1;
}

static exprtk_value_t table_clone_map(exprtk_value_t row) {
    exprtk_value_t out = exprtk_val_map();
    if (row.type != EXPRTK_VAL_MAP) return out;
    exprtk_map_iter_t it = exprtk_map_iter_begin(&row);
    const char *key = NULL;
    exprtk_value_t value;
    while (exprtk_map_iter_next(&it, &key, &value)) {
        exprtk_map_set(&out, key, value);
    }
    return out;
}

static exprtk_value_t fn_table_select(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc < 2 || args[0].type != EXPRTK_VAL_LIST) return exprtk_val_num(0);
    exprtk_value_t out = exprtk_val_list_empty();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t row = args[0].data.list.items[i];
        if (row.type != EXPRTK_VAL_MAP) continue;
        exprtk_value_t selected = exprtk_val_map();
        if (argc == 2 && args[1].type == EXPRTK_VAL_LIST) {
            for (size_t f = 0; f < args[1].data.list.count; ++f) {
                char key[128];
                if (table_arg_key(args[1].data.list.items[f], key, sizeof(key)) &&
                    exprtk_map_has(&row, key))
                    exprtk_map_set(&selected, key, exprtk_map_get(&row, key));
            }
        } else {
            for (size_t f = 1; f < argc; ++f) {
                char key[128];
                if (table_arg_key(args[f], key, sizeof(key)) && exprtk_map_has(&row, key))
                    exprtk_map_set(&selected, key, exprtk_map_get(&row, key));
            }
        }
        exprtk_list_push(&out, selected);
    }
    return out;
}

static exprtk_value_t fn_table_filter(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    if (argc != 2 || args[0].type != EXPRTK_VAL_LIST ||
        (args[1].type != EXPRTK_VAL_STRING && args[1].type != EXPRTK_VAL_FUNCTION))
        return exprtk_val_num(0);
    exprtk_value_t out = exprtk_val_list_empty();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t row = args[0].data.list.items[i];
        exprtk_value_t pred = exprtk_call_callable(args[1], 1, &row, env, arena);
        if (env && (env->aborted || env->flow != exprtk_FLOW_NORMAL)) return exprtk_val_num(0);
        if (fabs(exprtk_numeric_value(pred)) > 1e-9)
            exprtk_list_push(&out, table_clone_map(row));
    }
    return out;
}

static int table_op_equals(exprtk_value_t value, const char *text) {
    return value.type == EXPRTK_VAL_STRING && value.data.string.len == strlen(text) &&
           memcmp(value.data.string.data, text, value.data.string.len) == 0;
}

static exprtk_value_t fn_table_groupby(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if ((argc != 3 && argc != 4) || args[0].type != EXPRTK_VAL_LIST)
        return exprtk_val_num(0);
    char key_field[128];
    char value_field[128];
    if (!table_arg_key(args[1], key_field, sizeof(key_field)) ||
        !table_arg_key(args[2], value_field, sizeof(value_field)))
        return exprtk_val_num(0);
    const char *op = "sum";
    if (argc == 4) {
        if (table_op_equals(args[3], "count")) op = "count";
        else if (table_op_equals(args[3], "mean")) op = "mean";
        else if (table_op_equals(args[3], "sum")) op = "sum";
        else return exprtk_val_num(0);
    }
    exprtk_value_t groups = exprtk_val_map();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t row = args[0].data.list.items[i];
        if (row.type != EXPRTK_VAL_MAP || !exprtk_map_has(&row, key_field)) continue;
        char group_key[128];
        if (!table_key_from_value(exprtk_map_get(&row, key_field), group_key, sizeof(group_key)))
            continue;
        exprtk_value_t acc = exprtk_map_has(&groups, group_key) ? exprtk_map_get(&groups, group_key)
                                                                : exprtk_val_map();
        double sum = exprtk_map_has(&acc, "sum") ? exprtk_numeric_value(exprtk_map_get(&acc, "sum")) : 0.0;
        double count = exprtk_map_has(&acc, "count") ? exprtk_numeric_value(exprtk_map_get(&acc, "count")) : 0.0;
        double v = 0.0;
        if (exprtk_map_has(&row, value_field))
            v = exprtk_numeric_value(exprtk_map_get(&row, value_field));
        sum += v;
        count += 1.0;
        exprtk_map_set(&acc, "sum", exprtk_val_num(sum));
        exprtk_map_set(&acc, "count", exprtk_val_num(count));
        exprtk_map_set(&acc, "value",
                       exprtk_val_num(strcmp(op, "count") == 0 ? count :
                                      strcmp(op, "mean") == 0 ? sum / count : sum));
        exprtk_map_set(&groups, group_key, acc);
    }
    return groups;
}

static exprtk_value_t fn_table_join(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 3 || args[0].type != EXPRTK_VAL_LIST || args[1].type != EXPRTK_VAL_LIST)
        return exprtk_val_num(0);
    char key_field[128];
    if (!table_arg_key(args[2], key_field, sizeof(key_field))) return exprtk_val_num(0);
    exprtk_value_t out = exprtk_val_list_empty();
    for (size_t i = 0; i < args[0].data.list.count; ++i) {
        exprtk_value_t left = args[0].data.list.items[i];
        if (left.type != EXPRTK_VAL_MAP || !exprtk_map_has(&left, key_field)) continue;
        char lk[128];
        if (!table_key_from_value(exprtk_map_get(&left, key_field), lk, sizeof(lk))) continue;
        for (size_t j = 0; j < args[1].data.list.count; ++j) {
            exprtk_value_t right = args[1].data.list.items[j];
            if (right.type != EXPRTK_VAL_MAP || !exprtk_map_has(&right, key_field)) continue;
            char rk[128];
            if (!table_key_from_value(exprtk_map_get(&right, key_field), rk, sizeof(rk))) continue;
            if (strcmp(lk, rk) != 0) continue;
            exprtk_value_t row = table_clone_map(left);
            exprtk_map_iter_t it = exprtk_map_iter_begin(&right);
            const char *key = NULL;
            exprtk_value_t value;
            while (exprtk_map_iter_next(&it, &key, &value)) {
                if (strcmp(key, key_field) != 0)
                    exprtk_map_set(&row, key, value);
            }
            exprtk_list_push(&out, row);
        }
    }
    return out;
}

/* =========================================================================
 * 8. Module Definition
 * ========================================================================= */

static const exprtk_func_entry_t core_entries[] = {
    { "assert",    fn_assert },
    { "await",     fn_await },
    { "drop",      fn_drop },
    { "filter",    fn_filter },
    { "is_class",  fn_is_class },
    { "is_function", fn_is_function },
    { "is_instance", fn_is_instance },
    { "is_list",   fn_is_list },
    { "is_map",    fn_is_map },
    { "is_null",   fn_is_null },
    { "is_number", fn_is_number },
    { "is_string", fn_is_string },
    { "is_vector", fn_is_vector },
    { "lag",       fn_lag },
    { "list",      fn_list },
    { "map",       fn_map },
    { "print",     fn_print },
    { "reduce",    fn_reduce },
    { "range",     fn_range },
    { "stream.csv", fn_stream_csv },
    { "stream.file", fn_stream_file },
    { "stream.json", fn_stream_json },
    { "stream.lines", fn_stream_lines },
    { "stream.text", fn_stream_text },
    { "stream.xml", fn_stream_xml },
    { "stream.of", fn_stream_of },
    { "table.filter", fn_table_filter },
    { "table.groupby", fn_table_groupby },
    { "table.join", fn_table_join },
    { "table.select", fn_table_select },
    { "take",      fn_take },
    { "typeof",    fn_typeof },
};

static const exprtk_module_t core_module = {
    "core", core_entries, sizeof(core_entries) / sizeof(core_entries[0])
};

const exprtk_module_t *exprtk_module_core(void) { return &core_module; }
