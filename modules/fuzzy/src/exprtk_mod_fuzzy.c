#include "fuzzy.h"
#include "ac_automaton.h"
#include "levenshtein_automaton.h"
#include "tre/tre.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    wchar_t *data;
    size_t len;
    size_t *byte_offsets;
} fuzzy_wtext_t;

typedef struct {
    size_t *byte_offsets;
    size_t len;
} fuzzy_utf8_offsets_t;

typedef struct {
    exprtk_value_t list;
    mem_pool_t *arena;
    vstr text;
    const fuzzy_utf8_offsets_t *offsets;
    const exprtk_value_t *patterns;
    size_t pattern_count;
    int utf8_positions;
} fuzzy_collect_ctx_t;

static int fuzzy_is_number(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_NUMBER || value.type == EXPRTK_VAL_INTEGER;
}

static double fuzzy_number_value(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_INTEGER ? (double)value.data.integer : value.data.number;
}

static int fuzzy_int_arg(const exprtk_value_t *arg, int *out) {
    double value;
    if (!arg || !out || !fuzzy_is_number(*arg)) return 0;
    value = fuzzy_number_value(*arg);
    if (!isfinite(value) || value < 0.0 || value > (double)INT_MAX) return 0;
    *out = (int)value;
    return 1;
}

static int fuzzy_size_arg(const exprtk_value_t *arg, size_t *out) {
    double value;
    if (!arg || !out || !fuzzy_is_number(*arg)) return 0;
    value = fuzzy_number_value(*arg);
    if (!isfinite(value) || value < 0.0 || value > (double)SIZE_MAX) return 0;
    *out = (size_t)value;
    return 1;
}

static exprtk_value_t fuzzy_copy_string(mem_pool_t *arena, const char *data, size_t len) {
    char *buf;
    if (!arena) return exprtk_val_str(vstr_from_buf("", 0));
    buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_str(vstr_from_buf("", 0));
    if (len > 0 && data) memcpy(buf, data, len);
    buf[len] = '\0';
    return exprtk_val_str(vstr_from_buf(buf, len));
}

static int fuzzy_parse_flags(vstr flags, int literal_default, int *out) {
    int cflags = REG_EXTENDED;
    if (!out) return 0;
    if (literal_default) cflags |= REG_LITERAL;
    for (size_t i = 0; i < flags.len; ++i) {
        switch (flags.data[i]) {
        case 'i':
            cflags |= REG_ICASE;
            break;
        case 'n':
            cflags |= REG_NEWLINE;
            break;
        case 'l':
            cflags |= REG_LITERAL;
            break;
        case 'r':
            cflags &= ~REG_LITERAL;
            break;
        default:
            return 0;
        }
    }
    *out = cflags;
    return 1;
}

static int fuzzy_flags_arg(size_t argc, exprtk_value_t *args, size_t index,
                           int literal_default, int *out) {
    if (argc <= index) return fuzzy_parse_flags(vstr_from_buf("", 0), literal_default, out);
    if (args[index].type != EXPRTK_VAL_STRING) return 0;
    return fuzzy_parse_flags(args[index].data.string, literal_default, out);
}

static int fuzzy_utf8_to_wide(vstr input, mem_pool_t *arena, fuzzy_wtext_t *out) {
    vstr rest = input;
    size_t byte_offset = 0;
    size_t wide_len = 0;

    if (!arena || !out || (!input.data && input.len > 0)) return 0;
    memset(out, 0, sizeof(*out));

    out->data = (wchar_t *)mem_alloc(arena, sizeof(wchar_t) * (input.len + 1));
    out->byte_offsets = (size_t *)mem_alloc(arena, sizeof(size_t) * (input.len + 1));
    if (!out->data || !out->byte_offsets) return 0;

    while (rest.len > 0) {
        uint32_t codepoint = 0;
        size_t before = rest.len;
        size_t consumed;

        if (!vstr_utf8_next(&rest, &codepoint)) return 0;
        consumed = before - rest.len;
        if (consumed == 0 || codepoint > 0x10FFFFu) return 0;

#if WCHAR_MAX <= 0xFFFF
        if (codepoint > 0xFFFFu) {
            uint32_t scalar = codepoint - 0x10000u;
            out->byte_offsets[wide_len] = byte_offset;
            out->data[wide_len++] = (wchar_t)(0xD800u + (scalar >> 10));
            out->byte_offsets[wide_len] = byte_offset;
            out->data[wide_len++] = (wchar_t)(0xDC00u + (scalar & 0x3FFu));
        } else {
            out->byte_offsets[wide_len] = byte_offset;
            out->data[wide_len++] = (wchar_t)codepoint;
        }
#else
        out->byte_offsets[wide_len] = byte_offset;
        out->data[wide_len++] = (wchar_t)codepoint;
#endif
        byte_offset += consumed;
    }

    out->byte_offsets[wide_len] = input.len;
    out->data[wide_len] = L'\0';
    out->len = wide_len;
    return 1;
}

static size_t fuzzy_byte_offset(const fuzzy_wtext_t *text, regoff_t offset) {
    if (!text || offset < 0) return (size_t)-1;
    if ((size_t)offset > text->len) return text->byte_offsets[text->len];
    return text->byte_offsets[(size_t)offset];
}

static int64_t fuzzy_utf8_index(vstr text, size_t byte_offset) {
    size_t len;
    if (byte_offset > text.len) byte_offset = text.len;
    len = vstr_utf8_len(vstr_from_buf(text.data, byte_offset));
    return len == VSTR_NPOS ? -1 : (int64_t)len;
}

static int fuzzy_utf8_offsets(vstr input, mem_pool_t *arena, fuzzy_utf8_offsets_t *out) {
    vstr rest = input;
    size_t byte_offset = 0;
    size_t cp_len = 0;

    if (!arena || !out || (!input.data && input.len > 0)) return 0;
    memset(out, 0, sizeof(*out));
    out->byte_offsets = (size_t *)mem_alloc(arena, sizeof(size_t) * (input.len + 1));
    if (!out->byte_offsets) return 0;

    while (rest.len > 0) {
        uint32_t codepoint = 0;
        size_t before = rest.len;
        size_t consumed;
        (void)codepoint;

        if (!vstr_utf8_next(&rest, &codepoint)) return 0;
        consumed = before - rest.len;
        if (consumed == 0) return 0;
        out->byte_offsets[cp_len++] = byte_offset;
        byte_offset += consumed;
    }

    out->byte_offsets[cp_len] = input.len;
    out->len = cp_len;
    return 1;
}

static size_t fuzzy_offset_to_byte(const fuzzy_collect_ctx_t *ctx, size_t offset) {
    if (!ctx || !ctx->utf8_positions) return offset <= ctx->text.len ? offset : ctx->text.len;
    if (!ctx->offsets || !ctx->offsets->byte_offsets) return ctx->text.len;
    if (offset > ctx->offsets->len) offset = ctx->offsets->len;
    return ctx->offsets->byte_offsets[offset];
}

static exprtk_value_t fuzzy_result(mem_pool_t *arena, vstr text, int matched,
                                   size_t start, size_t end, const regamatch_t *amatch) {
    exprtk_value_t map = exprtk_val_map();
    exprtk_map_set(&map, "matched", exprtk_val_num(matched ? 1.0 : 0.0));
    exprtk_map_set(&map, "start", exprtk_val_int(matched ? (int64_t)start : -1));
    exprtk_map_set(&map, "end", exprtk_val_int(matched ? (int64_t)end : -1));
    exprtk_map_set(&map, "utf8_start",
                   exprtk_val_int(matched ? fuzzy_utf8_index(text, start) : -1));
    exprtk_map_set(&map, "utf8_end",
                   exprtk_val_int(matched ? fuzzy_utf8_index(text, end) : -1));
    exprtk_map_set(&map, "text",
                   matched ? fuzzy_copy_string(arena, text.data + start, end - start)
                           : exprtk_val_str(vstr_from_buf("", 0)));
    if (amatch) {
        exprtk_map_set(&map, "cost", exprtk_val_int(matched ? amatch->cost : -1));
        exprtk_map_set(&map, "insertions", exprtk_val_int(matched ? amatch->num_ins : -1));
        exprtk_map_set(&map, "deletions", exprtk_val_int(matched ? amatch->num_del : -1));
        exprtk_map_set(&map, "substitutions", exprtk_val_int(matched ? amatch->num_subst : -1));
    }
    return map;
}

static exprtk_value_t fuzzy_no_match(mem_pool_t *arena, vstr text, int approximate) {
    regamatch_t amatch;
    memset(&amatch, 0, sizeof(amatch));
    return fuzzy_result(arena, text, 0, 0, 0, approximate ? &amatch : NULL);
}

static exprtk_value_t fuzzy_match_map(mem_pool_t *arena, vstr text, size_t start, size_t end,
                                      size_t utf8_start, size_t utf8_end, size_t distance) {
    exprtk_value_t map = exprtk_val_map();
    if (end > text.len) end = text.len;
    if (start > end) start = end;
    exprtk_map_set(&map, "matched", exprtk_val_num(1.0));
    exprtk_map_set(&map, "start", exprtk_val_int((int64_t)start));
    exprtk_map_set(&map, "end", exprtk_val_int((int64_t)end));
    exprtk_map_set(&map, "utf8_start", exprtk_val_int((int64_t)utf8_start));
    exprtk_map_set(&map, "utf8_end", exprtk_val_int((int64_t)utf8_end));
    exprtk_map_set(&map, "text", fuzzy_copy_string(arena, text.data + start, end - start));
    exprtk_map_set(&map, "distance", exprtk_val_int((int64_t)distance));
    return map;
}

static int fuzzy_mode_arg(size_t argc, exprtk_value_t *args, size_t index, int *utf8_mode) {
    vstr mode;
    if (!utf8_mode) return 0;
    *utf8_mode = 1;
    if (argc <= index) return 1;
    if (args[index].type != EXPRTK_VAL_STRING) return 0;
    mode = args[index].data.string;
    if (mode.len == 4 && memcmp(mode.data, "utf8", 4) == 0) {
        *utf8_mode = 1;
        return 1;
    }
    if ((mode.len == 4 && memcmp(mode.data, "byte", 4) == 0) ||
        (mode.len == 5 && memcmp(mode.data, "bytes", 5) == 0) ||
        (mode.len == 3 && memcmp(mode.data, "raw", 3) == 0)) {
        *utf8_mode = 0;
        return 1;
    }
    return 0;
}

static exprtk_value_t fuzzy_exact(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    fuzzy_wtext_t pattern;
    fuzzy_wtext_t text;
    regex_t regex;
    regmatch_t match[1];
    int cflags;
    int rc;
    (void)env;

    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING ||
        !fuzzy_flags_arg(argc, args, 2, 0, &cflags)) {
        return fuzzy_no_match(arena, vstr_from_buf("", 0), 0);
    }

    if (!fuzzy_utf8_to_wide(args[0].data.string, arena, &pattern) ||
        !fuzzy_utf8_to_wide(args[1].data.string, arena, &text)) {
        return fuzzy_no_match(arena, args[1].data.string, 0);
    }

    memset(&regex, 0, sizeof(regex));
    rc = tre_regwncomp(&regex, pattern.data, pattern.len, cflags);
    if (rc != REG_OK) return fuzzy_no_match(arena, args[1].data.string, 0);

    match[0].rm_so = -1;
    match[0].rm_eo = -1;
    rc = tre_regwnexec(&regex, text.data, text.len, 1, match, 0);
    tre_regfree(&regex);

    if (rc == REG_OK && match[0].rm_so >= 0 && match[0].rm_eo >= match[0].rm_so) {
        size_t start = fuzzy_byte_offset(&text, match[0].rm_so);
        size_t end = fuzzy_byte_offset(&text, match[0].rm_eo);
        return fuzzy_result(arena, args[1].data.string, 1, start, end, NULL);
    }
    return fuzzy_no_match(arena, args[1].data.string, 0);
}

static exprtk_value_t fuzzy_match_impl(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena,
                                       int literal_default) {
    fuzzy_wtext_t pattern;
    fuzzy_wtext_t text;
    regex_t regex;
    regmatch_t pmatch[1];
    regamatch_t amatch;
    regaparams_t params;
    int max_cost = 1;
    int cflags;
    size_t flags_index = 2;
    int rc;
    (void)env;

    if (argc < 2 || argc > 4 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING) {
        return fuzzy_no_match(arena, vstr_from_buf("", 0), 1);
    }

    if (argc >= 3 && fuzzy_is_number(args[2])) {
        if (!fuzzy_int_arg(&args[2], &max_cost)) {
            return fuzzy_no_match(arena, args[1].data.string, 1);
        }
        flags_index = 3;
    }

    if (!fuzzy_flags_arg(argc, args, flags_index, literal_default, &cflags)) {
        return fuzzy_no_match(arena, args[1].data.string, 1);
    }

    if (!fuzzy_utf8_to_wide(args[0].data.string, arena, &pattern) ||
        !fuzzy_utf8_to_wide(args[1].data.string, arena, &text)) {
        return fuzzy_no_match(arena, args[1].data.string, 1);
    }

    memset(&regex, 0, sizeof(regex));
    rc = tre_regwncomp(&regex, pattern.data, pattern.len, cflags);
    if (rc != REG_OK) return fuzzy_no_match(arena, args[1].data.string, 1);

    tre_regaparams_default(&params);
    params.max_cost = max_cost;
    params.max_err = max_cost;

    memset(&pmatch, 0, sizeof(pmatch));
    memset(&amatch, 0, sizeof(amatch));
    amatch.nmatch = 1;
    amatch.pmatch = pmatch;

    rc = tre_regawnexec(&regex, text.data, text.len, &amatch, params, REG_APPROX_MATCHER);
    tre_regfree(&regex);

    if (rc == REG_OK && pmatch[0].rm_so >= 0 && pmatch[0].rm_eo >= pmatch[0].rm_so) {
        size_t start = fuzzy_byte_offset(&text, pmatch[0].rm_so);
        size_t end = fuzzy_byte_offset(&text, pmatch[0].rm_eo);
        return fuzzy_result(arena, args[1].data.string, 1, start, end, &amatch);
    }
    return fuzzy_no_match(arena, args[1].data.string, 1);
}

static bool fuzzy_lev_collect(size_t start, size_t end, size_t distance, void *user_data) {
    fuzzy_collect_ctx_t *ctx = (fuzzy_collect_ctx_t *)user_data;
    size_t byte_start = fuzzy_offset_to_byte(ctx, start);
    size_t byte_end = fuzzy_offset_to_byte(ctx, end);
    exprtk_value_t item = fuzzy_match_map(ctx->arena, ctx->text, byte_start, byte_end,
                                          ctx->utf8_positions ? start : fuzzy_utf8_index(ctx->text, byte_start),
                                          ctx->utf8_positions ? end : fuzzy_utf8_index(ctx->text, byte_end),
                                          distance);
    exprtk_list_push(&ctx->list, item);
    return true;
}

static exprtk_value_t fuzzy_levenshtein_impl(size_t argc, exprtk_value_t *args,
                                             exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t list = exprtk_val_list_empty();
    fuzzy_collect_ctx_t collect;
    fuzzy_utf8_offsets_t offsets;
    size_t max_distance;
    int utf8_mode;
    int rc;
    (void)env;

    if ((argc != 3 && argc != 4) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || !fuzzy_size_arg(&args[2], &max_distance) ||
        !fuzzy_mode_arg(argc, args, 3, &utf8_mode)) {
        return list;
    }

    memset(&collect, 0, sizeof(collect));
    memset(&offsets, 0, sizeof(offsets));
    collect.list = list;
    collect.arena = arena;
    collect.text = args[1].data.string;
    collect.utf8_positions = utf8_mode;

    if (utf8_mode) {
        lev_utf8_automaton_t *lev;
        if (!fuzzy_utf8_offsets(args[1].data.string, arena, &offsets)) return list;
        collect.offsets = &offsets;
        lev = lev_utf8_automaton_create();
        if (!lev) return list;
        rc = lev_utf8_automaton_init(lev, args[0].data.string, max_distance);
        if (rc == 0) (void)lev_utf8_automaton_match(lev, args[1].data.string,
                                                     fuzzy_lev_collect, &collect);
        lev_utf8_automaton_free(lev);
    } else {
        lev_automaton_t *lev = lev_automaton_create();
        if (!lev) return list;
        rc = lev_automaton_init(lev, args[0].data.string, max_distance);
        if (rc == 0) (void)lev_automaton_match(lev, args[1].data.string,
                                               fuzzy_lev_collect, &collect);
        lev_automaton_free(lev);
    }
    return collect.list;
}

static int fuzzy_patterns_from_list(const exprtk_value_t *value, const exprtk_value_t **patterns,
                                    size_t *count) {
    if (!value || !patterns || !count) return 0;
    if (value->type == EXPRTK_VAL_STRING) {
        *patterns = value;
        *count = 1;
        return 1;
    }
    if (value->type != EXPRTK_VAL_LIST) return 0;
    for (size_t i = 0; i < value->data.list.count; ++i) {
        if (value->data.list.items[i].type != EXPRTK_VAL_STRING) return 0;
    }
    *patterns = value->data.list.items;
    *count = value->data.list.count;
    return 1;
}

static bool fuzzy_ac_collect(uint32_t pattern_id, size_t start, size_t end, void *user_data) {
    fuzzy_collect_ctx_t *ctx = (fuzzy_collect_ctx_t *)user_data;
    size_t byte_start = fuzzy_offset_to_byte(ctx, start);
    size_t byte_end = fuzzy_offset_to_byte(ctx, end);
    exprtk_value_t item = fuzzy_match_map(ctx->arena, ctx->text, byte_start, byte_end,
                                          ctx->utf8_positions ? start : fuzzy_utf8_index(ctx->text, byte_start),
                                          ctx->utf8_positions ? end : fuzzy_utf8_index(ctx->text, byte_end),
                                          0);
    exprtk_map_set(&item, "pattern_id", exprtk_val_int((int64_t)pattern_id));
    if (pattern_id < ctx->pattern_count) {
        vstr pattern = ctx->patterns[pattern_id].data.string;
        exprtk_map_set(&item, "pattern", fuzzy_copy_string(ctx->arena, pattern.data, pattern.len));
    }
    exprtk_list_push(&ctx->list, item);
    return true;
}

static exprtk_value_t fuzzy_ac_impl(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    exprtk_value_t list = exprtk_val_list_empty();
    fuzzy_collect_ctx_t collect;
    fuzzy_utf8_offsets_t offsets;
    const exprtk_value_t *patterns = NULL;
    size_t pattern_count = 0;
    int utf8_mode;
    int rc;
    (void)env;

    if ((argc != 2 && argc != 3) || args[1].type != EXPRTK_VAL_STRING ||
        !fuzzy_patterns_from_list(&args[0], &patterns, &pattern_count) ||
        !fuzzy_mode_arg(argc, args, 2, &utf8_mode)) {
        return list;
    }

    memset(&collect, 0, sizeof(collect));
    memset(&offsets, 0, sizeof(offsets));
    collect.list = list;
    collect.arena = arena;
    collect.text = args[1].data.string;
    collect.patterns = patterns;
    collect.pattern_count = pattern_count;
    collect.utf8_positions = utf8_mode;

    if (utf8_mode) {
        ac_utf8_automaton_t *ac;
        if (!fuzzy_utf8_offsets(args[1].data.string, arena, &offsets)) return list;
        collect.offsets = &offsets;
        ac = ac_utf8_automaton_create();
        if (!ac) return list;
        for (size_t i = 0; i < pattern_count; ++i) {
            uint32_t id;
            rc = ac_utf8_automaton_add_pattern(ac, patterns[i].data.string, &id);
            if (rc != 0) {
                ac_utf8_automaton_free(ac);
                return list;
            }
        }
        if (ac_utf8_automaton_build(ac) == 0)
            (void)ac_utf8_automaton_match(ac, args[1].data.string, fuzzy_ac_collect, &collect);
        ac_utf8_automaton_free(ac);
    } else {
        ac_automaton_t *ac = ac_automaton_create();
        if (!ac) return list;
        for (size_t i = 0; i < pattern_count; ++i) {
            uint32_t id;
            rc = ac_automaton_add_pattern(ac, patterns[i].data.string, &id);
            if (rc != 0) {
                ac_automaton_free(ac);
                return list;
            }
        }
        if (ac_automaton_build(ac) == 0)
            (void)ac_automaton_match(ac, args[1].data.string, fuzzy_ac_collect, &collect);
        ac_automaton_free(ac);
    }
    return collect.list;
}

static exprtk_value_t fn_regex_search(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    return fuzzy_exact(argc, args, env, arena);
}

static exprtk_value_t fn_fuzzy_search(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    return fuzzy_match_impl(argc, args, env, arena, 1);
}

static exprtk_value_t fn_fuzzy_regex_search(size_t argc, exprtk_value_t *args,
                                            exprtk_env_t *env, mem_pool_t *arena) {
    return fuzzy_match_impl(argc, args, env, arena, 0);
}

static exprtk_value_t fn_levenshtein(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    return fuzzy_levenshtein_impl(argc, args, env, arena);
}

static exprtk_value_t fn_ac(size_t argc, exprtk_value_t *args,
                            exprtk_env_t *env, mem_pool_t *arena) {
    return fuzzy_ac_impl(argc, args, env, arena);
}

static exprtk_value_t fn_tre_version(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    const char *version;
    (void)args;
    (void)env;
    if (argc != 0) return exprtk_val_str(vstr_from_buf("", 0));
    version = tre_version();
    return fuzzy_copy_string(arena, version, strlen(version));
}

static const exprtk_func_entry_t fuzzy_entries[] = {
    {"search", fn_fuzzy_search},
    {"regex", fn_regex_search},
    {"regex_search", fn_regex_search},
    {"fuzzy_search", fn_fuzzy_search},
    {"fuzzy_regex_search", fn_fuzzy_regex_search},
    {"levenshtein", fn_levenshtein},
    {"lev", fn_levenshtein},
    {"agrep", fn_levenshtein},
    {"ac", fn_ac},
    {"multi_search", fn_ac},
    {"tre_version", fn_tre_version},
};

static const exprtk_module_t fuzzy_module = {
    .module_name = "fuzzy",
    .entries = fuzzy_entries,
    .count = sizeof(fuzzy_entries) / sizeof(fuzzy_entries[0]),
};

const exprtk_module_t *exprtk_module_fuzzy(void) { return &fuzzy_module; }
