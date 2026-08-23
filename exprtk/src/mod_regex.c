/**
 * @file mod_regex.c
 * @brief Core regex builtins backed by the local tiny regex engine.
 */
#include "exprtk_module.h"
#include "re.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>

#define EXPRTK_REGEX_MAX_PATTERNS 32
#define EXPRTK_REGEX_FLAG_IGNORE_CASE 1
#define EXPRTK_REGEX_MAX_COUNTED_REPEAT 64

typedef struct {
    char *pattern;
    char *compiled_pattern;
    int flags;
} exprtk_regex_pattern_t;

typedef struct {
    exprtk_regex_pattern_t *patterns[EXPRTK_REGEX_MAX_PATTERNS];
} exprtk_regex_ctx_t;

static exprtk_value_t regex_zero(void) {
    return exprtk_val_num(0.0);
}

static exprtk_value_t regex_null(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static exprtk_env_t *regex_root_env(exprtk_env_t *env) {
    while (env && env->parent) env = env->parent;
    return env;
}

static void regex_set_error(exprtk_env_t *env, const char *msg) {
    if (!env || !msg) return;
    strncpy(env->error_msg, msg, sizeof(env->error_msg) - 1);
    env->error_msg[sizeof(env->error_msg) - 1] = '\0';
    env->aborted = 1;
}

static char *regex_strdup_len(const char *data, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    if (len > 0 && data) memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

static char *regex_arena_string(mem_pool_t *arena, vstr s) {
    char *out = (char *)mem_alloc(arena, s.len + 1);
    if (!out) return NULL;
    if (s.len > 0 && s.data) memcpy(out, s.data, s.len);
    out[s.len] = '\0';
    return out;
}

static int regex_parse_flags(vstr flags) {
    int out = 0;
    for (size_t i = 0; i < flags.len; ++i) {
        if (flags.data[i] == 'i' || flags.data[i] == 'I') {
            out |= EXPRTK_REGEX_FLAG_IGNORE_CASE;
        }
    }
    return out;
}

static char *regex_expand_flags(const char *pattern, int flags) {
    if (!(flags & EXPRTK_REGEX_FLAG_IGNORE_CASE)) return regex_strdup_len(pattern, strlen(pattern));
    size_t len = strlen(pattern);
    char *out = (char *)malloc(len * 6 + 1);
    if (!out) return NULL;
    size_t pos = 0;
    int escaped = 0;
    int in_class = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)pattern[i];
        if (escaped) {
            out[pos++] = (char)c;
            escaped = 0;
            continue;
        }
        if (c == '\\') {
            out[pos++] = (char)c;
            escaped = 1;
            continue;
        }
        if (c == '[') {
            in_class = 1;
            out[pos++] = (char)c;
            continue;
        }
        if (c == ']') {
            in_class = 0;
            out[pos++] = (char)c;
            continue;
        }

        if (in_class && isalpha(c) && i + 2 < len && pattern[i + 1] == '-' &&
            isalpha((unsigned char)pattern[i + 2])) {
            unsigned char end = (unsigned char)pattern[i + 2];
            out[pos++] = (char)c;
            out[pos++] = '-';
            out[pos++] = (char)end;
            out[pos++] = (char)tolower(c);
            out[pos++] = '-';
            out[pos++] = (char)tolower(end);
            out[pos++] = (char)toupper(c);
            out[pos++] = '-';
            out[pos++] = (char)toupper(end);
            i += 2;
        } else if (in_class && isalpha(c)) {
            out[pos++] = (char)c;
            out[pos++] = (char)(islower(c) ? toupper(c) : tolower(c));
        } else if (!in_class && isalpha(c)) {
            out[pos++] = '[';
            out[pos++] = (char)tolower(c);
            out[pos++] = (char)toupper(c);
            out[pos++] = ']';
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos] = '\0';
    return out;
}

static int regex_buf_reserve(char **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) return 1;
    size_t next = *cap ? *cap : 64;
    while (next < needed) {
        if (next > (SIZE_MAX / 2)) return 0;
        next *= 2;
    }
    char *grown = (char *)realloc(*buf, next);
    if (!grown) return 0;
    *buf = grown;
    *cap = next;
    return 1;
}

static int regex_buf_append(char **buf, size_t *len, size_t *cap,
                            const char *data, size_t data_len) {
    if (!regex_buf_reserve(buf, cap, *len + data_len + 1)) return 0;
    if (data_len > 0) memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
    return 1;
}

static size_t regex_atom_end(const char *pattern, size_t start, size_t len) {
    if (start >= len) return start;
    if (pattern[start] == '\\') {
        return start + ((start + 1 < len) ? 2 : 1);
    }
    if (pattern[start] == '[') {
        size_t i = start + 1;
        while (i < len) {
            if (pattern[i] == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (pattern[i] == ']') return i + 1;
            ++i;
        }
        return len;
    }
    if (pattern[start] == '(') {
        size_t i = start + 1;
        int depth = 1;
        while (i < len) {
            if (pattern[i] == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (pattern[i] == '[') {
                i = regex_atom_end(pattern, i, len);
                continue;
            }
            if (pattern[i] == '(') {
                ++depth;
            } else if (pattern[i] == ')') {
                --depth;
                if (depth == 0) return i + 1;
            }
            ++i;
        }
        return len;
    }
    return start + 1;
}

static int regex_parse_exact_repeat(const char *pattern, size_t open, size_t len,
                                    size_t *close_out, int *count_out) {
    size_t i = open + 1;
    int count = 0;
    if (i >= len || !isdigit((unsigned char)pattern[i])) return 0;
    while (i < len && isdigit((unsigned char)pattern[i])) {
        int digit = pattern[i] - '0';
        if (count > (EXPRTK_REGEX_MAX_COUNTED_REPEAT - digit) / 10) return -1;
        count = count * 10 + digit;
        ++i;
    }
    if (i >= len || pattern[i] != '}') return 0;
    *close_out = i;
    *count_out = count;
    return 1;
}

static char *regex_expand_counted_repeats(const char *pattern, exprtk_env_t *env) {
    size_t len = strlen(pattern);
    char *out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    size_t i = 0;

    while (i < len) {
        size_t atom_start = out_len;
        size_t atom_end = regex_atom_end(pattern, i, len);
        size_t atom_len = atom_end - i;
        if (!regex_buf_append(&out, &out_len, &out_cap, pattern + i, atom_len)) {
            free(out);
            regex_set_error(env, "regex: out of memory");
            return NULL;
        }
        i = atom_end;

        if (i < len && pattern[i] == '{') {
            size_t repeat_close = 0;
            int repeat_count = 0;
            int repeat_status = regex_parse_exact_repeat(pattern, i, len,
                                                         &repeat_close, &repeat_count);
            if (repeat_status < 0) {
                free(out);
                regex_set_error(env, "regex: counted repeat is too large");
                return NULL;
            }
            if (repeat_status > 0) {
                if (repeat_count == 0) {
                    out_len = atom_start;
                    out[out_len] = '\0';
                } else {
                    size_t extra = atom_len * (size_t)(repeat_count - 1);
                    if (atom_len != 0 && extra / atom_len != (size_t)(repeat_count - 1)) {
                        free(out);
                        regex_set_error(env, "regex: counted repeat is too large");
                        return NULL;
                    }
                    if (!regex_buf_reserve(&out, &out_cap, out_len + extra + 1)) {
                        free(out);
                        regex_set_error(env, "regex: out of memory");
                        return NULL;
                    }
                    for (int n = 1; n < repeat_count; ++n) {
                        memcpy(out + out_len, out + atom_start, atom_len);
                        out_len += atom_len;
                        out[out_len] = '\0';
                    }
                }
                i = repeat_close + 1;
            }
        }
    }

    if (!out) return regex_strdup_len("", 0);
    return out;
}

static char *regex_prepare_pattern(const char *pattern, int flags, int anchored,
                                   exprtk_env_t *env) {
    if (!pattern) return NULL;
    char *case_expanded = regex_expand_flags(pattern, flags);
    if (!case_expanded) {
        regex_set_error(env, "regex: out of memory");
        return NULL;
    }
    char *expanded = regex_expand_counted_repeats(case_expanded, env);
    free(case_expanded);
    if (!expanded) return NULL;
    if (!anchored) return expanded;

    size_t pattern_len = strlen(expanded);
    char *wrapped = (char *)malloc(pattern_len + 5);
    if (!wrapped) {
        free(expanded);
        regex_set_error(env, "regex: out of memory");
        return NULL;
    }
    wrapped[0] = '^';
    wrapped[1] = '(';
    memcpy(wrapped + 2, expanded, pattern_len);
    wrapped[pattern_len + 2] = ')';
    wrapped[pattern_len + 3] = '$';
    wrapped[pattern_len + 4] = '\0';
    free(expanded);
    return wrapped;
}

static int regex_validate_prepared(const char *pattern, exprtk_env_t *env) {
    if (!re_compile(pattern)) {
        regex_set_error(env, "regex: pattern compilation failed");
        return 0;
    }
    return 1;
}

static exprtk_value_t regex_copy_string(mem_pool_t *arena, const char *data, size_t len) {
    char *out = (char *)mem_alloc(arena, len + 1);
    if (!out) return regex_zero();
    if (len > 0 && data) memcpy(out, data, len);
    out[len] = '\0';
    return exprtk_val_str(vstr_from_buf(out, len));
}

static int regex_list_push_string(exprtk_value_t *list, mem_pool_t *arena,
                                  const char *data, size_t len) {
    exprtk_value_t item = regex_copy_string(arena, data, len);
    if (item.type != EXPRTK_VAL_STRING) return 0;
    exprtk_list_push(list, item);
    return 1;
}

static exprtk_regex_ctx_t *regex_get_ctx(exprtk_env_t *env) {
    exprtk_env_t *root = regex_root_env(env);
    if (!root) return NULL;
    if (!root->regex_ctx) {
        root->regex_ctx = calloc(1, sizeof(exprtk_regex_ctx_t));
    }
    return (exprtk_regex_ctx_t *)root->regex_ctx;
}

static void regex_free_pattern(exprtk_regex_pattern_t *pattern) {
    if (!pattern) return;
    free(pattern->pattern);
    free(pattern->compiled_pattern);
    free(pattern);
}

void exprtk_regex_ctx_destroy(void *p) {
    exprtk_regex_ctx_t *ctx = (exprtk_regex_ctx_t *)p;
    if (!ctx) return;
    for (int i = 0; i < EXPRTK_REGEX_MAX_PATTERNS; ++i) {
        regex_free_pattern(ctx->patterns[i]);
        ctx->patterns[i] = NULL;
    }
    free(ctx);
}

static int regex_alloc_handle(exprtk_regex_ctx_t *ctx, exprtk_regex_pattern_t *pattern) {
    if (!ctx || !pattern) return -1;
    for (int i = 0; i < EXPRTK_REGEX_MAX_PATTERNS; ++i) {
        if (!ctx->patterns[i]) {
            ctx->patterns[i] = pattern;
            return i;
        }
    }
    return -1;
}

static int regex_arg_handle(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_INTEGER) return (int)value.data.integer;
    if (value.type == EXPRTK_VAL_NUMBER) return (int)value.data.number;
    return -1;
}

static exprtk_regex_pattern_t *regex_get_handle(exprtk_env_t *env, exprtk_value_t value) {
    exprtk_regex_ctx_t *ctx = regex_get_ctx(env);
    int handle = regex_arg_handle(value);
    if (!ctx || handle < 0 || handle >= EXPRTK_REGEX_MAX_PATTERNS || !ctx->patterns[handle]) {
        regex_set_error(env, "regex: invalid pattern handle");
        return NULL;
    }
    return ctx->patterns[handle];
}

static const char *regex_compile_arg(exprtk_value_t value, int flags, exprtk_env_t *env,
                                     mem_pool_t *arena, int *owned,
                                     exprtk_regex_pattern_t **stored) {
    *owned = 0;
    *stored = NULL;
    if (value.type == EXPRTK_VAL_STRING) {
        char *pattern = regex_arena_string(arena, value.data.string);
        if (!pattern) {
            regex_set_error(env, "regex: out of memory");
            return NULL;
        }
        *owned = 1;
        char *compiled_pattern = regex_prepare_pattern(pattern, flags, 0, env);
        if (!compiled_pattern) return NULL;
        if (!regex_validate_prepared(compiled_pattern, env)) {
            free(compiled_pattern);
            return NULL;
        }
        return compiled_pattern;
    }
    if (value.type == EXPRTK_VAL_INTEGER || value.type == EXPRTK_VAL_NUMBER) {
        *stored = regex_get_handle(env, value);
        return *stored ? (*stored)->compiled_pattern : NULL;
    }
    regex_set_error(env, "regex: expected pattern string or compiled handle");
    return NULL;
}

static int regex_accepts_span(const char *compiled_pattern, const char *data, size_t len,
                              exprtk_env_t *env) {
    if (len > (size_t)INT_MAX) {
        regex_set_error(env, "regex: input is too large");
        return 0;
    }
    char *anchored = regex_prepare_pattern(compiled_pattern, 0, 1, env);
    if (!anchored) return 0;
    re_t compiled = re_compile(anchored);
    free(anchored);
    if (!compiled) {
        regex_set_error(env, "regex: pattern compilation failed");
        return 0;
    }
    char *buf = regex_strdup_len(data, len);
    if (!buf) return 0;
    int match_len = -1;
    int idx = re_matchp(compiled, buf, &match_len);
    int ok = idx == 0 && match_len == (int)len;
    free(buf);
    return ok;
}

static int regex_find(const char *compiled_pattern, const char *data, size_t len,
                      size_t start_at, size_t *start_out, size_t *end_out,
                      exprtk_env_t *env) {
    if (start_at >= len || len - start_at > (size_t)INT_MAX) return 0;
    re_t compiled = re_compile(compiled_pattern);
    if (!compiled) {
        regex_set_error(env, "regex: pattern compilation failed");
        return 0;
    }
    char *buf = regex_strdup_len(data + start_at, len - start_at);
    if (!buf) {
        regex_set_error(env, "regex: out of memory");
        return 0;
    }
    int match_len = -1;
    int idx = re_matchp(compiled, buf, &match_len);
    free(buf);
    if (idx < 0 || match_len <= 0) return 0;
    if (start_out) *start_out = start_at + (size_t)idx;
    if (end_out) *end_out = start_at + (size_t)idx + (size_t)match_len;
    return 1;
}

static exprtk_value_t regex_match_map(mem_pool_t *arena, const char *data, size_t len,
                                      int matched, size_t start, size_t end) {
    exprtk_value_t map = exprtk_val_map();
    exprtk_map_set(&map, "matched", exprtk_val_num(matched ? 1.0 : 0.0));
    exprtk_map_set(&map, "start", exprtk_val_num(matched ? (double)start : -1.0));
    exprtk_map_set(&map, "end", exprtk_val_num(matched ? (double)end : -1.0));
    exprtk_map_set(&map, "text", matched ? regex_copy_string(arena, data + start, end - start)
                                          : regex_copy_string(arena, "", 0));
    return map;
}

static int regex_optional_flags(size_t argc, exprtk_value_t *args, size_t index, int *flags, exprtk_env_t *env) {
    *flags = 0;
    if (argc <= index) return 1;
    if (args[index].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex: flags must be a string");
        return 0;
    }
    *flags = regex_parse_flags(args[index].data.string);
    return 1;
}

static exprtk_value_t regex_object_from_handle(int handle, exprtk_regex_pattern_t *pattern,
                                               mem_pool_t *arena) {
    exprtk_value_t object = exprtk_val_object();
    const char *flags = (pattern && (pattern->flags & EXPRTK_REGEX_FLAG_IGNORE_CASE)) ? "i" : "";
    exprtk_map_set(&object, "__ts_method_provider", exprtk_val_str(vstr_from_cstr("RegExp")));
    exprtk_map_set(&object, "handle", exprtk_val_int(handle));
    exprtk_map_set(&object, "source",
                   pattern && pattern->pattern
                       ? regex_copy_string(arena, pattern->pattern, strlen(pattern->pattern))
                       : exprtk_val_str(vstr_from_cstr("")));
    exprtk_map_set(&object, "flags", exprtk_val_str(vstr_from_cstr(flags)));
    exprtk_map_set(&object, "lastIndex", exprtk_val_num(0.0));
    return object;
}

static int regex_object_handle(exprtk_value_t value, exprtk_env_t *env) {
    exprtk_value_t provider;
    exprtk_value_t handle_value;
    const char marker[] = "RegExp";

    if (!exprtk_value_is_object_like(&value) ||
        !exprtk_map_has(&value, "__ts_method_provider") ||
        !exprtk_map_has(&value, "handle")) {
        regex_set_error(env, "RegExp: expected RegExp object");
        return -1;
    }

    provider = exprtk_map_get(&value, "__ts_method_provider");
    if (provider.type != EXPRTK_VAL_STRING ||
        provider.data.string.len != sizeof(marker) - 1 ||
        memcmp(provider.data.string.data, marker, sizeof(marker) - 1) != 0) {
        regex_set_error(env, "RegExp: expected RegExp object");
        return -1;
    }

    handle_value = exprtk_map_get(&value, "handle");
    return regex_arg_handle(handle_value);
}

static exprtk_regex_pattern_t *regex_object_pattern(exprtk_value_t value, exprtk_env_t *env) {
    exprtk_regex_ctx_t *ctx = regex_get_ctx(env);
    int handle = regex_object_handle(value, env);
    if (!ctx || handle < 0 || handle >= EXPRTK_REGEX_MAX_PATTERNS || !ctx->patterns[handle]) {
        regex_set_error(env, "RegExp: invalid pattern handle");
        return NULL;
    }
    return ctx->patterns[handle];
}

static exprtk_value_t fn_regex_compile(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.compile: expected string pattern [, flags]");
        return regex_zero();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 1, &flags, env)) return regex_zero();

    exprtk_regex_ctx_t *ctx = regex_get_ctx(env);
    char *pattern = regex_arena_string(arena, args[0].data.string);
    if (!ctx || !pattern) {
        regex_set_error(env, "regex.compile: out of memory");
        return regex_zero();
    }

    char *compiled_pattern = regex_prepare_pattern(pattern, flags, 0, env);
    if (!compiled_pattern) return regex_zero();
    if (!regex_validate_prepared(compiled_pattern, env)) {
        free(compiled_pattern);
        return regex_zero();
    }

    exprtk_regex_pattern_t *compiled =
        (exprtk_regex_pattern_t *)calloc(1, sizeof(exprtk_regex_pattern_t));
    if (!compiled) {
        free(compiled_pattern);
        regex_set_error(env, "regex.compile: out of memory");
        return regex_zero();
    }
    compiled->flags = flags;
    compiled->compiled_pattern = compiled_pattern;
    compiled->pattern = regex_strdup_len(pattern, strlen(pattern));
    if (!compiled->pattern) {
        regex_free_pattern(compiled);
        regex_set_error(env, "regex.compile: out of memory");
        return regex_zero();
    }

    int handle = regex_alloc_handle(ctx, compiled);
    if (handle < 0) {
        regex_free_pattern(compiled);
        regex_set_error(env, "regex.compile: too many compiled patterns");
        return regex_zero();
    }
    return exprtk_val_int(handle);
}

static exprtk_value_t fn_regexp_ctor(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "RegExp: expected string pattern [, flags]");
        return regex_null();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 1, &flags, env)) return regex_null();

    exprtk_regex_ctx_t *ctx = regex_get_ctx(env);
    char *pattern = regex_arena_string(arena, args[0].data.string);
    if (!ctx || !pattern) {
        regex_set_error(env, "RegExp: out of memory");
        return regex_null();
    }

    char *compiled_pattern = regex_prepare_pattern(pattern, flags, 0, env);
    if (!compiled_pattern) return regex_null();
    if (!regex_validate_prepared(compiled_pattern, env)) {
        free(compiled_pattern);
        return regex_null();
    }

    exprtk_regex_pattern_t *compiled =
        (exprtk_regex_pattern_t *)calloc(1, sizeof(exprtk_regex_pattern_t));
    if (!compiled) {
        free(compiled_pattern);
        regex_set_error(env, "RegExp: out of memory");
        return regex_null();
    }
    compiled->flags = flags;
    compiled->compiled_pattern = compiled_pattern;
    compiled->pattern = regex_strdup_len(pattern, strlen(pattern));
    if (!compiled->pattern) {
        regex_free_pattern(compiled);
        regex_set_error(env, "RegExp: out of memory");
        return regex_null();
    }

    int handle = regex_alloc_handle(ctx, compiled);
    if (handle < 0) {
        regex_free_pattern(compiled);
        regex_set_error(env, "RegExp: too many compiled patterns");
        return regex_null();
    }
    return regex_object_from_handle(handle, compiled, arena);
}

static exprtk_value_t fn_regex_free(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc != 1) return regex_zero();
    exprtk_regex_ctx_t *ctx = regex_get_ctx(env);
    int handle = regex_arg_handle(args[0]);
    if (!ctx || handle < 0 || handle >= EXPRTK_REGEX_MAX_PATTERNS) return regex_zero();
    regex_free_pattern(ctx->patterns[handle]);
    ctx->patterns[handle] = NULL;
    return exprtk_val_num(1.0);
}

static exprtk_value_t fn_regex_match(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 2 && argc != 3) || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.match: expected (pattern_or_handle, string [, flags])");
        return regex_zero();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 2, &flags, env)) return regex_zero();
    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();

    vstr text = args[1].data.string;
    int ok = regex_accepts_span(compiled_pattern, text.data, text.len, env);
    if (owned) free((char *)compiled_pattern);
    return exprtk_val_num(ok ? 1.0 : 0.0);
}

static exprtk_value_t fn_regex_search(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 2 && argc != 3) || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.search: expected (pattern_or_handle, string [, flags])");
        return regex_zero();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 2, &flags, env)) return regex_zero();
    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();

    vstr text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(compiled_pattern, text.data, text.len, 0, &start, &end, env);
    if (owned) free((char *)compiled_pattern);
    return exprtk_val_num(found ? (double)start : -1.0);
}

static exprtk_value_t fn_regex_find_all(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 2 && argc != 3) || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.find_all: expected (pattern_or_handle, string [, flags])");
        return regex_zero();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 2, &flags, env)) return regex_zero();
    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();

    vstr text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    while (cursor <= text.len) {
        size_t start = 0, end = 0;
        if (!regex_find(compiled_pattern, text.data, text.len, cursor, &start, &end, env)) break;
        if (!regex_list_push_string(&list, arena, text.data + start, end - start)) {
            if (owned) free((char *)compiled_pattern);
            return regex_zero();
        }
        cursor = end > start ? end : start + 1;
    }
    if (owned) free((char *)compiled_pattern);
    return list;
}

static exprtk_value_t fn_regex_split(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc < 2 || argc > 4) || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.split: expected (pattern_or_handle, string [, limit] [, flags])");
        return regex_zero();
    }
    int limit = 0;
    if (argc >= 3 && args[2].type != EXPRTK_VAL_STRING) {
        if (args[2].type == EXPRTK_VAL_INTEGER) limit = (int)args[2].data.integer;
        else if (args[2].type == EXPRTK_VAL_NUMBER) limit = (int)args[2].data.number;
        else {
            regex_set_error(env, "regex.split: limit must be numeric");
            return regex_zero();
        }
    }
    int flags = 0;
    if (argc >= 3 && args[2].type == EXPRTK_VAL_STRING) {
        flags = regex_parse_flags(args[2].data.string);
    } else if (argc == 4) {
        if (args[3].type != EXPRTK_VAL_STRING) {
            regex_set_error(env, "regex.split: flags must be a string");
            return regex_zero();
        }
        flags = regex_parse_flags(args[3].data.string);
    }

    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();

    vstr text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    int splits = 0;
    while (cursor <= text.len && (limit <= 0 || splits < limit)) {
        size_t start = 0, end = 0;
        if (!regex_find(compiled_pattern, text.data, text.len, cursor, &start, &end, env)) break;
        if (!regex_list_push_string(&list, arena, text.data + cursor, start - cursor)) {
            if (owned) free((char *)compiled_pattern);
            return regex_zero();
        }
        cursor = end > start ? end : start + 1;
        splits++;
    }
    if (!regex_list_push_string(&list, arena, text.data + cursor, text.len - cursor)) {
        if (owned) free((char *)compiled_pattern);
        return regex_zero();
    }
    if (owned) free((char *)compiled_pattern);
    return list;
}

static exprtk_value_t fn_regex_replace(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc < 3 || argc > 5) || args[1].type != EXPRTK_VAL_STRING ||
        args[2].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.replace: expected (pattern_or_handle, string, replacement [, limit] [, flags])");
        return regex_zero();
    }
    int limit = 0;
    if (argc >= 4 && args[3].type != EXPRTK_VAL_STRING) {
        if (args[3].type == EXPRTK_VAL_INTEGER) limit = (int)args[3].data.integer;
        else if (args[3].type == EXPRTK_VAL_NUMBER) limit = (int)args[3].data.number;
        else {
            regex_set_error(env, "regex.replace: limit must be numeric");
            return regex_zero();
        }
    }
    int flags = 0;
    if (argc >= 4 && args[3].type == EXPRTK_VAL_STRING) {
        flags = regex_parse_flags(args[3].data.string);
    } else if (argc == 5) {
        if (args[4].type != EXPRTK_VAL_STRING) {
            regex_set_error(env, "regex.replace: flags must be a string");
            return regex_zero();
        }
        flags = regex_parse_flags(args[4].data.string);
    }

    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();

    vstr text = args[1].data.string;
    vstr replacement = args[2].data.string;
    size_t cap = text.len + 1;
    char *buf = (char *)mem_alloc(arena, cap);
    if (!buf) {
        if (owned) free((char *)compiled_pattern);
        regex_set_error(env, "regex.replace: out of memory");
        return regex_zero();
    }

    size_t cursor = 0;
    size_t pos = 0;
    int replacements = 0;
    while (cursor <= text.len && (limit <= 0 || replacements < limit)) {
        size_t start = 0, end = 0;
        if (!regex_find(compiled_pattern, text.data, text.len, cursor, &start, &end, env)) break;
        size_t extra = (start - cursor) + replacement.len;
        if (pos + extra + 1 > cap) {
            cap = (pos + extra + 1) * 2;
            char *grown = (char *)mem_alloc(arena, cap);
            if (!grown) {
                if (owned) free((char *)compiled_pattern);
                return regex_zero();
            }
            if (pos > 0) memcpy(grown, buf, pos);
            buf = grown;
        }
        if (start > cursor) {
            memcpy(buf + pos, text.data + cursor, start - cursor);
            pos += start - cursor;
        }
        if (replacement.len > 0) {
            memcpy(buf + pos, replacement.data, replacement.len);
            pos += replacement.len;
        }
        cursor = end > start ? end : start + 1;
        replacements++;
    }

    if (cursor < text.len) {
        size_t rest = text.len - cursor;
        if (pos + rest + 1 > cap) {
            cap = pos + rest + 1;
            char *grown = (char *)mem_alloc(arena, cap);
            if (!grown) {
                if (owned) free((char *)compiled_pattern);
                return regex_zero();
            }
            if (pos > 0) memcpy(grown, buf, pos);
            buf = grown;
        }
        memcpy(buf + pos, text.data + cursor, rest);
        pos += rest;
    }
    buf[pos] = '\0';
    if (owned) free((char *)compiled_pattern);
    return exprtk_val_str(vstr_from_buf(buf, pos));
}

static exprtk_value_t fn_regex_match_info(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 2 && argc != 3) || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.match_info: expected (pattern_or_handle, string [, flags])");
        return regex_zero();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 2, &flags, env)) return regex_zero();
    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();
    vstr text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(compiled_pattern, text.data, text.len, 0, &start, &end, env);
    if (owned) free((char *)compiled_pattern);
    return regex_match_map(arena, text.data, text.len, found, start, end);
}

static exprtk_value_t fn_regex_find_iter(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    if ((argc != 2 && argc != 3) || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "regex.find_iter: expected (pattern_or_handle, string [, flags])");
        return regex_zero();
    }
    int flags = 0;
    if (!regex_optional_flags(argc, args, 2, &flags, env)) return regex_zero();
    int owned = 0;
    exprtk_regex_pattern_t *stored = NULL;
    const char *compiled_pattern = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!compiled_pattern) return regex_zero();
    vstr text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    while (cursor <= text.len) {
        size_t start = 0, end = 0;
        if (!regex_find(compiled_pattern, text.data, text.len, cursor, &start, &end, env)) break;
        exprtk_list_push(&list, regex_match_map(arena, text.data, text.len, 1, start, end));
        cursor = end > start ? end : start + 1;
    }
    if (owned) free((char *)compiled_pattern);
    return list;
}

static exprtk_value_t fn_regexp_test(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc != 2 || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "RegExp.test: expected (this, string)");
        return regex_zero();
    }
    exprtk_regex_pattern_t *pattern = regex_object_pattern(args[0], env);
    if (!pattern) return regex_zero();

    vstr text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(pattern->compiled_pattern, text.data, text.len, 0, &start, &end, env);
    return exprtk_val_num(found ? 1.0 : 0.0);
}

static exprtk_value_t fn_regexp_match(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc != 2 || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "RegExp.match: expected (this, string)");
        return regex_zero();
    }
    exprtk_regex_pattern_t *pattern = regex_object_pattern(args[0], env);
    if (!pattern) return regex_zero();
    vstr text = args[1].data.string;
    return exprtk_val_num(regex_accepts_span(pattern->compiled_pattern, text.data, text.len, env) ? 1.0 : 0.0);
}

static exprtk_value_t fn_regexp_search(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc != 2 || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "RegExp.search: expected (this, string)");
        return regex_zero();
    }
    exprtk_regex_pattern_t *pattern = regex_object_pattern(args[0], env);
    if (!pattern) return regex_zero();
    vstr text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(pattern->compiled_pattern, text.data, text.len, 0, &start, &end, env);
    return exprtk_val_num(found ? (double)start : -1.0);
}

static exprtk_value_t fn_regexp_exec(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    if (argc != 2 || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "RegExp.exec: expected (this, string)");
        return regex_null();
    }
    exprtk_regex_pattern_t *pattern = regex_object_pattern(args[0], env);
    if (!pattern) return regex_null();
    vstr text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(pattern->compiled_pattern, text.data, text.len, 0, &start, &end, env);
    if (!found) return regex_null();
    return regex_match_map(arena, text.data, text.len, 1, start, end);
}

static exprtk_value_t fn_regexp_find_all(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    if (argc != 2 || args[1].type != EXPRTK_VAL_STRING) {
        regex_set_error(env, "RegExp.find_all: expected (this, string)");
        return regex_zero();
    }
    exprtk_regex_pattern_t *pattern = regex_object_pattern(args[0], env);
    if (!pattern) return regex_zero();
    vstr text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    while (cursor <= text.len) {
        size_t start = 0, end = 0;
        if (!regex_find(pattern->compiled_pattern, text.data, text.len, cursor, &start, &end, env)) break;
        if (!regex_list_push_string(&list, arena, text.data + start, end - start)) {
            return regex_zero();
        }
        cursor = end > start ? end : start + 1;
    }
    return list;
}

static exprtk_value_t fn_regexp_free(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc != 1) return regex_zero();
    exprtk_regex_ctx_t *ctx = regex_get_ctx(env);
    int handle = regex_object_handle(args[0], env);
    if (!ctx || handle < 0 || handle >= EXPRTK_REGEX_MAX_PATTERNS) return regex_zero();
    regex_free_pattern(ctx->patterns[handle]);
    ctx->patterns[handle] = NULL;
    return exprtk_val_num(1.0);
}

static exprtk_value_t fn_regexp_to_string(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || !exprtk_value_is_object_like(&args[0]) ||
        !exprtk_map_has(&args[0], "source")) {
        return regex_copy_string(arena, "", 0);
    }
    exprtk_value_t source = exprtk_map_get(&args[0], "source");
    if (source.type != EXPRTK_VAL_STRING) return regex_copy_string(arena, "", 0);
    return regex_copy_string(arena, source.data.string.data, source.data.string.len);
}

static const exprtk_func_entry_t regex_entries[] = {
    { "RegExp",         fn_regexp_ctor },
    { "RegExp.exec",    fn_regexp_exec },
    { "RegExp.find_all", fn_regexp_find_all },
    { "RegExp.free",    fn_regexp_free },
    { "RegExp.match",   fn_regexp_match },
    { "RegExp.search",  fn_regexp_search },
    { "RegExp.test",    fn_regexp_test },
    { "RegExp.toString", fn_regexp_to_string },
    { "RegExp.to_string", fn_regexp_to_string },
    { "regex.compile",  fn_regex_compile },
    { "regex.free",     fn_regex_free },
    { "regex.find_iter", fn_regex_find_iter },
    { "regex.find_all", fn_regex_find_all },
    { "regex.match",    fn_regex_match },
    { "regex.match_info", fn_regex_match_info },
    { "regex.replace",  fn_regex_replace },
    { "regex.search",   fn_regex_search },
    { "regex.split",    fn_regex_split },
    { "regex.test",     fn_regex_match },
    { "regex_compile",  fn_regex_compile },
    { "regex_find_all", fn_regex_find_all },
    { "regex_find_iter", fn_regex_find_iter },
    { "regex_match",    fn_regex_match },
    { "regex_match_info", fn_regex_match_info },
    { "regex_replace",  fn_regex_replace },
    { "regex_search",   fn_regex_search },
    { "regex_split",    fn_regex_split },
};

static const exprtk_module_t regex_module = {
    "regex", regex_entries, sizeof(regex_entries) / sizeof(regex_entries[0])
};

const exprtk_module_t *exprtk_module_regex(void) { return &regex_module; }
