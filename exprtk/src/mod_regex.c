/**
 * @file mod_regex.c
 * @brief Core regex builtins backed by libfsm.
 */
#include "exprtk_module.h"

#include <fsm/fsm.h>
#include <re/re.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define EXPRTK_REGEX_MAX_PATTERNS 32

typedef struct {
    struct fsm *fsm;
    char *pattern;
    int flags;
} exprtk_regex_pattern_t;

typedef struct {
    exprtk_regex_pattern_t *patterns[EXPRTK_REGEX_MAX_PATTERNS];
} exprtk_regex_ctx_t;

static exprtk_value_t regex_zero(void) {
    return exprtk_val_num(0.0);
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

static char *regex_arena_string(mem_pool_t *arena, tstr_v s) {
    char *out = (char *)mem_alloc(arena, s.len + 1);
    if (!out) return NULL;
    if (s.len > 0 && s.data) memcpy(out, s.data, s.len);
    out[s.len] = '\0';
    return out;
}

static int regex_parse_flags(tstr_v flags) {
    int out = 0;
    for (size_t i = 0; i < flags.len; ++i) {
        if (flags.data[i] == 'i' || flags.data[i] == 'I') out |= 1;
    }
    return out;
}

static char *regex_expand_flags(const char *pattern, int flags) {
    if (!(flags & 1)) return regex_strdup_len(pattern, strlen(pattern));
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

static exprtk_value_t regex_copy_string(mem_pool_t *arena, const char *data, size_t len) {
    char *out = (char *)mem_alloc(arena, len + 1);
    if (!out) return regex_zero();
    if (len > 0 && data) memcpy(out, data, len);
    out[len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(out, len));
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
    if (pattern->fsm) fsm_free(pattern->fsm);
    free(pattern->pattern);
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

static struct fsm *regex_compile_fsm(const char *pattern, int flags, exprtk_env_t *env) {
    if (!pattern) return NULL;
    char *expanded = regex_expand_flags(pattern, flags);
    if (!expanded) {
        regex_set_error(env, "regex: out of memory");
        return NULL;
    }
    size_t pattern_len = strlen(expanded);
    char *anchored = (char *)malloc(pattern_len + 5);
    if (!anchored) {
        free(expanded);
        regex_set_error(env, "regex: out of memory");
        return NULL;
    }
    anchored[0] = '^';
    anchored[1] = '(';
    memcpy(anchored + 2, expanded, pattern_len);
    anchored[pattern_len + 2] = ')';
    anchored[pattern_len + 3] = '$';
    anchored[pattern_len + 4] = '\0';
    free(expanded);

    char *cursor = anchored;
    struct re_err err;
    enum re_flags re_flags_value = 0;
    struct fsm *fsm = re_comp(RE_PCRE, fsm_sgetc, &cursor, NULL, re_flags_value, &err);
    if (!fsm) {
        free(anchored);
        regex_set_error(env, "regex: pattern compilation failed");
        return NULL;
    }
    if (!fsm_determinise(fsm) || !fsm_minimise(fsm)) {
        fsm_free(fsm);
        free(anchored);
        regex_set_error(env, "regex: pattern optimization failed");
        return NULL;
    }
    free(anchored);
    return fsm;
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

static struct fsm *regex_compile_arg(exprtk_value_t value, int flags, exprtk_env_t *env,
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
        return regex_compile_fsm(pattern, flags, env);
    }
    if (value.type == EXPRTK_VAL_INTEGER || value.type == EXPRTK_VAL_NUMBER) {
        *stored = regex_get_handle(env, value);
        return *stored ? (*stored)->fsm : NULL;
    }
    regex_set_error(env, "regex: expected pattern string or compiled handle");
    return NULL;
}

static int regex_accepts_span(struct fsm *fsm, const char *data, size_t len) {
    char *buf = regex_strdup_len(data, len);
    if (!buf) return 0;
    const char *cursor = buf;
    fsm_state_t end_state;
    int result = fsm_exec(fsm, fsm_sgetc, &cursor, &end_state, NULL);
    int ok = result == 1 && cursor == buf + len;
    free(buf);
    return ok;
}

static int regex_find(struct fsm *fsm, const char *data, size_t len,
                      size_t start_at, size_t *start_out, size_t *end_out) {
    if (start_at > len) return 0;
    for (size_t i = start_at; i < len; ++i) {
        size_t best_end = 0;
        for (size_t j = i + 1; j <= len; ++j) {
            if (regex_accepts_span(fsm, data + i, j - i)) {
                best_end = j;
            }
        }
        if (best_end > i) {
            if (start_out) *start_out = i;
            if (end_out) *end_out = best_end;
            return 1;
        }
    }
    return 0;
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

    struct fsm *fsm = regex_compile_fsm(pattern, flags, env);
    if (!fsm) return regex_zero();

    exprtk_regex_pattern_t *compiled =
        (exprtk_regex_pattern_t *)calloc(1, sizeof(exprtk_regex_pattern_t));
    if (!compiled) {
        fsm_free(fsm);
        regex_set_error(env, "regex.compile: out of memory");
        return regex_zero();
    }
    compiled->fsm = fsm;
    compiled->flags = flags;
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();

    tstr_v text = args[1].data.string;
    int ok = regex_accepts_span(fsm, text.data, text.len);
    if (owned) fsm_free(fsm);
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();

    tstr_v text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(fsm, text.data, text.len, 0, &start, &end);
    if (owned) fsm_free(fsm);
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();

    tstr_v text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    while (cursor <= text.len) {
        size_t start = 0, end = 0;
        if (!regex_find(fsm, text.data, text.len, cursor, &start, &end)) break;
        if (!regex_list_push_string(&list, arena, text.data + start, end - start)) {
            if (owned) fsm_free(fsm);
            return regex_zero();
        }
        cursor = end > start ? end : start + 1;
    }
    if (owned) fsm_free(fsm);
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();

    tstr_v text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    int splits = 0;
    while (cursor <= text.len && (limit <= 0 || splits < limit)) {
        size_t start = 0, end = 0;
        if (!regex_find(fsm, text.data, text.len, cursor, &start, &end)) break;
        if (!regex_list_push_string(&list, arena, text.data + cursor, start - cursor)) {
            if (owned) fsm_free(fsm);
            return regex_zero();
        }
        cursor = end > start ? end : start + 1;
        splits++;
    }
    if (!regex_list_push_string(&list, arena, text.data + cursor, text.len - cursor)) {
        if (owned) fsm_free(fsm);
        return regex_zero();
    }
    if (owned) fsm_free(fsm);
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();

    tstr_v text = args[1].data.string;
    tstr_v replacement = args[2].data.string;
    size_t cap = text.len + 1;
    char *buf = (char *)mem_alloc(arena, cap);
    if (!buf) {
        if (owned) fsm_free(fsm);
        regex_set_error(env, "regex.replace: out of memory");
        return regex_zero();
    }

    size_t cursor = 0;
    size_t pos = 0;
    int replacements = 0;
    while (cursor <= text.len && (limit <= 0 || replacements < limit)) {
        size_t start = 0, end = 0;
        if (!regex_find(fsm, text.data, text.len, cursor, &start, &end)) break;
        size_t extra = (start - cursor) + replacement.len;
        if (pos + extra + 1 > cap) {
            cap = (pos + extra + 1) * 2;
            char *grown = (char *)mem_alloc(arena, cap);
            if (!grown) {
                if (owned) fsm_free(fsm);
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
                if (owned) fsm_free(fsm);
                return regex_zero();
            }
            if (pos > 0) memcpy(grown, buf, pos);
            buf = grown;
        }
        memcpy(buf + pos, text.data + cursor, rest);
        pos += rest;
    }
    buf[pos] = '\0';
    if (owned) fsm_free(fsm);
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();
    tstr_v text = args[1].data.string;
    size_t start = 0, end = 0;
    int found = regex_find(fsm, text.data, text.len, 0, &start, &end);
    if (owned) fsm_free(fsm);
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
    struct fsm *fsm = regex_compile_arg(args[0], flags, env, arena, &owned, &stored);
    (void)stored;
    if (!fsm) return regex_zero();
    tstr_v text = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t cursor = 0;
    while (cursor <= text.len) {
        size_t start = 0, end = 0;
        if (!regex_find(fsm, text.data, text.len, cursor, &start, &end)) break;
        exprtk_list_push(&list, regex_match_map(arena, text.data, text.len, 1, start, end));
        cursor = end > start ? end : start + 1;
    }
    if (owned) fsm_free(fsm);
    return list;
}

static const exprtk_func_entry_t regex_entries[] = {
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
