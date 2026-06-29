/**
 * @file exprtk_mod_string.c
 * @brief String module: lower/upper/trim/ltrim/rtrim/contains/starts_with/
 *        ends_with/index_of/substr/replace/reverse/assert/
 *        tokenize/split/token_count/to_num/to_str/to_int/to_double/to_bool
 */
#include "exprtk_module.h"
#include "exprtk.h"
#include "base64_utils.h"
#include "mustache.h"
#include "turbo_str.h"
#include "turbo_str_view.h"
#include "utf8h/utf8.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>

/* ========================================================================= */
/* String-view tokenization and conversion                                    */
/* ========================================================================= */

static int mod_view_contains_char(tstr_v chars, char c) {
    return chars.data && chars.len > 0 && memchr(chars.data, (unsigned char)c, chars.len) != NULL;
}

static int mod_next_token(tstr_v input, tstr_v delimiters, size_t *cursor,
                          bool ignore_empty, tstr_v *out) {
    if (!cursor || !out || !input.data) return 0;

    while (*cursor <= input.len) {
        size_t start = *cursor;
        size_t end = start;
        while (end < input.len && !mod_view_contains_char(delimiters, input.data[end])) {
            end++;
        }

        *cursor = (end < input.len) ? end + 1 : input.len + 1;
        if (end > start || !ignore_empty) {
            *out = tstr_v_from_buf(input.data + start, end - start);
            return 1;
        }
    }

    return 0;
}

static size_t mod_count_tokens(tstr_v input, tstr_v delimiters, bool ignore_empty) {
    size_t cursor = 0;
    size_t count = 0;
    tstr_v token;
    while (mod_next_token(input, delimiters, &cursor, ignore_empty, &token)) {
        count++;
    }
    return count;
}

static char *mod_view_to_parse_cstr(tstr_v s, mem_pool_t *arena,
                                    char *stack_buf, size_t stack_cap) {
    char *buf = NULL;
    if (!stack_buf || stack_cap == 0) return NULL;
    if (s.len + 1 <= stack_cap) {
        buf = stack_buf;
    } else {
        buf = (char *)mem_alloc(arena, s.len + 1);
        if (!buf) return NULL;
    }
    if (s.len > 0 && s.data) memcpy(buf, s.data, s.len);
    buf[s.len] = '\0';
    return buf;
}

static bool mod_to_double_v(tstr_v s, mem_pool_t *arena, double *out) {
    char stack_buf[128];
    char *text = mod_view_to_parse_cstr(s, arena, stack_buf, sizeof(stack_buf));
    if (!text || !out) return false;
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text) return false;
    *out = v;
    return true;
}

static bool mod_to_int_v(tstr_v s, mem_pool_t *arena, long long *out) {
    char stack_buf[128];
    char *text = mod_view_to_parse_cstr(s, arena, stack_buf, sizeof(stack_buf));
    if (!text || !out) return false;
    char *end = NULL;
    long long v = strtoll(text, &end, 0);
    if (end == text) return false;
    *out = v;
    return true;
}

static bool mod_to_bool_v(tstr_v s, bool *out) {
    if (!out) return false;
    if (tstr_v_ieq(s, tstr_v_from_cstr("true")) ||
        tstr_v_ieq(s, tstr_v_from_cstr("yes")) ||
        tstr_v_eq(s, tstr_v_from_cstr("1"))) {
        *out = true;
        return true;
    }
    if (tstr_v_ieq(s, tstr_v_from_cstr("false")) ||
        tstr_v_ieq(s, tstr_v_from_cstr("no")) ||
        tstr_v_eq(s, tstr_v_from_cstr("0"))) {
        *out = false;
        return true;
    }
    return false;
}

static int mod_is_number(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_NUMBER || value.type == EXPRTK_VAL_INTEGER;
}

static double mod_number_value(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_INTEGER ? (double)value.data.integer : value.data.number;
}

static size_t mod_number_to_index(exprtk_value_t value) {
    double n = mod_number_value(value);
    return n <= 0.0 ? 0 : (size_t)n;
}

static long long mod_number_to_signed(exprtk_value_t value) {
    return (long long)mod_number_value(value);
}

static size_t mod_normalize_byte_index(tstr_v s, exprtk_value_t value) {
    long long index = mod_number_to_signed(value);
    if (index < 0) index = (long long)s.len + index;
    if (index < 0) return 0;
    if ((size_t)index > s.len) return s.len;
    return (size_t)index;
}

static char *mod_view_to_arena_cstr(tstr_v s, mem_pool_t *arena) {
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return NULL;
    if (s.len > 0 && s.data) memcpy(buf, s.data, s.len);
    buf[s.len] = '\0';
    return buf;
}

static int mod_utf8_valid_view(tstr_v s) {
    if (s.len == 0) return 1;
    if (!s.data) return 0;
    return utf8nvalid((const utf8_int8_t *)s.data, s.len) == NULL;
}

static size_t mod_utf8_byte_offset(tstr_v s, size_t codepoint_index) {
    size_t offset = 0;
    size_t index = 0;
    while (offset < s.len && index < codepoint_index) {
        offset += utf8codepointcalcsize((const utf8_int8_t *)(s.data + offset));
        index++;
    }
    return offset > s.len ? s.len : offset;
}

static size_t mod_utf8_codepoint_count_before(tstr_v s, size_t byte_offset) {
    size_t offset = 0;
    size_t count = 0;
    if (byte_offset > s.len) byte_offset = s.len;
    while (offset < byte_offset) {
        offset += utf8codepointcalcsize((const utf8_int8_t *)(s.data + offset));
        count++;
    }
    return count;
}

static int mod_utf8_is_boundary(tstr_v s, size_t byte_offset) {
    return byte_offset == 0 || byte_offset == s.len ||
           (byte_offset < s.len && ((unsigned char)s.data[byte_offset] & 0xC0) != 0x80);
}

static exprtk_value_t mod_copy_string(mem_pool_t *arena, const char *data, size_t len) {
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    if (len > 0 && data) memcpy(buf, data, len);
    buf[len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static int mod_list_append_string(exprtk_value_t *list, mem_pool_t *arena,
                                  const char *data, size_t len) {
    exprtk_value_t item = mod_copy_string(arena, data, len);
    if (item.type != EXPRTK_VAL_STRING) return 0;
    exprtk_list_push(list, item);
    return 1;
}

static size_t mod_first_utf8_char_len(tstr_v s) {
    if (s.len == 0 || !s.data) return 0;
    if (!mod_utf8_valid_view(s)) return 1;
    size_t n = utf8codepointcalcsize((const utf8_int8_t *)s.data);
    return n > s.len ? s.len : n;
}

static int mod_view_contains_span(tstr_v haystack, const char *data, size_t len) {
    if (!data || len == 0) return 0;
    if (haystack.len < len) return 0;
    size_t pos = 0;
    while (pos < haystack.len) {
        size_t n = mod_utf8_valid_view(tstr_v_from_buf(haystack.data + pos, haystack.len - pos))
                       ? utf8codepointcalcsize((const utf8_int8_t *)(haystack.data + pos))
                       : 1;
        if (n == 0 || pos + n > haystack.len) n = 1;
        if (n == len && memcmp(haystack.data + pos, data, len) == 0) return 1;
        pos += n;
    }
    return 0;
}

static int mod_is_ascii_alnum(unsigned char c) {
    return isalnum(c) != 0;
}

static int mod_is_unreserved_url_char(unsigned char c) {
    return mod_is_ascii_alnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static size_t mod_string_unit_count(tstr_v s) {
    return mod_utf8_valid_view(s) ? utf8nlen((const utf8_int8_t *)s.data, s.len) : s.len;
}

static size_t mod_string_byte_offset(tstr_v s, size_t units) {
    return mod_utf8_valid_view(s) ? mod_utf8_byte_offset(s, units) : (units > s.len ? s.len : units);
}

static size_t mod_normalize_unit_index(size_t unit_count, exprtk_value_t value) {
    long long index = mod_number_to_signed(value);
    if (index < 0) index = (long long)unit_count + index;
    if (index < 0) return 0;
    if ((size_t)index > unit_count) return unit_count;
    return (size_t)index;
}

static int mod_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static int mod_hex4_value(const char *data) {
    int a = mod_hex_value((unsigned char)data[0]);
    int b = mod_hex_value((unsigned char)data[1]);
    int c = mod_hex_value((unsigned char)data[2]);
    int d = mod_hex_value((unsigned char)data[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
    return (a << 12) | (b << 8) | (c << 4) | d;
}

static size_t mod_utf8_write_codepoint(char *out, size_t cap, int cp) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    size_t n = utf8codepointsize((utf8_int32_t)cp);
    if (n == 0 || n > cap) return 0;
    utf8catcodepoint((utf8_int8_t *)out, (utf8_int32_t)cp, n);
    return n;
}

static exprtk_value_t fn_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING)
        return exprtk_val_num((double)args[0].data.string.len);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_lower(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = mem_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = (char)tolower((unsigned char)s.data[i]);
            return exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_upper(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = mem_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = (char)toupper((unsigned char)s.data[i]);
            return exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_trim(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING)
        return exprtk_val_str(tstr_v_trim(args[0].data.string, " \t\r\n"));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ltrim(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING)
        return exprtk_val_str(tstr_v_trim_left(args[0].data.string, " \t\r\n"));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_rtrim(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING)
        return exprtk_val_str(tstr_v_trim_right(args[0].data.string, " \t\r\n"));
    return exprtk_val_num(0);
}

static exprtk_value_t fn_is_empty(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc == 1 && args[0].type == EXPRTK_VAL_STRING &&
                          args[0].data.string.len == 0 ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_blank(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if (!isspace((unsigned char)s.data[i])) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_ascii(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if ((unsigned char)s.data[i] > 0x7F) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_hex(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if (!isxdigit((unsigned char)s.data[i])) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_printable(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (c >= 0x80) return exprtk_val_num(0);
        if (!isprint(c) && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f')
            return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_digit(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if (!isdigit((unsigned char)s.data[i])) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_alpha(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if (!isalpha((unsigned char)s.data[i])) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_alnum(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if (!isalnum((unsigned char)s.data[i])) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_space(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    for (size_t i = 0; i < s.len; ++i) {
        if (!isspace((unsigned char)s.data[i])) return exprtk_val_num(0);
    }
    return exprtk_val_num(1);
}

static exprtk_value_t fn_is_lower(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    int has_cased = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (isupper(c)) return exprtk_val_num(0);
        if (islower(c)) has_cased = 1;
    }
    return exprtk_val_num(has_cased ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_upper(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING || args[0].data.string.len == 0)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    int has_cased = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (islower(c)) return exprtk_val_num(0);
        if (isupper(c)) has_cased = 1;
    }
    return exprtk_val_num(has_cased ? 1.0 : 0.0);
}

static exprtk_value_t fn_contains(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING)
        return exprtk_val_num(tstr_v_contains(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_starts_with(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING)
        return exprtk_val_num(tstr_v_starts_with(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_ends_with(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING)
        return exprtk_val_num(tstr_v_ends_with(args[0].data.string, args[1].data.string) ? 1.0 : 0.0);
    return exprtk_val_num(0);
}

static exprtk_value_t fn_index_of(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
        size_t pos = tstr_v_find(args[0].data.string, args[1].data.string);
        return exprtk_val_num(pos == TSTR_V_NPOS ? -1.0 : (double)pos);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_last_index_of(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
        size_t pos = tstr_v_rfind(args[0].data.string, args[1].data.string);
        return exprtk_val_num(pos == TSTR_V_NPOS ? -1.0 : (double)pos);
    }
    return exprtk_val_num(-1);
}

static exprtk_value_t fn_find(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_STRING &&
        args[1].type == EXPRTK_VAL_STRING && (argc == 2 || mod_is_number(args[2]))) {
        tstr_v s = args[0].data.string;
        size_t start = argc == 3 ? mod_number_to_index(args[2]) : 0;
        if (start > s.len) return exprtk_val_num(-1);
        tstr_v hay = tstr_v_from_buf(s.data + start, s.len - start);
        size_t pos = tstr_v_find(hay, args[1].data.string);
        return exprtk_val_num(pos == TSTR_V_NPOS ? -1.0 : (double)(start + pos));
    }
    return exprtk_val_num(-1);
}

static exprtk_value_t fn_rfind(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_STRING &&
        args[1].type == EXPRTK_VAL_STRING && (argc == 2 || mod_is_number(args[2]))) {
        tstr_v s = args[0].data.string;
        size_t end = argc == 3 ? mod_number_to_index(args[2]) : s.len;
        if (end > s.len) end = s.len;
        tstr_v hay = tstr_v_from_buf(s.data, end);
        size_t pos = tstr_v_rfind(hay, args[1].data.string);
        return exprtk_val_num(pos == TSTR_V_NPOS ? -1.0 : (double)pos);
    }
    return exprtk_val_num(-1);
}

static exprtk_value_t fn_find_all(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    tstr_v sub = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    if (sub.len == 0) return list;

    size_t cursor = 0;
    while (cursor <= s.len) {
        size_t found = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), sub);
        if (found == TSTR_V_NPOS) break;
        size_t pos = cursor + found;
        exprtk_list_push(&list, exprtk_val_num((double)pos));
        cursor = pos + sub.len;
    }
    return list;
}

static exprtk_value_t fn_find_all_overlapping(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    tstr_v sub = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    if (sub.len == 0) return list;

    size_t cursor = 0;
    while (cursor <= s.len) {
        size_t found = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), sub);
        if (found == TSTR_V_NPOS) break;
        size_t pos = cursor + found;
        exprtk_list_push(&list, exprtk_val_num((double)pos));
        cursor = pos + 1;
    }
    return list;
}

static exprtk_value_t fn_substr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_STRING) {
        size_t start = (size_t)args[1].data.number;
        size_t len = (argc == 3) ? (size_t)args[2].data.number : TSTR_V_NPOS;
        return exprtk_val_str(tstr_v_sub(args[0].data.string, start, len));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_slice(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc == 2 || argc == 3) && args[0].type == EXPRTK_VAL_STRING &&
        mod_is_number(args[1]) && (argc == 2 || mod_is_number(args[2]))) {
        tstr_v s = args[0].data.string;
        double start_num = mod_number_value(args[1]);
        double end_num = argc == 3 ? mod_number_value(args[2]) : (double)s.len;
        long long start = start_num < 0.0 ? (long long)s.len + (long long)start_num : (long long)start_num;
        long long end = end_num < 0.0 ? (long long)s.len + (long long)end_num : (long long)end_num;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if ((size_t)start > s.len) start = (long long)s.len;
        if ((size_t)end > s.len) end = (long long)s.len;
        if (end < start) end = start;
        return mod_copy_string(arena, s.data + start, (size_t)(end - start));
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_char_at(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && mod_is_number(args[1])) {
        tstr_v s = args[0].data.string;
        size_t index = mod_number_to_index(args[1]);
        if (index >= s.len) return exprtk_val_str(tstr_v_from_buf("", 0));
        return mod_copy_string(arena, s.data + index, 1);
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_replace(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 3 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING && args[2].type == EXPRTK_VAL_STRING) {
        tstr_v s = args[0].data.string;
        tstr_v old_v = args[1].data.string;
        tstr_v new_v = args[2].data.string;
        if (old_v.len == 0) return args[0];
        size_t count = 0, curr = 0;
        while (curr <= s.len) {
            size_t p = tstr_v_find(tstr_v_sub(s, curr, TSTR_V_NPOS), old_v);
            if (p == TSTR_V_NPOS) break;
            count++;
            curr += p + old_v.len;
            if (old_v.len == 0) break;
        }
        if (count == 0) return args[0];
        size_t new_len = s.len + count * (new_v.len - old_v.len);
        char *buf = mem_alloc(arena, new_len);
        if (buf) {
            char *dest = buf;
            size_t last_src = 0;
            while (last_src < s.len) {
                size_t p = tstr_v_find(tstr_v_sub(s, last_src, TSTR_V_NPOS), old_v);
                if (p == TSTR_V_NPOS) break;
                memcpy(dest, s.data + last_src, p);
                dest += p;
                memcpy(dest, new_v.data, new_v.len);
                dest += new_v.len;
                last_src += p + old_v.len;
            }
            if (last_src < s.len) memcpy(dest, s.data + last_src, s.len - last_src);
            return exprtk_val_str(tstr_v_from_buf(buf, new_len));
        }
    }
    return exprtk_val_num(0);
}

static exprtk_value_t fn_reverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
        tstr_v s = args[0].data.string;
        char *buf = mem_alloc(arena, s.len);
        if (buf) {
            for (size_t i = 0; i < s.len; ++i) buf[i] = s.data[s.len - 1 - i];
            return exprtk_val_str(tstr_v_from_buf(buf, s.len));
        }
    }
    return exprtk_val_num(0);
}

/* utf8_valid(str) -> number: validate UTF-8 byte sequence */
static exprtk_value_t fn_utf8_valid(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    return exprtk_val_num(mod_utf8_valid_view(args[0].data.string) ? 1.0 : 0.0);
}

/* utf8_len(str) -> number: codepoint length */
static exprtk_value_t fn_utf8_len(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(-1.0);
    return exprtk_val_num((double)utf8nlen((const utf8_int8_t *)s.data, s.len));
}

/* utf8_index_of(str, needle) -> number: codepoint index, -1 if not found */
static exprtk_value_t fn_utf8_index_of(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v needle = args[1].data.string;
    if (!mod_utf8_valid_view(s) || !mod_utf8_valid_view(needle)) return exprtk_val_num(-1.0);
    size_t pos = tstr_v_find(s, needle);
    if (pos == TSTR_V_NPOS) return exprtk_val_num(-1.0);
    return exprtk_val_num((double)mod_utf8_codepoint_count_before(s, pos));
}

/* utf8_substr(str, start, [len]) -> string: codepoint slice */
static exprtk_value_t fn_utf8_substr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    if (argc == 3 && !mod_is_number(args[2])) return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);

    size_t start = mod_number_to_index(args[1]);
    size_t start_byte = mod_utf8_byte_offset(s, start);
    size_t end_byte = s.len;
    if (argc == 3) {
        size_t count = mod_number_to_index(args[2]);
        end_byte = mod_utf8_byte_offset(tstr_v_from_buf(s.data + start_byte, s.len - start_byte), count) + start_byte;
    }
    return mod_copy_string(arena, s.data + start_byte, end_byte - start_byte);
}

static exprtk_value_t fn_utf8_slice(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    if (argc == 3 && !mod_is_number(args[2])) return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
    size_t units = utf8nlen((const utf8_int8_t *)s.data, s.len);
    size_t start = mod_normalize_unit_index(units, args[1]);
    size_t end = argc == 3 ? mod_normalize_unit_index(units, args[2]) : units;
    if (end < start) end = start;
    size_t start_byte = mod_utf8_byte_offset(s, start);
    size_t end_byte = mod_utf8_byte_offset(s, end);
    return mod_copy_string(arena, s.data + start_byte, end_byte - start_byte);
}

static exprtk_value_t fn_from_codepoint(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    size_t len = 0;
    for (size_t i = 0; i < argc; ++i) {
        if (!mod_is_number(args[i])) return exprtk_val_num(0);
        long long cp = mod_number_to_signed(args[i]);
        if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return exprtk_val_num(0);
        len += utf8codepointsize((utf8_int32_t)cp);
    }
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < argc; ++i) {
        long long cp = mod_number_to_signed(args[i]);
        size_t n = mod_utf8_write_codepoint(buf + pos, len + 1 - pos, (int)cp);
        if (n == 0) return exprtk_val_num(0);
        pos += n;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_chr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    return fn_from_codepoint(argc, args, env, arena);
}

static exprtk_value_t fn_ord(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (argc == 2 && !mod_is_number(args[1])) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(-1.0);
    size_t index = argc == 2 ? mod_number_to_index(args[1]) : 0;
    size_t start = mod_utf8_byte_offset(s, index);
    if (start >= s.len) return exprtk_val_num(-1.0);
    utf8_int32_t cp = 0;
    utf8codepoint((const utf8_int8_t *)(s.data + start), &cp);
    return exprtk_val_num((double)cp);
}

static exprtk_value_t fn_byte_at(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t index = mod_number_to_index(args[1]);
    if (index >= s.len) return exprtk_val_num(-1.0);
    return exprtk_val_num((double)(unsigned char)s.data[index]);
}

/* utf8_lower/utf8_upper(str) -> string */
static exprtk_value_t fn_utf8_lower(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (!mod_utf8_valid_view(args[0].data.string)) return exprtk_val_num(0);
    char *buf = mod_view_to_arena_cstr(args[0].data.string, arena);
    if (!buf) return exprtk_val_num(0);
    utf8lwr((utf8_int8_t *)buf);
    return exprtk_val_str(tstr_v_from_buf(buf, strlen(buf)));
}

static exprtk_value_t fn_utf8_upper(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (!mod_utf8_valid_view(args[0].data.string)) return exprtk_val_num(0);
    char *buf = mod_view_to_arena_cstr(args[0].data.string, arena);
    if (!buf) return exprtk_val_num(0);
    utf8upr((utf8_int8_t *)buf);
    return exprtk_val_str(tstr_v_from_buf(buf, strlen(buf)));
}

/* str_split(str, sep) -> list<string>: Python/JS-style substring separator */
static exprtk_value_t fn_str_split(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    exprtk_value_t list = exprtk_val_list_empty();

    if (sep.len == 0) {
        if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
        size_t offset = 0;
        while (offset < s.len) {
            size_t n = utf8codepointcalcsize((const utf8_int8_t *)(s.data + offset));
            if (!mod_list_append_string(&list, arena, s.data + offset, n)) return exprtk_val_num(0);
            offset += n;
        }
        return list;
    }

    size_t cursor = 0;
    while (cursor <= s.len) {
        tstr_v rest = tstr_v_from_buf(s.data + cursor, s.len - cursor);
        size_t found = tstr_v_find(rest, sep);
        size_t part_len = (found == TSTR_V_NPOS) ? (s.len - cursor) : found;
        if (!mod_list_append_string(&list, arena, s.data + cursor, part_len)) return exprtk_val_num(0);
        if (found == TSTR_V_NPOS) break;
        cursor += found + sep.len;
    }
    return list;
}

static exprtk_value_t fn_split_limit(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING ||
        !mod_is_number(args[2]))
        return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    size_t maxsplit = mod_number_to_index(args[2]);
    exprtk_value_t list = exprtk_val_list_empty();

    if (maxsplit == 0) {
        if (!mod_list_append_string(&list, arena, s.data, s.len)) return exprtk_val_num(0);
        return list;
    }

    if (sep.len == 0) {
        if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
        size_t offset = 0;
        size_t splits = 0;
        while (offset < s.len && splits < maxsplit) {
            size_t n = utf8codepointcalcsize((const utf8_int8_t *)(s.data + offset));
            if (!mod_list_append_string(&list, arena, s.data + offset, n)) return exprtk_val_num(0);
            offset += n;
            splits++;
        }
        if (!mod_list_append_string(&list, arena, s.data + offset, s.len - offset)) return exprtk_val_num(0);
        return list;
    }

    size_t cursor = 0;
    size_t splits = 0;
    while (cursor <= s.len && splits < maxsplit) {
        size_t found = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), sep);
        if (found == TSTR_V_NPOS) break;
        if (!mod_list_append_string(&list, arena, s.data + cursor, found)) return exprtk_val_num(0);
        cursor += found + sep.len;
        splits++;
    }
    if (!mod_list_append_string(&list, arena, s.data + cursor, s.len - cursor)) return exprtk_val_num(0);
    return list;
}

static exprtk_value_t fn_rsplit(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || (argc == 3 && !mod_is_number(args[2])))
        return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    if (sep.len == 0) return exprtk_val_num(0);

    size_t total = 0;
    size_t cursor = 0;
    while (cursor <= s.len) {
        size_t found = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), sep);
        if (found == TSTR_V_NPOS) break;
        total++;
        cursor += found + sep.len;
    }

    exprtk_value_t list = exprtk_val_list_empty();
    if (total == 0) {
        if (!mod_list_append_string(&list, arena, s.data, s.len)) return exprtk_val_num(0);
        return list;
    }

    size_t maxsplit = argc == 3 ? mod_number_to_index(args[2]) : total;
    if (maxsplit == 0) {
        if (!mod_list_append_string(&list, arena, s.data, s.len)) return exprtk_val_num(0);
        return list;
    }
    if (maxsplit > total) maxsplit = total;

    size_t *positions = (size_t *)mem_alloc(arena, total * sizeof(size_t));
    if (!positions) return exprtk_val_num(0);
    cursor = 0;
    for (size_t i = 0; i < total; ++i) {
        size_t found = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), sep);
        positions[i] = cursor + found;
        cursor = positions[i] + sep.len;
    }

    size_t first_sep = total - maxsplit;
    size_t start = 0;
    if (!mod_list_append_string(&list, arena, s.data, positions[first_sep]))
        return exprtk_val_num(0);
    start = positions[first_sep] + sep.len;
    for (size_t i = first_sep + 1; i < total; ++i) {
        if (!mod_list_append_string(&list, arena, s.data + start, positions[i] - start))
            return exprtk_val_num(0);
        start = positions[i] + sep.len;
    }
    if (!mod_list_append_string(&list, arena, s.data + start, s.len - start))
        return exprtk_val_num(0);
    return list;
}

static exprtk_value_t fn_split_once(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    if (sep.len == 0) return exprtk_val_num(0);
    size_t pos = tstr_v_find(s, sep);
    exprtk_value_t list = exprtk_val_list_empty();
    if (pos == TSTR_V_NPOS) {
        if (!mod_list_append_string(&list, arena, s.data, s.len)) return exprtk_val_num(0);
        if (!mod_list_append_string(&list, arena, "", 0)) return exprtk_val_num(0);
        return list;
    }
    if (!mod_list_append_string(&list, arena, s.data, pos)) return exprtk_val_num(0);
    if (!mod_list_append_string(&list, arena, s.data + pos + sep.len, s.len - pos - sep.len))
        return exprtk_val_num(0);
    return list;
}

static exprtk_value_t fn_rsplit_once(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    if (sep.len == 0) return exprtk_val_num(0);
    size_t pos = tstr_v_rfind(s, sep);
    exprtk_value_t list = exprtk_val_list_empty();
    if (pos == TSTR_V_NPOS) {
        if (!mod_list_append_string(&list, arena, s.data, s.len)) return exprtk_val_num(0);
        if (!mod_list_append_string(&list, arena, "", 0)) return exprtk_val_num(0);
        return list;
    }
    if (!mod_list_append_string(&list, arena, s.data, pos)) return exprtk_val_num(0);
    if (!mod_list_append_string(&list, arena, s.data + pos + sep.len, s.len - pos - sep.len))
        return exprtk_val_num(0);
    return list;
}

/* str_join(list, sep) -> string */
static exprtk_value_t fn_str_join(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_LIST || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    exprtk_value_t list = args[0];
    tstr_v sep = args[1].data.string;
    size_t cap = 1 + (list.data.list.count > 0 ? (list.data.list.count - 1) * sep.len : 0);
    char tmp[64];
    for (size_t i = 0; i < list.data.list.count; ++i) {
        exprtk_value_t item = list.data.list.items[i];
        if (item.type == EXPRTK_VAL_STRING) {
            cap += item.data.string.len;
        } else if (mod_is_number(item)) {
            int n = snprintf(tmp, sizeof(tmp), "%g", mod_number_value(item));
            if (n > 0) cap += (size_t)n;
        }
    }

    char *buf = (char *)mem_alloc(arena, cap);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < list.data.list.count; ++i) {
        exprtk_value_t item = list.data.list.items[i];
        if (i > 0 && sep.len > 0) {
            memcpy(buf + pos, sep.data, sep.len);
            pos += sep.len;
        }
        if (item.type == EXPRTK_VAL_STRING) {
            memcpy(buf + pos, item.data.string.data, item.data.string.len);
            pos += item.data.string.len;
        } else if (mod_is_number(item)) {
            int n = snprintf(buf + pos, cap - pos, "%g", mod_number_value(item));
            if (n > 0) pos += (size_t)n;
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_utf8_rindex_of(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v needle = args[1].data.string;
    if (!mod_utf8_valid_view(s) || !mod_utf8_valid_view(needle)) return exprtk_val_num(-1.0);
    size_t pos = tstr_v_rfind(s, needle);
    if (pos == TSTR_V_NPOS) return exprtk_val_num(-1.0);
    return exprtk_val_num((double)mod_utf8_codepoint_count_before(s, pos));
}

static exprtk_value_t fn_utf8_char_at(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
    size_t start = mod_utf8_byte_offset(s, mod_number_to_index(args[1]));
    if (start >= s.len) return exprtk_val_str(tstr_v_from_buf("", 0));
    size_t end = mod_utf8_byte_offset(tstr_v_from_buf(s.data + start, s.len - start), 1) + start;
    return mod_copy_string(arena, s.data + start, end - start);
}

static exprtk_value_t fn_utf8_codepoint_at(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(-1.0);
    size_t start = mod_utf8_byte_offset(s, mod_number_to_index(args[1]));
    if (start >= s.len) return exprtk_val_num(-1.0);
    utf8_int32_t cp = 0;
    utf8codepoint((const utf8_int8_t *)(s.data + start), &cp);
    return exprtk_val_num((double)cp);
}

static exprtk_value_t fn_utf8_reverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t read = s.len;
    size_t write = 0;
    while (read > 0) {
        size_t start = read - 1;
        while (start > 0 && ((unsigned char)s.data[start] & 0xC0) == 0x80) start--;
        memcpy(buf + write, s.data + start, read - start);
        write += read - start;
        read = start;
    }
    buf[write] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, write));
}

static exprtk_value_t fn_str_count_substr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v needle = args[1].data.string;
    if (needle.len == 0) return exprtk_val_num(0);
    size_t cursor = 0;
    size_t count = 0;
    while (cursor <= s.len) {
        size_t pos = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), needle);
        if (pos == TSTR_V_NPOS) break;
        count++;
        cursor += pos + needle.len;
    }
    return exprtk_val_num((double)count);
}

static exprtk_value_t fn_str_count_overlapping(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v needle = args[1].data.string;
    if (needle.len == 0) return exprtk_val_num(0);
    size_t cursor = 0;
    size_t count = 0;
    while (cursor <= s.len) {
        size_t pos = tstr_v_find(tstr_v_from_buf(s.data + cursor, s.len - cursor), needle);
        if (pos == TSTR_V_NPOS) break;
        count++;
        cursor += pos + 1;
    }
    return exprtk_val_num((double)count);
}

static exprtk_value_t fn_str_repeat(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t count = mod_number_to_index(args[1]);
    size_t len = s.len * count;
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s.len > 0) memcpy(buf + pos, s.data, s.len);
        pos += s.len;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_str_lines(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t start = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] != '\n' && s.data[i] != '\r') continue;
        if (!mod_list_append_string(&list, arena, s.data + start, i - start)) return exprtk_val_num(0);
        if (s.data[i] == '\r' && i + 1 < s.len && s.data[i + 1] == '\n') i++;
        start = i + 1;
    }
    if (start < s.len) {
        if (!mod_list_append_string(&list, arena, s.data + start, s.len - start)) return exprtk_val_num(0);
    }
    return list;
}

static exprtk_value_t fn_line_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (s.len == 0) return exprtk_val_num(0);
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] != '\n' && s.data[i] != '\r') continue;
        count++;
        if (s.data[i] == '\r' && i + 1 < s.len && s.data[i + 1] == '\n') i++;
        start = i + 1;
    }
    if (start < s.len) count++;
    return exprtk_val_num((double)count);
}

static exprtk_value_t fn_chomp(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t len = s.len;
    if (len > 0 && s.data[len - 1] == '\n') {
        len--;
        if (len > 0 && s.data[len - 1] == '\r') len--;
    } else if (len > 0 && s.data[len - 1] == '\r') {
        len--;
    }
    return mod_copy_string(arena, s.data, len);
}

static exprtk_value_t fn_expand_tabs(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING ||
        (argc == 2 && !mod_is_number(args[1])))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t tabsize = argc == 2 ? mod_number_to_index(args[1]) : 8;
    int valid = mod_utf8_valid_view(s);
    size_t len = 0;
    size_t col = 0;
    for (size_t i = 0; i < s.len;) {
        unsigned char c = (unsigned char)s.data[i];
        if (c == '\t') {
            size_t spaces = tabsize == 0 ? 0 : tabsize - (col % tabsize);
            len += spaces;
            col += spaces;
            i++;
        } else {
            size_t n = valid ? utf8codepointcalcsize((const utf8_int8_t *)(s.data + i)) : 1;
            if (n == 0 || i + n > s.len) n = 1;
            len += n;
            i += n;
            col = (c == '\n' || c == '\r') ? 0 : col + 1;
        }
    }
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    col = 0;
    for (size_t i = 0; i < s.len;) {
        unsigned char c = (unsigned char)s.data[i];
        if (c == '\t') {
            size_t spaces = tabsize == 0 ? 0 : tabsize - (col % tabsize);
            for (size_t j = 0; j < spaces; ++j) buf[pos++] = ' ';
            col += spaces;
            i++;
        } else {
            size_t n = valid ? utf8codepointcalcsize((const utf8_int8_t *)(s.data + i)) : 1;
            if (n == 0 || i + n > s.len) n = 1;
            memcpy(buf + pos, s.data + i, n);
            pos += n;
            i += n;
            col = (c == '\n' || c == '\r') ? 0 : col + 1;
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_indent(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || (argc == 3 && !mod_is_number(args[2])))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v prefix = args[1].data.string;
    int include_first = argc == 3 ? fabs(mod_number_value(args[2])) > 1e-9 : 1;
    size_t lines = s.len > 0 ? 1 : 0;
    for (size_t i = 0; i < s.len; ++i) {
        if ((s.data[i] == '\n' || s.data[i] == '\r') && i + 1 < s.len) {
            if (s.data[i] == '\r' && s.data[i + 1] == '\n') i++;
            lines++;
        }
    }
    size_t prefixes = lines;
    if (!include_first && prefixes > 0) prefixes--;
    char *buf = (char *)mem_alloc(arena, s.len + prefixes * prefix.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    int at_line_start = 1;
    size_t line_index = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (at_line_start) {
            if ((include_first || line_index > 0) && prefix.len > 0) {
                memcpy(buf + pos, prefix.data, prefix.len);
                pos += prefix.len;
            }
            at_line_start = 0;
        }
        char c = s.data[i];
        buf[pos++] = c;
        if (c == '\r') {
            if (i + 1 < s.len && s.data[i + 1] == '\n') {
                buf[pos++] = s.data[++i];
            }
            if (i + 1 < s.len) {
                at_line_start = 1;
                line_index++;
            }
        } else if (c == '\n' && i + 1 < s.len) {
            at_line_start = 1;
            line_index++;
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_dedent(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t min_indent = (size_t)-1;
    size_t line_start = 0;
    while (line_start < s.len) {
        size_t line_end = line_start;
        while (line_end < s.len && s.data[line_end] != '\n' && s.data[line_end] != '\r') line_end++;
        size_t indent = 0;
        while (line_start + indent < line_end &&
               (s.data[line_start + indent] == ' ' || s.data[line_start + indent] == '\t')) {
            indent++;
        }
        if (line_start + indent < line_end && indent < min_indent) min_indent = indent;
        if (line_end < s.len && s.data[line_end] == '\r' && line_end + 1 < s.len && s.data[line_end + 1] == '\n')
            line_end++;
        line_start = line_end + 1;
    }
    if (min_indent == (size_t)-1 || min_indent == 0) return mod_copy_string(arena, s.data, s.len);
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    line_start = 0;
    while (line_start < s.len) {
        size_t line_end = line_start;
        while (line_end < s.len && s.data[line_end] != '\n' && s.data[line_end] != '\r') line_end++;
        size_t remove = 0;
        while (remove < min_indent && line_start + remove < line_end &&
               (s.data[line_start + remove] == ' ' || s.data[line_start + remove] == '\t')) {
            remove++;
        }
        if (line_end > line_start + remove) {
            memcpy(buf + pos, s.data + line_start + remove, line_end - line_start - remove);
            pos += line_end - line_start - remove;
        }
        if (line_end < s.len) {
            buf[pos++] = s.data[line_end];
            if (s.data[line_end] == '\r' && line_end + 1 < s.len && s.data[line_end + 1] == '\n')
                buf[pos++] = s.data[++line_end];
        }
        line_start = line_end + 1;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_csv_escape(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    int needs_quote = 0;
    size_t quote_count = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == '"') quote_count++;
        if (s.data[i] == '"' || s.data[i] == ',' || s.data[i] == '\n' || s.data[i] == '\r') needs_quote = 1;
    }
    if (!needs_quote) return mod_copy_string(arena, s.data, s.len);
    char *buf = (char *)mem_alloc(arena, s.len + quote_count + 3);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    buf[pos++] = '"';
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == '"') buf[pos++] = '"';
        buf[pos++] = s.data[i];
    }
    buf[pos++] = '"';
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_csv_unescape(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (s.len < 2 || s.data[0] != '"' || s.data[s.len - 1] != '"')
        return mod_copy_string(arena, s.data, s.len);
    char *buf = (char *)mem_alloc(arena, s.len - 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 1; i + 1 < s.len; ++i) {
        if (s.data[i] == '"' && i + 1 < s.len - 1 && s.data[i + 1] == '"') i++;
        buf[pos++] = s.data[i];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_trim_chars(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    char *chars = mod_view_to_arena_cstr(args[1].data.string, arena);
    if (!chars) return exprtk_val_num(0);
    return exprtk_val_str(tstr_v_trim(args[0].data.string, chars));
}

static exprtk_value_t fn_ltrim_chars(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    char *chars = mod_view_to_arena_cstr(args[1].data.string, arena);
    if (!chars) return exprtk_val_num(0);
    return exprtk_val_str(tstr_v_trim_left(args[0].data.string, chars));
}

static exprtk_value_t fn_rtrim_chars(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    char *chars = mod_view_to_arena_cstr(args[1].data.string, arena);
    if (!chars) return exprtk_val_num(0);
    return exprtk_val_str(tstr_v_trim_right(args[0].data.string, chars));
}

static exprtk_value_t fn_str_eq_ci(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (!mod_utf8_valid_view(args[0].data.string) || !mod_utf8_valid_view(args[1].data.string))
        return exprtk_val_num(0);
    char *lhs = mod_view_to_arena_cstr(args[0].data.string, arena);
    char *rhs = mod_view_to_arena_cstr(args[1].data.string, arena);
    if (!lhs || !rhs) return exprtk_val_num(0);
    return exprtk_val_num(utf8casecmp((const utf8_int8_t *)lhs, (const utf8_int8_t *)rhs) == 0 ? 1.0 : 0.0);
}

static exprtk_value_t fn_str_contains_ci(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (!mod_utf8_valid_view(args[0].data.string) || !mod_utf8_valid_view(args[1].data.string))
        return exprtk_val_num(0);
    char *haystack = mod_view_to_arena_cstr(args[0].data.string, arena);
    char *needle = mod_view_to_arena_cstr(args[1].data.string, arena);
    if (!haystack || !needle) return exprtk_val_num(0);
    return exprtk_val_num(utf8casestr((const utf8_int8_t *)haystack,
                                      (const utf8_int8_t *)needle) ? 1.0 : 0.0);
}

static exprtk_value_t fn_replace_once(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v old_v = args[1].data.string;
    tstr_v new_v = args[2].data.string;
    if (old_v.len == 0) return mod_copy_string(arena, s.data, s.len);
    size_t pos = tstr_v_find(s, old_v);
    if (pos == TSTR_V_NPOS) return mod_copy_string(arena, s.data, s.len);
    size_t new_len = s.len - old_v.len + new_v.len;
    char *buf = (char *)mem_alloc(arena, new_len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, s.data, pos);
    memcpy(buf + pos, new_v.data, new_v.len);
    memcpy(buf + pos + new_v.len, s.data + pos + old_v.len, s.len - pos - old_v.len);
    buf[new_len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, new_len));
}

static exprtk_value_t fn_remove_prefix(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v prefix = args[1].data.string;
    if (tstr_v_starts_with(s, prefix))
        return mod_copy_string(arena, s.data + prefix.len, s.len - prefix.len);
    return mod_copy_string(arena, s.data, s.len);
}

static exprtk_value_t fn_remove_suffix(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v suffix = args[1].data.string;
    if (tstr_v_ends_with(s, suffix))
        return mod_copy_string(arena, s.data, s.len - suffix.len);
    return mod_copy_string(arena, s.data, s.len);
}

static exprtk_value_t fn_surround(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || (argc == 3 && args[2].type != EXPRTK_VAL_STRING))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v prefix = args[1].data.string;
    tstr_v suffix = argc == 3 ? args[2].data.string : prefix;
    size_t len = prefix.len + s.len + suffix.len;
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    if (prefix.len > 0) {
        memcpy(buf + pos, prefix.data, prefix.len);
        pos += prefix.len;
    }
    if (s.len > 0) {
        memcpy(buf + pos, s.data, s.len);
        pos += s.len;
    }
    if (suffix.len > 0) {
        memcpy(buf + pos, suffix.data, suffix.len);
        pos += suffix.len;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_unwrap(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || (argc == 3 && args[2].type != EXPRTK_VAL_STRING))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v prefix = args[1].data.string;
    tstr_v suffix = argc == 3 ? args[2].data.string : prefix;
    if (prefix.len + suffix.len <= s.len &&
        tstr_v_starts_with(s, prefix) && tstr_v_ends_with(s, suffix)) {
        return mod_copy_string(arena, s.data + prefix.len, s.len - prefix.len - suffix.len);
    }
    return mod_copy_string(arena, s.data, s.len);
}

static exprtk_value_t fn_normalize_space(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    size_t i = 0;
    while (i < s.len && isspace((unsigned char)s.data[i])) i++;
    int pending_space = 0;
    for (; i < s.len; ++i) {
        if (isspace((unsigned char)s.data[i])) {
            pending_space = pos > 0;
            continue;
        }
        if (pending_space) {
            buf[pos++] = ' ';
            pending_space = 0;
        }
        buf[pos++] = s.data[i];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_center(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v fill = (argc == 3 && args[2].type == EXPRTK_VAL_STRING) ? args[2].data.string : tstr_v_from_cstr(" ");
    size_t fill_len = mod_first_utf8_char_len(fill);
    if (fill_len == 0) fill = tstr_v_from_cstr(" "), fill_len = 1;
    size_t width = mod_number_to_index(args[1]);
    size_t current = mod_string_unit_count(s);
    if (width <= current) return mod_copy_string(arena, s.data, s.len);
    size_t pad = width - current;
    size_t left = pad / 2;
    size_t right = pad - left;
    size_t len = s.len + pad * fill_len;
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < left; ++i) {
        memcpy(buf + pos, fill.data, fill_len);
        pos += fill_len;
    }
    memcpy(buf + pos, s.data, s.len);
    pos += s.len;
    for (size_t i = 0; i < right; ++i) {
        memcpy(buf + pos, fill.data, fill_len);
        pos += fill_len;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_replace_range(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 4 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]) ||
        !mod_is_number(args[2]) || args[3].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v replacement = args[3].data.string;
    size_t start = mod_normalize_byte_index(s, args[1]);
    size_t len = mod_number_to_index(args[2]);
    if (len > s.len - start) len = s.len - start;
    size_t out_len = s.len - len + replacement.len;
    char *buf = (char *)mem_alloc(arena, out_len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, s.data, start);
    memcpy(buf + start, replacement.data, replacement.len);
    memcpy(buf + start + replacement.len, s.data + start + len, s.len - start - len);
    buf[out_len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, out_len));
}

static exprtk_value_t fn_insert(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]) ||
        args[2].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    exprtk_value_t range_args[4] = { args[0], args[1], exprtk_val_num(0), args[2] };
    return fn_replace_range(4, range_args, env, arena);
}

static exprtk_value_t fn_delete_range(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]) || !mod_is_number(args[2]))
        return exprtk_val_num(0);
    exprtk_value_t range_args[4] = { args[0], args[1], args[2], exprtk_val_str(tstr_v_from_buf("", 0)) };
    return fn_replace_range(4, range_args, env, arena);
}

static exprtk_value_t fn_truncate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]) ||
        (argc == 3 && args[2].type != EXPRTK_VAL_STRING))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v suffix = argc == 3 ? args[2].data.string : tstr_v_from_cstr("...");
    size_t max_units = mod_number_to_index(args[1]);
    size_t current = mod_string_unit_count(s);
    if (current <= max_units) return mod_copy_string(arena, s.data, s.len);

    size_t suffix_units = mod_string_unit_count(suffix);
    if (suffix_units > max_units) suffix_units = max_units;
    size_t keep_units = max_units - suffix_units;
    size_t keep_bytes = mod_string_byte_offset(s, keep_units);
    size_t suffix_bytes = mod_string_byte_offset(suffix, suffix_units);
    size_t out_len = keep_bytes + suffix_bytes;
    char *buf = (char *)mem_alloc(arena, out_len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, s.data, keep_bytes);
    memcpy(buf + keep_bytes, suffix.data, suffix_bytes);
    buf[out_len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, out_len));
}

static exprtk_value_t fn_left(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t count = mod_number_to_index(args[1]);
    size_t end = mod_string_byte_offset(s, count);
    return mod_copy_string(arena, s.data, end);
}

static exprtk_value_t fn_right(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t count = mod_number_to_index(args[1]);
    size_t units = mod_string_unit_count(s);
    size_t start_units = count >= units ? 0 : units - count;
    size_t start = mod_string_byte_offset(s, start_units);
    return mod_copy_string(arena, s.data + start, s.len - start);
}

static exprtk_value_t fn_drop(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t start = mod_string_byte_offset(s, mod_number_to_index(args[1]));
    return mod_copy_string(arena, s.data + start, s.len - start);
}

static exprtk_value_t fn_zfill(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t width = mod_number_to_index(args[1]);
    size_t current = mod_string_unit_count(s);
    if (width <= current) return mod_copy_string(arena, s.data, s.len);
    size_t zeros = width - current;
    size_t sign = (s.len > 0 && (s.data[0] == '+' || s.data[0] == '-')) ? 1 : 0;
    char *buf = (char *)mem_alloc(arena, s.len + zeros + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    if (sign) buf[pos++] = s.data[0];
    for (size_t i = 0; i < zeros; ++i) buf[pos++] = '0';
    if (s.len > sign) {
        memcpy(buf + pos, s.data + sign, s.len - sign);
        pos += s.len - sign;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_pad_left(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v fill = (argc == 3 && args[2].type == EXPRTK_VAL_STRING) ? args[2].data.string : tstr_v_from_cstr(" ");
    size_t fill_len = mod_first_utf8_char_len(fill);
    if (fill_len == 0) fill = tstr_v_from_cstr(" "), fill_len = 1;
    size_t width = mod_number_to_index(args[1]);
    size_t current = mod_utf8_valid_view(s) ? utf8nlen((const utf8_int8_t *)s.data, s.len) : s.len;
    if (width <= current) return mod_copy_string(arena, s.data, s.len);
    size_t pad_count = width - current;
    size_t len = s.len + pad_count * fill_len;
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < pad_count; ++i) {
        memcpy(buf + pos, fill.data, fill_len);
        pos += fill_len;
    }
    memcpy(buf + pos, s.data, s.len);
    pos += s.len;
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_pad_right(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v fill = (argc == 3 && args[2].type == EXPRTK_VAL_STRING) ? args[2].data.string : tstr_v_from_cstr(" ");
    size_t fill_len = mod_first_utf8_char_len(fill);
    if (fill_len == 0) fill = tstr_v_from_cstr(" "), fill_len = 1;
    size_t width = mod_number_to_index(args[1]);
    size_t current = mod_utf8_valid_view(s) ? utf8nlen((const utf8_int8_t *)s.data, s.len) : s.len;
    if (width <= current) return mod_copy_string(arena, s.data, s.len);
    size_t pad_count = width - current;
    size_t len = s.len + pad_count * fill_len;
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, s.data, s.len);
    size_t pos = s.len;
    for (size_t i = 0; i < pad_count; ++i) {
        memcpy(buf + pos, fill.data, fill_len);
        pos += fill_len;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_starts_with_ci(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v prefix = args[1].data.string;
    if (prefix.len > s.len || !mod_utf8_valid_view(s) || !mod_utf8_valid_view(prefix))
        return exprtk_val_num(0);
    if (!mod_utf8_is_boundary(s, prefix.len))
        return exprtk_val_num(0);
    char *lhs = mod_view_to_arena_cstr(tstr_v_from_buf(s.data, prefix.len), arena);
    char *rhs = mod_view_to_arena_cstr(prefix, arena);
    if (!lhs || !rhs) return exprtk_val_num(0);
    return exprtk_val_num(utf8casecmp((const utf8_int8_t *)lhs, (const utf8_int8_t *)rhs) == 0 ? 1.0 : 0.0);
}

static exprtk_value_t fn_ends_with_ci(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v suffix = args[1].data.string;
    if (suffix.len > s.len || !mod_utf8_valid_view(s) || !mod_utf8_valid_view(suffix))
        return exprtk_val_num(0);
    size_t start = s.len - suffix.len;
    if (!mod_utf8_is_boundary(s, start))
        return exprtk_val_num(0);
    char *lhs = mod_view_to_arena_cstr(tstr_v_from_buf(s.data + start, suffix.len), arena);
    char *rhs = mod_view_to_arena_cstr(suffix, arena);
    if (!lhs || !rhs) return exprtk_val_num(0);
    return exprtk_val_num(utf8casecmp((const utf8_int8_t *)lhs, (const utf8_int8_t *)rhs) == 0 ? 1.0 : 0.0);
}

static exprtk_value_t mod_partition_result(mem_pool_t *arena, tstr_v s, tstr_v sep, size_t pos, int found) {
    exprtk_value_t list = exprtk_val_list_empty();
    if (!found) {
        if (!mod_list_append_string(&list, arena, s.data, s.len)) return exprtk_val_num(0);
        if (!mod_list_append_string(&list, arena, "", 0)) return exprtk_val_num(0);
        if (!mod_list_append_string(&list, arena, "", 0)) return exprtk_val_num(0);
        return list;
    }
    if (!mod_list_append_string(&list, arena, s.data, pos)) return exprtk_val_num(0);
    if (!mod_list_append_string(&list, arena, sep.data, sep.len)) return exprtk_val_num(0);
    if (!mod_list_append_string(&list, arena, s.data + pos + sep.len, s.len - pos - sep.len)) return exprtk_val_num(0);
    return list;
}

static exprtk_value_t fn_str_partition(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    if (sep.len == 0) return exprtk_val_num(0);
    size_t pos = tstr_v_find(s, sep);
    return mod_partition_result(arena, s, sep, pos, pos != TSTR_V_NPOS);
}

static exprtk_value_t fn_str_rpartition(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v sep = args[1].data.string;
    if (sep.len == 0) return exprtk_val_num(0);
    size_t pos = tstr_v_rfind(s, sep);
    return mod_partition_result(arena, s, sep, pos, pos != TSTR_V_NPOS);
}

static exprtk_value_t fn_str_words(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    size_t i = 0;
    while (i < s.len) {
        while (i < s.len && isspace((unsigned char)s.data[i])) i++;
        size_t start = i;
        while (i < s.len && !isspace((unsigned char)s.data[i])) i++;
        if (i > start && !mod_list_append_string(&list, arena, s.data + start, i - start))
            return exprtk_val_num(0);
    }
    return list;
}

static exprtk_value_t fn_csv_split_line(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    exprtk_value_t list = exprtk_val_list_empty();
    char *cell = (char *)mem_alloc(arena, s.len + 1);
    if (!cell) return exprtk_val_num(0);
    size_t cell_len = 0;
    int quoted = 0;
    for (size_t i = 0; i <= s.len; ++i) {
        char c = (i < s.len) ? s.data[i] : ',';
        if (quoted) {
            if (c == '"' && i + 1 < s.len && s.data[i + 1] == '"') {
                cell[cell_len++] = '"';
                i++;
            } else if (c == '"') {
                quoted = 0;
            } else {
                cell[cell_len++] = c;
            }
        } else if (c == '"' && cell_len == 0) {
            quoted = 1;
        } else if (c == ',' || i == s.len) {
            if (!mod_list_append_string(&list, arena, cell, cell_len)) return exprtk_val_num(0);
            cell_len = 0;
        } else {
            cell[cell_len++] = c;
        }
    }
    return list;
}

static size_t mod_csv_cell_encoded_len(tstr_v s) {
    int needs_quote = 0;
    size_t quote_count = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == '"') quote_count++;
        if (s.data[i] == '"' || s.data[i] == ',' || s.data[i] == '\n' || s.data[i] == '\r') needs_quote = 1;
    }
    return needs_quote ? s.len + quote_count + 2 : s.len;
}

static char *mod_csv_write_cell(char *out, tstr_v s) {
    int needs_quote = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == '"' || s.data[i] == ',' || s.data[i] == '\n' || s.data[i] == '\r') {
            needs_quote = 1;
            break;
        }
    }
    if (!needs_quote) {
        if (s.len > 0) memcpy(out, s.data, s.len);
        return out + s.len;
    }
    *out++ = '"';
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == '"') *out++ = '"';
        *out++ = s.data[i];
    }
    *out++ = '"';
    return out;
}

static exprtk_value_t fn_csv_join_line(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_LIST)
        return exprtk_val_num(0);
    exprtk_value_t list = args[0];
    size_t cap = list.data.list.count > 0 ? list.data.list.count - 1 : 0;
    char tmp[64];
    for (size_t i = 0; i < list.data.list.count; ++i) {
        exprtk_value_t item = list.data.list.items[i];
        if (item.type == EXPRTK_VAL_STRING) {
            cap += mod_csv_cell_encoded_len(item.data.string);
        } else if (mod_is_number(item)) {
            int n = snprintf(tmp, sizeof(tmp), "%g", mod_number_value(item));
            if (n > 0) cap += (size_t)n;
        }
    }
    char *buf = (char *)mem_alloc(arena, cap + 1);
    if (!buf) return exprtk_val_num(0);
    char *out = buf;
    for (size_t i = 0; i < list.data.list.count; ++i) {
        if (i > 0) *out++ = ',';
        exprtk_value_t item = list.data.list.items[i];
        if (item.type == EXPRTK_VAL_STRING) {
            out = mod_csv_write_cell(out, item.data.string);
        } else if (mod_is_number(item)) {
            int n = snprintf(out, (buf + cap + 1) - out, "%g", mod_number_value(item));
            if (n > 0) out += n;
        }
    }
    *out = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, (size_t)(out - buf)));
}

static exprtk_value_t fn_assert(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)arena;
    if (argc == 1 || argc == 2) {
        double cond = (args[0].type == EXPRTK_VAL_NUMBER) ? args[0].data.number : (args[0].data.string.len > 0);
        if (fabs(cond) <= 1e-9) {
            if (env) {
                env->aborted = 1;
                if (argc == 2 && args[1].type == EXPRTK_VAL_STRING)
                    printf("Assertion failed: %.*s\n", (int)args[1].data.string.len, args[1].data.string.data);
                else
                    printf("Assertion failed\n");
            }
        }
        return args[0];
    }
    return exprtk_val_num(0);
}

/* ========================================================================= */
/* Tokenization & conversion functions                                        */
/* ========================================================================= */

/* tokenize(str, delim, index) → string: extract token at index */
static exprtk_value_t fn_tokenize(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != EXPRTK_VAL_STRING
                   || args[1].type != EXPRTK_VAL_STRING
                   || args[2].type != EXPRTK_VAL_NUMBER)
        return exprtk_val_num(0);

    size_t index = (size_t)args[2].data.number;
    size_t cursor = 0;
    size_t current = 0;
    tstr_v token;

    while (mod_next_token(args[0].data.string, args[1].data.string, &cursor, true, &token)) {
        if (current == index) {
            char *buf = mem_alloc(arena, token.len + 1);
            if (!buf) return exprtk_val_num(0);
            if (token.len > 0) memcpy(buf, token.data, token.len);
            buf[token.len] = '\0';
            return exprtk_val_str(tstr_v_from_buf(buf, token.len));
        }
        current++;
    }
    return exprtk_val_num(0);
}

/* split(str, delim) → vector: split string into numeric vector */
static exprtk_value_t fn_split(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING
                   || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    size_t count = mod_count_tokens(args[0].data.string, args[1].data.string, true);
    if (count > 0) {
        double *vec = (double *)mem_alloc(arena, count * sizeof(double));
        if (vec) {
            size_t cursor = 0;
            size_t i = 0;
            tstr_v token;
            while (i < count &&
                   mod_next_token(args[0].data.string, args[1].data.string, &cursor, true, &token)) {
                vec[i] = 0.0;
                mod_to_double_v(token, arena, &vec[i]);
                i++;
            }
            return exprtk_val_vec(vec, count);
        }
    }
    return exprtk_val_num(0);
}

/* token_count(str, delim) → number: count tokens */
static exprtk_value_t fn_token_count(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING
                   || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    size_t count = mod_count_tokens(args[0].data.string, args[1].data.string, true);
    return exprtk_val_num((double)count);
}

/* to_num(str) → number: parse string as double */
static exprtk_value_t fn_to_num(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    double val = 0.0;
    mod_to_double_v(args[0].data.string, arena, &val);
    return exprtk_val_num(val);
}

/* to_str(num) → string: format number as string */
static exprtk_value_t fn_to_str(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_NUMBER)
        return exprtk_val_num(0);
    char tmp[64];
    int len = snprintf(tmp, sizeof(tmp), "%g", args[0].data.number);
    char *buf = mem_alloc(arena, len + 1);
    if (buf) {
        memcpy(buf, tmp, len);
        buf[len] = '\0';
        return exprtk_val_str(tstr_v_from_buf(buf, len));
    }
    return exprtk_val_num(0);
}

/* to_int(str) → number: parse string as integer */
static exprtk_value_t fn_to_int(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    long long val = 0;
    mod_to_int_v(args[0].data.string, arena, &val);
    return exprtk_val_num((double)val);
}

/* to_double(str) → number: alias for to_num */
static exprtk_value_t fn_to_double(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    return fn_to_num(argc, args, env, arena);
}

/* to_bool(str) → number: parse "true"/"false"/"yes"/"no"/"1"/"0" */
static exprtk_value_t fn_to_bool(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    bool val = false;
    mod_to_bool_v(args[0].data.string, &val);
    return exprtk_val_num(val ? 1.0 : 0.0);
}

/* format(fmt, ...) → string: sprintf-style formatting (%d %f %g %s %%) */
static exprtk_value_t fn_format(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v fmt = args[0].data.string;
    size_t cap = fmt.len + argc * 32;
    for (size_t i = 1; i < argc; ++i) {
        if (args[i].type == EXPRTK_VAL_STRING) cap += args[i].data.string.len;
    }
    char *buf = mem_alloc(arena, cap + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0, arg_idx = 1;
    for (size_t i = 0; i < fmt.len; ++i) {
        if (fmt.data[i] == '%' && i + 1 < fmt.len) {
            char spec = fmt.data[i + 1];
            if (spec == '%') {
                buf[pos++] = '%';
                ++i;
                continue;
            }
            if (arg_idx < argc) {
                int written = 0;
                switch (spec) {
                    case 'd':
                        written = snprintf(buf + pos, cap - pos, "%d", (int)args[arg_idx].data.number);
                        break;
                    case 'f':
                        written = snprintf(buf + pos, cap - pos, "%f", args[arg_idx].data.number);
                        break;
                    case 'g':
                        written = snprintf(buf + pos, cap - pos, "%g", args[arg_idx].data.number);
                        break;
                    case 's':
                        if (args[arg_idx].type == EXPRTK_VAL_STRING) {
                            tstr_v s = args[arg_idx].data.string;
                            written = snprintf(buf + pos, cap - pos, "%.*s", (int)s.len, s.data);
                        }
                        break;
                    default:
                        buf[pos++] = '%';
                        buf[pos++] = spec;
                        ++i;
                        continue;
                }
                if (written > 0) pos += (size_t)written;
                ++arg_idx;
                ++i;
                continue;
            }
        }
        buf[pos++] = fmt.data[i];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static size_t mod_value_string_len(exprtk_value_t value) {
    char tmp[64];
    if (value.type == EXPRTK_VAL_STRING) return value.data.string.len;
    if (value.type == EXPRTK_VAL_INTEGER) {
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)value.data.integer);
        return n > 0 ? (size_t)n : 0;
    }
    if (value.type == EXPRTK_VAL_NUMBER) {
        int n = snprintf(tmp, sizeof(tmp), "%g", value.data.number);
        return n > 0 ? (size_t)n : 0;
    }
    if (value.type == EXPRTK_VAL_NULL) return 4;
    return 0;
}

static size_t mod_write_value_string(char *buf, size_t cap, exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_STRING) {
        size_t n = value.data.string.len < cap ? value.data.string.len : cap;
        if (n > 0) memcpy(buf, value.data.string.data, n);
        return n;
    }
    if (value.type == EXPRTK_VAL_INTEGER) {
        int n = snprintf(buf, cap, "%lld", (long long)value.data.integer);
        return n > 0 ? (size_t)n : 0;
    }
    if (value.type == EXPRTK_VAL_NUMBER) {
        int n = snprintf(buf, cap, "%g", value.data.number);
        return n > 0 ? (size_t)n : 0;
    }
    if (value.type == EXPRTK_VAL_NULL && cap >= 4) {
        memcpy(buf, "null", 4);
        return 4;
    }
    return 0;
}

typedef struct {
    char *name;
    size_t name_len;
    MUSTACHE_TEMPLATE *templ;
} mod_mustache_partial_entry_t;

typedef struct {
    exprtk_value_t *root;
    exprtk_value_t *partials;
    exprtk_env_t *env;
    mem_pool_t *arena;
    mod_mustache_partial_entry_t *partial_cache;
    size_t partial_count;
    size_t partial_capacity;
} mod_mustache_provider_t;

static int mod_mustache_is_truthy(const exprtk_value_t *value) {
    if (!value) return 0;
    switch (value->type) {
        case EXPRTK_VAL_NULL:
            return 0;
        case EXPRTK_VAL_NUMBER:
            return fabs(value->data.number) > 1e-12;
        case EXPRTK_VAL_INTEGER:
            return value->data.integer != 0;
        case EXPRTK_VAL_STRING:
            return value->data.string.len > 0;
        case EXPRTK_VAL_LIST:
            return value->data.list.count > 0;
        case EXPRTK_VAL_VECTOR:
            return value->data.vector.size > 0;
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT:
            return exprtk_map_count(value) > 0;
        default:
            return 1;
    }
}

static void *mod_mustache_get_root(void *provider_data) {
    mod_mustache_provider_t *provider = (mod_mustache_provider_t *)provider_data;
    return provider ? provider->root : NULL;
}

static int mod_mustache_dump(void *node, int (*out_fn)(const char *, size_t, void *),
                             void *renderer_data, void *provider_data) {
    (void)provider_data;
    exprtk_value_t *value = (exprtk_value_t *)node;
    if (!value || !out_fn) return 0;

    char buf[64];
    switch (value->type) {
        case EXPRTK_VAL_STRING:
            return out_fn(value->data.string.data, value->data.string.len, renderer_data);
        case EXPRTK_VAL_INTEGER: {
            int n = snprintf(buf, sizeof(buf), "%lld", (long long)value->data.integer);
            return n > 0 ? out_fn(buf, (size_t)n, renderer_data) : 0;
        }
        case EXPRTK_VAL_NUMBER: {
            int n = snprintf(buf, sizeof(buf), "%g", value->data.number);
            return n > 0 ? out_fn(buf, (size_t)n, renderer_data) : 0;
        }
        case EXPRTK_VAL_NULL:
            return 0;
        default:
            return 0;
    }
}

static exprtk_value_t *mod_mustache_name_lookup(exprtk_value_t *value,
                                                const char *name, size_t size,
                                                mod_mustache_provider_t *provider);

static exprtk_value_t *mod_mustache_lookup_part(exprtk_value_t *value,
                                                const char *name, size_t size,
                                                mod_mustache_provider_t *provider) {
    if (!value || !name) return NULL;
    if (size == 1 && name[0] == '.') return value;
    if (!exprtk_value_is_object_like(value)) return NULL;

    char stack_key[128];
    char *key = stack_key;
    if (size + 1 > sizeof(stack_key)) {
        if (!provider || !provider->arena) return NULL;
        key = (char *)mem_alloc(provider->arena, size + 1);
        if (!key) return NULL;
    }
    memcpy(key, name, size);
    key[size] = '\0';
    return exprtk_map_get_ptr(value, key);
}

static exprtk_value_t *mod_mustache_name_lookup(exprtk_value_t *value,
                                                const char *name, size_t size,
                                                mod_mustache_provider_t *provider) {
    if (!value || !name || size == 0) return NULL;
    const char *dot = (const char *)memchr(name, '.', size);
    if (dot && !(size == 1 && name[0] == '.')) {
        size_t head_size = (size_t)(dot - name);
        exprtk_value_t *child = mod_mustache_lookup_part(value, name, head_size, provider);
        if (!child) return NULL;
        return mod_mustache_name_lookup(child, dot + 1, size - head_size - 1, provider);
    }
    return mod_mustache_lookup_part(value, name, size, provider);
}

static void *mod_mustache_get_child_by_name(void *node, const char *name, size_t size,
                                            void *provider_data) {
    mod_mustache_provider_t *provider = (mod_mustache_provider_t *)provider_data;
    return mod_mustache_name_lookup((exprtk_value_t *)node, name, size, provider);
}

static void *mod_mustache_get_child_by_index(void *node, unsigned index,
                                             void *provider_data) {
    exprtk_value_t *value = (exprtk_value_t *)node;
    mod_mustache_provider_t *provider = (mod_mustache_provider_t *)provider_data;
    if (!value || !mod_mustache_is_truthy(value)) return NULL;

    if (value->type == EXPRTK_VAL_LIST) {
        return index < value->data.list.count ? &value->data.list.items[index] : NULL;
    }
    if (value->type == EXPRTK_VAL_VECTOR) {
        if (index >= value->data.vector.size || !provider || !provider->arena) return NULL;
        exprtk_value_t *item = (exprtk_value_t *)mem_alloc(provider->arena, sizeof(exprtk_value_t));
        if (!item) return NULL;
        *item = exprtk_val_num(value->data.vector.data[index]);
        return item;
    }
    return index == 0 ? value : NULL;
}

static MUSTACHE_TEMPLATE *mod_mustache_get_partial(const char *name, size_t size,
                                                   void *provider_data) {
    mod_mustache_provider_t *provider = (mod_mustache_provider_t *)provider_data;
    if (!provider || !provider->partials || !exprtk_value_is_object_like(provider->partials) ||
        !name || size == 0) {
        return NULL;
    }

    for (size_t i = 0; i < provider->partial_count; ++i) {
        mod_mustache_partial_entry_t *entry = &provider->partial_cache[i];
        if (entry->name_len == size && strncmp(entry->name, name, size) == 0) {
            return entry->templ;
        }
    }

    exprtk_value_t *partial_value =
        mod_mustache_name_lookup(provider->partials, name, size, provider);
    if (!partial_value || partial_value->type != EXPRTK_VAL_STRING) return NULL;

    MUSTACHE_TEMPLATE *templ =
        mustache_compile_v(partial_value->data.string, NULL, NULL, 0);
    if (!templ) return NULL;

    if (provider->partial_count >= provider->partial_capacity) {
        size_t new_capacity = provider->partial_capacity ? provider->partial_capacity * 2 : 4;
        mod_mustache_partial_entry_t *grown =
            (mod_mustache_partial_entry_t *)realloc(
                provider->partial_cache, new_capacity * sizeof(mod_mustache_partial_entry_t));
        if (!grown) {
            mustache_release(templ);
            return NULL;
        }
        provider->partial_cache = grown;
        provider->partial_capacity = new_capacity;
    }

    char *name_copy = (char *)malloc(size + 1);
    if (!name_copy) {
        mustache_release(templ);
        return NULL;
    }
    memcpy(name_copy, name, size);
    name_copy[size] = '\0';

    mod_mustache_partial_entry_t *entry =
        &provider->partial_cache[provider->partial_count++];
    entry->name = name_copy;
    entry->name_len = size;
    entry->templ = templ;
    return templ;
}

static void mod_mustache_provider_free(mod_mustache_provider_t *provider) {
    if (!provider) return;
    for (size_t i = 0; i < provider->partial_count; ++i) {
        free(provider->partial_cache[i].name);
        if (provider->partial_cache[i].templ) {
            mustache_release(provider->partial_cache[i].templ);
        }
    }
    free(provider->partial_cache);
    provider->partial_cache = NULL;
    provider->partial_count = 0;
    provider->partial_capacity = 0;
}

static int mod_mustache_is_lambda(void *node, void *provider_data) {
    (void)provider_data;
    exprtk_value_t *value = (exprtk_value_t *)node;
    return value && value->type == EXPRTK_VAL_FUNCTION;
}

static int mod_mustache_call_lambda(void *node, const char *text, size_t text_len,
                                    char **out_text, size_t *out_len,
                                    void *provider_data) {
    exprtk_value_t *value = (exprtk_value_t *)node;
    mod_mustache_provider_t *provider = (mod_mustache_provider_t *)provider_data;
    if (!value || value->type != EXPRTK_VAL_FUNCTION || !provider ||
        !provider->env || !out_text || !out_len) {
        return -1;
    }

    exprtk_value_t arg = exprtk_val_str(tstr_v_from_buf((char *)(text ? text : ""), text_len));
    exprtk_value_t result = exprtk_call_function_value(*value, 1, &arg, provider->env);
    if (provider->env->aborted || provider->env->flow == exprtk_FLOW_THROW) return -1;

    size_t len = mod_value_string_len(result);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return -1;
    size_t written = mod_write_value_string(buf, len + 1, result);
    buf[written] = '\0';
    *out_text = buf;
    *out_len = written;
    return 0;
}

static exprtk_value_t fn_template_render(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (argc == 3 && !exprtk_value_is_object_like(&args[2]))
        return exprtk_val_num(0);

    MUSTACHE_TEMPLATE *templ =
        mustache_compile_v(args[0].data.string, NULL, NULL, 0);
    if (!templ) return exprtk_val_num(0);

    MUSTACHE_STRING_RENDERER_ARENA renderer;
    if (mustache_string_renderer_init_arena(&renderer, arena, args[0].data.string.len + 64) != 0) {
        mustache_release(templ);
        return exprtk_val_num(0);
    }

    mod_mustache_provider_t provider_data;
    memset(&provider_data, 0, sizeof(provider_data));
    provider_data.root = &args[1];
    provider_data.partials = argc == 3 ? &args[2] : NULL;
    provider_data.env = env;
    provider_data.arena = arena;
    MUSTACHE_DATAPROVIDER provider = {
        .dump = mod_mustache_dump,
        .get_root = mod_mustache_get_root,
        .get_child_by_name = mod_mustache_get_child_by_name,
        .get_child_by_index = mod_mustache_get_child_by_index,
        .get_partial = mod_mustache_get_partial,
        .is_lambda = mod_mustache_is_lambda,
        .call_lambda = mod_mustache_call_lambda,
    };

    int rc = mustache_process(templ, &renderer.base, &renderer, &provider, &provider_data);
    char *rendered = rc == 0 ? mustache_string_renderer_get_arena(&renderer) : NULL;
    exprtk_value_t result = exprtk_val_num(0);
    if (rendered) {
        result = mod_copy_string(arena, rendered, strlen(rendered));
    }

    mod_mustache_provider_free(&provider_data);
    mustache_string_renderer_free_arena(&renderer);
    mustache_release(templ);
    return result;
}

static exprtk_value_t fn_str_format(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc < 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v fmt = args[0].data.string;
    size_t cap = fmt.len + 1;
    for (size_t i = 1; i < argc; ++i) cap += mod_value_string_len(args[i]) + 16;
    char *buf = (char *)mem_alloc(arena, cap);
    if (!buf) return exprtk_val_num(0);

    size_t pos = 0;
    size_t next_arg = 1;
    for (size_t i = 0; i < fmt.len; ++i) {
        if (fmt.data[i] == '{' && i + 1 < fmt.len && fmt.data[i + 1] == '{') {
            buf[pos++] = '{';
            i++;
            continue;
        }
        if (fmt.data[i] == '}' && i + 1 < fmt.len && fmt.data[i + 1] == '}') {
            buf[pos++] = '}';
            i++;
            continue;
        }
        if (fmt.data[i] == '{') {
            size_t j = i + 1;
            int explicit_index = 0;
            size_t index = 0;
            while (j < fmt.len && isdigit((unsigned char)fmt.data[j])) {
                explicit_index = 1;
                index = index * 10 + (size_t)(fmt.data[j] - '0');
                j++;
            }
            if (j < fmt.len && fmt.data[j] == '}') {
                size_t arg_index = explicit_index ? index + 1 : next_arg++;
                if (arg_index < argc) {
                    pos += mod_write_value_string(buf + pos, cap - pos, args[arg_index]);
                }
                i = j;
                continue;
            }
        }
        buf[pos++] = fmt.data[i];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_unicode_normalize(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 1 && argc != 2) || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    if (argc == 2 && args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
    if (argc == 2) {
        tstr_v form = args[1].data.string;
        if (!(tstr_v_ieq(form, tstr_v_from_cstr("NFC")) ||
              tstr_v_ieq(form, tstr_v_from_cstr("NFD")) ||
              tstr_v_ieq(form, tstr_v_from_cstr("NFKC")) ||
              tstr_v_ieq(form, tstr_v_from_cstr("NFKD")))) {
            return exprtk_val_num(0);
        }
    }
    return mod_copy_string(arena, s.data, s.len);
}

static exprtk_value_t fn_grapheme_len(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    return fn_utf8_len(argc, args, env, arena);
}

static exprtk_value_t fn_grapheme_substr(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    return fn_utf8_substr(argc, args, env, arena);
}

static exprtk_value_t fn_grapheme_reverse(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    return fn_utf8_reverse(argc, args, env, arena);
}

static exprtk_value_t fn_grapheme_split(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (!mod_utf8_valid_view(s)) return exprtk_val_num(0);
    exprtk_value_t list = exprtk_val_list_empty();
    size_t offset = 0;
    while (offset < s.len) {
        size_t n = utf8codepointcalcsize((const utf8_int8_t *)(s.data + offset));
        if (n == 0 || offset + n > s.len) n = 1;
        if (!mod_list_append_string(&list, arena, s.data + offset, n)) return exprtk_val_num(0);
        offset += n;
    }
    return list;
}

static exprtk_value_t fn_casefold(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    return fn_utf8_lower(argc, args, env, arena);
}

static exprtk_value_t fn_capitalize(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        buf[i] = (char)(i == 0 ? toupper(c) : tolower(c));
    }
    buf[s.len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, s.len));
}

static exprtk_value_t fn_title(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    int word_start = 1;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (isalnum(c)) {
            buf[i] = (char)(word_start ? toupper(c) : tolower(c));
            word_start = 0;
        } else {
            buf[i] = s.data[i];
            word_start = 1;
        }
    }
    buf[s.len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, s.len));
}

static exprtk_value_t fn_swapcase(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (islower(c)) buf[i] = (char)toupper(c);
        else if (isupper(c)) buf[i] = (char)tolower(c);
        else buf[i] = s.data[i];
    }
    buf[s.len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, s.len));
}

static exprtk_value_t fn_word_wrap(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]) ||
        (argc == 3 && !mod_is_number(args[2])))
        return exprtk_val_num(0);

    tstr_v s = args[0].data.string;
    size_t width = mod_number_to_index(args[1]);
    int break_long = argc == 3 ? fabs(mod_number_value(args[2])) > 1e-9 : 1;
    if (width == 0 || s.len == 0) return mod_copy_string(arena, s.data, s.len);

    char *buf = (char *)mem_alloc(arena, s.len * 2 + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    size_t line_len = 0;
    size_t i = 0;
    while (i < s.len) {
        int saw_newline = 0;
        while (i < s.len && isspace((unsigned char)s.data[i])) {
            if (s.data[i] == '\n' || s.data[i] == '\r') saw_newline = 1;
            i++;
        }
        if (saw_newline && line_len > 0) {
            buf[pos++] = '\n';
            line_len = 0;
        }
        if (i >= s.len) break;

        size_t start = i;
        while (i < s.len && !isspace((unsigned char)s.data[i])) i++;
        size_t word_len = i - start;

        if (line_len > 0) {
            if (line_len + 1 + word_len <= width) {
                buf[pos++] = ' ';
                line_len++;
            } else {
                buf[pos++] = '\n';
                line_len = 0;
            }
        }

        if (word_len <= width || !break_long) {
            memcpy(buf + pos, s.data + start, word_len);
            pos += word_len;
            line_len += word_len;
            continue;
        }

        size_t off = 0;
        while (off < word_len) {
            size_t room = width - line_len;
            size_t n = word_len - off < room ? word_len - off : room;
            memcpy(buf + pos, s.data + start + off, n);
            pos += n;
            line_len += n;
            off += n;
            if (off < word_len) {
                buf[pos++] = '\n';
                line_len = 0;
            }
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_shorten(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if ((argc != 2 && argc != 3) || args[0].type != EXPRTK_VAL_STRING || !mod_is_number(args[1]) ||
        (argc == 3 && args[2].type != EXPRTK_VAL_STRING))
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v suffix = argc == 3 ? args[2].data.string : tstr_v_from_cstr("...");
    size_t width = mod_number_to_index(args[1]);
    if (s.len <= width) return mod_copy_string(arena, s.data, s.len);
    if (width == 0) return mod_copy_string(arena, "", 0);
    if (suffix.len >= width) return mod_copy_string(arena, suffix.data, width);

    size_t limit = width - suffix.len;
    size_t cut = limit;
    size_t last_space = TSTR_V_NPOS;
    for (size_t i = 0; i < limit && i < s.len; ++i) {
        if (isspace((unsigned char)s.data[i])) last_space = i;
    }
    if (last_space != TSTR_V_NPOS && last_space > 0) cut = last_space;
    while (cut > 0 && isspace((unsigned char)s.data[cut - 1])) cut--;

    char *buf = (char *)mem_alloc(arena, cut + suffix.len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, s.data, cut);
    memcpy(buf + cut, suffix.data, suffix.len);
    buf[cut + suffix.len] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, cut + suffix.len));
}

static exprtk_value_t mod_case_convert(tstr_v s, mem_pool_t *arena, int style) {
    char sep = style == 0 ? '_' : '-';
    char *buf = (char *)mem_alloc(arena, s.len * 2 + 1);
    if (!buf) return exprtk_val_num(0);

    size_t pos = 0;
    size_t word_index = 0;
    int in_word = 0;
    int cap_next = 0;
    unsigned char prev = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (!isalnum(c)) {
            in_word = 0;
            prev = 0;
            continue;
        }

        int boundary = in_word && isupper(c) && (islower(prev) || isdigit(prev));
        if (!in_word || boundary) {
            if ((style == 0 || style == 1 || style == 4) && word_index > 0 && pos > 0)
                buf[pos++] = sep;
            cap_next = (style == 3) || (style == 2 && word_index > 0);
            word_index++;
            in_word = 1;
        }

        char out = (char)tolower(c);
        if (cap_next && isalpha(c)) {
            out = (char)toupper(c);
            cap_next = 0;
        } else if (!isalpha(c)) {
            cap_next = 0;
        }
        buf[pos++] = out;
        prev = c;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_snake_case(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    return mod_case_convert(args[0].data.string, arena, 0);
}

static exprtk_value_t fn_kebab_case(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    return mod_case_convert(args[0].data.string, arena, 1);
}

static exprtk_value_t fn_camel_case(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    return mod_case_convert(args[0].data.string, arena, 2);
}

static exprtk_value_t fn_pascal_case(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    return mod_case_convert(args[0].data.string, arena, 3);
}

static exprtk_value_t fn_slugify(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    return mod_case_convert(args[0].data.string, arena, 4);
}

static exprtk_value_t fn_remove_chars(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v chars = args[1].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    size_t i = 0;
    int valid = mod_utf8_valid_view(s) && mod_utf8_valid_view(chars);
    while (i < s.len) {
        size_t n = valid ? utf8codepointcalcsize((const utf8_int8_t *)(s.data + i)) : 1;
        if (n == 0 || i + n > s.len) n = 1;
        if (!mod_view_contains_span(chars, s.data + i, n)) {
            memcpy(buf + pos, s.data + i, n);
            pos += n;
        }
        i += n;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_keep_chars(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v chars = args[1].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    size_t i = 0;
    int valid = mod_utf8_valid_view(s) && mod_utf8_valid_view(chars);
    while (i < s.len) {
        size_t n = valid ? utf8codepointcalcsize((const utf8_int8_t *)(s.data + i)) : 1;
        if (n == 0 || i + n > s.len) n = 1;
        if (mod_view_contains_span(chars, s.data + i, n)) {
            memcpy(buf + pos, s.data + i, n);
            pos += n;
        }
        i += n;
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_translate(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 3 || args[0].type != EXPRTK_VAL_STRING ||
        args[1].type != EXPRTK_VAL_STRING || args[2].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    tstr_v from = args[1].data.string;
    tstr_v to = args[2].data.string;
    char *buf = (char *)mem_alloc(arena, s.len * (to.len ? to.len : 1) + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        char *hit = (char *)memchr(from.data, (unsigned char)s.data[i], from.len);
        if (!hit) {
            buf[pos++] = s.data[i];
            continue;
        }
        size_t idx = (size_t)(hit - from.data);
        if (idx < to.len) buf[pos++] = to.data[idx];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_html_escape(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    size_t len = 0;
    for (size_t i = 0; i < s.len; ++i) {
        switch (s.data[i]) {
            case '&': len += 5; break;
            case '<':
            case '>': len += 4; break;
            case '"': len += 6; break;
            case '\'': len += 5; break;
            default: len += 1; break;
        }
    }
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        switch (s.data[i]) {
            case '&': memcpy(buf + pos, "&amp;", 5); pos += 5; break;
            case '<': memcpy(buf + pos, "&lt;", 4); pos += 4; break;
            case '>': memcpy(buf + pos, "&gt;", 4); pos += 4; break;
            case '"': memcpy(buf + pos, "&quot;", 6); pos += 6; break;
            case '\'': memcpy(buf + pos, "&#39;", 5); pos += 5; break;
            default: buf[pos++] = s.data[i]; break;
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_html_unescape(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        size_t rem = s.len - i;
        if (rem >= 5 && memcmp(s.data + i, "&amp;", 5) == 0) {
            buf[pos++] = '&'; i += 4;
        } else if (rem >= 4 && memcmp(s.data + i, "&lt;", 4) == 0) {
            buf[pos++] = '<'; i += 3;
        } else if (rem >= 4 && memcmp(s.data + i, "&gt;", 4) == 0) {
            buf[pos++] = '>'; i += 3;
        } else if (rem >= 6 && memcmp(s.data + i, "&quot;", 6) == 0) {
            buf[pos++] = '"'; i += 5;
        } else if (rem >= 6 && memcmp(s.data + i, "&apos;", 6) == 0) {
            buf[pos++] = '\''; i += 5;
        } else if (rem >= 5 && memcmp(s.data + i, "&#39;", 5) == 0) {
            buf[pos++] = '\''; i += 4;
        } else {
            buf[pos++] = s.data[i];
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_json_escape(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    static const char hex[] = "0123456789ABCDEF";
    tstr_v s = args[0].data.string;
    size_t len = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                len += 2;
                break;
            default:
                len += c < 0x20 ? 6 : 1;
                break;
        }
    }
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
            case '"':  buf[pos++] = '\\'; buf[pos++] = '"'; break;
            case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
            case '\b': buf[pos++] = '\\'; buf[pos++] = 'b'; break;
            case '\f': buf[pos++] = '\\'; buf[pos++] = 'f'; break;
            case '\n': buf[pos++] = '\\'; buf[pos++] = 'n'; break;
            case '\r': buf[pos++] = '\\'; buf[pos++] = 'r'; break;
            case '\t': buf[pos++] = '\\'; buf[pos++] = 't'; break;
            default:
                if (c < 0x20) {
                    buf[pos++] = '\\';
                    buf[pos++] = 'u';
                    buf[pos++] = '0';
                    buf[pos++] = '0';
                    buf[pos++] = hex[c >> 4];
                    buf[pos++] = hex[c & 0x0F];
                } else {
                    buf[pos++] = (char)c;
                }
                break;
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_json_unescape(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] != '\\' || i + 1 >= s.len) {
            buf[pos++] = s.data[i];
            continue;
        }
        char e = s.data[++i];
        switch (e) {
            case '"':  buf[pos++] = '"'; break;
            case '\\': buf[pos++] = '\\'; break;
            case '/':  buf[pos++] = '/'; break;
            case 'b':  buf[pos++] = '\b'; break;
            case 'f':  buf[pos++] = '\f'; break;
            case 'n':  buf[pos++] = '\n'; break;
            case 'r':  buf[pos++] = '\r'; break;
            case 't':  buf[pos++] = '\t'; break;
            case 'u': {
                if (i + 4 >= s.len) {
                    buf[pos++] = '\\';
                    buf[pos++] = 'u';
                    break;
                }
                int cp = mod_hex4_value(s.data + i + 1);
                if (cp < 0) {
                    buf[pos++] = '\\';
                    buf[pos++] = 'u';
                    break;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.len &&
                    s.data[i + 1] == '\\' && s.data[i + 2] == 'u') {
                    int low = mod_hex4_value(s.data + i + 3);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                size_t n = mod_utf8_write_codepoint(buf + pos, s.len + 1 - pos, cp);
                if (n == 0) return exprtk_val_num(0);
                pos += n;
                break;
            }
            default:
                buf[pos++] = e;
                break;
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_url_encode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    static const char hex[] = "0123456789ABCDEF";
    tstr_v s = args[0].data.string;
    size_t len = 0;
    for (size_t i = 0; i < s.len; ++i) {
        len += mod_is_unreserved_url_char((unsigned char)s.data[i]) ? 1 : 3;
    }
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        if (mod_is_unreserved_url_char(c)) {
            buf[pos++] = (char)c;
        } else {
            buf[pos++] = '%';
            buf[pos++] = hex[c >> 4];
            buf[pos++] = hex[c & 0x0F];
        }
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_url_decode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        if (s.data[i] == '%' && i + 2 < s.len) {
            int hi = mod_hex_value((unsigned char)s.data[i + 1]);
            int lo = mod_hex_value((unsigned char)s.data[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[pos++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[pos++] = s.data[i] == '+' ? ' ' : s.data[i];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_base64_encode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (s.len == 0) return exprtk_val_str(tstr_v_from_buf("", 0));
    char *encoded = NULL;
    if (tn_base64_encode((const uint8_t *)s.data, s.len, &encoded) != 0 || !encoded)
        return exprtk_val_num(0);
    exprtk_value_t result = mod_copy_string(arena, encoded, strlen(encoded));
    free(encoded);
    return result;
}

static exprtk_value_t fn_base64_decode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (s.len == 0) return exprtk_val_str(tstr_v_from_buf("", 0));
    char *input = mod_view_to_arena_cstr(s, arena);
    if (!input) return exprtk_val_num(0);
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (tn_base64_decode(input, &decoded, &decoded_len) != 0 || !decoded)
        return exprtk_val_num(0);
    exprtk_value_t result = mod_copy_string(arena, (const char *)decoded, decoded_len);
    free(decoded);
    return result;
}

static exprtk_value_t fn_base64url_encode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (s.len == 0) return exprtk_val_str(tstr_v_from_buf("", 0));
    char *encoded = NULL;
    if (tn_base64_encode((const uint8_t *)s.data, s.len, &encoded) != 0 || !encoded)
        return exprtk_val_num(0);

    size_t len = strlen(encoded);
    while (len > 0 && encoded[len - 1] == '=') len--;
    char *buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) {
        free(encoded);
        return exprtk_val_num(0);
    }
    for (size_t i = 0; i < len; ++i) {
        char c = encoded[i];
        buf[i] = c == '+' ? '-' : (c == '/' ? '_' : c);
    }
    buf[len] = '\0';
    free(encoded);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static exprtk_value_t fn_base64url_decode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if (s.len == 0) return exprtk_val_str(tstr_v_from_buf("", 0));

    size_t pad = (4 - (s.len % 4)) % 4;
    if (s.len % 4 == 1) return exprtk_val_num(0);
    char *input = (char *)mem_alloc(arena, s.len + pad + 1);
    if (!input) return exprtk_val_num(0);
    for (size_t i = 0; i < s.len; ++i) {
        char c = s.data[i];
        input[i] = c == '-' ? '+' : (c == '_' ? '/' : c);
    }
    for (size_t i = 0; i < pad; ++i) input[s.len + i] = '=';
    input[s.len + pad] = '\0';

    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    if (tn_base64_decode(input, &decoded, &decoded_len) != 0 || !decoded)
        return exprtk_val_num(0);
    exprtk_value_t result = mod_copy_string(arena, (const char *)decoded, decoded_len);
    free(decoded);
    return result;
}

static exprtk_value_t fn_hex_encode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    static const char hex[] = "0123456789abcdef";
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len * 2 + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; ++i) {
        unsigned char c = (unsigned char)s.data[i];
        buf[pos++] = hex[c >> 4];
        buf[pos++] = hex[c & 0x0F];
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_hex_decode(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    if ((s.len % 2) != 0) return exprtk_val_num(0);
    char *buf = (char *)mem_alloc(arena, s.len / 2 + 1);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < s.len; i += 2) {
        int hi = mod_hex_value((unsigned char)s.data[i]);
        int lo = mod_hex_value((unsigned char)s.data[i + 1]);
        if (hi < 0 || lo < 0) return exprtk_val_num(0);
        buf[pos++] = (char)((hi << 4) | lo);
    }
    buf[pos] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static exprtk_value_t fn_constant_time_eq(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 2 || args[0].type != EXPRTK_VAL_STRING || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);

    tstr_v a = args[0].data.string;
    tstr_v b = args[1].data.string;
    size_t max_len = a.len > b.len ? a.len : b.len;
    unsigned int diff = (unsigned int)(a.len ^ b.len);
    for (size_t i = 0; i < max_len; ++i) {
        unsigned char ca = i < a.len ? (unsigned char)a.data[i] : 0;
        unsigned char cb = i < b.len ? (unsigned char)b.data[i] : 0;
        diff |= (unsigned int)(ca ^ cb);
    }
    return exprtk_val_num(diff == 0 ? 1.0 : 0.0);
}

static exprtk_value_t fn_bytes(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING) return exprtk_val_num(0);
    tstr_v s = args[0].data.string;
    char *buf = (char *)mem_alloc(arena, s.len);
    if (!buf && s.len > 0) return exprtk_val_num(0);
    if (s.len > 0) memcpy(buf, s.data, s.len);
    return exprtk_val_bytes(tstr_v_from_buf(buf, s.len));
}

static exprtk_value_t fn_from_bytes(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1 || (args[0].type != EXPRTK_VAL_BYTES &&
                      args[0].type != EXPRTK_VAL_LIST &&
                      args[0].type != EXPRTK_VAL_VECTOR))
        return exprtk_val_num(0);

    if (args[0].type == EXPRTK_VAL_BYTES) {
        tstr_v b = args[0].data.bytes;
        char *buf = (char *)mem_alloc(arena, b.len + 1);
        if (!buf) return exprtk_val_num(0);
        if (b.len > 0) memcpy(buf, b.data, b.len);
        buf[b.len] = '\0';
        return exprtk_val_str(tstr_v_from_buf(buf, b.len));
    }

    size_t n = args[0].type == EXPRTK_VAL_LIST ? args[0].data.list.count : args[0].data.vector.size;
    char *buf = (char *)mem_alloc(arena, n + 1);
    if (!buf) return exprtk_val_num(0);
    for (size_t i = 0; i < n; ++i) {
        double v = 0.0;
        if (args[0].type == EXPRTK_VAL_LIST) {
            exprtk_value_t item = args[0].data.list.items[i];
            if (!mod_is_number(item)) return exprtk_val_num(0);
            v = mod_number_value(item);
        } else {
            v = args[0].data.vector.data[i];
        }
        double iv = floor(v);
        if (v < 0.0 || v > 255.0 || fabs(v - iv) > 1e-9) return exprtk_val_num(0);
        buf[i] = (char)((unsigned char)iv);
    }
    buf[n] = '\0';
    return exprtk_val_str(tstr_v_from_buf(buf, n));
}

/* join(vec, delim) → string: join numeric vector into delimited string */
static exprtk_value_t fn_join(size_t argc, exprtk_value_t *args, exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 2 || args[0].type != EXPRTK_VAL_VECTOR || args[1].type != EXPRTK_VAL_STRING)
        return exprtk_val_num(0);
    size_t n = args[0].data.vector.size;
    if (n == 0) return exprtk_val_str(tstr_v_from_buf("", 0));
    tstr_v delim = args[1].data.string;
    size_t cap = n * 24 + (n > 0 ? (n - 1) * delim.len : 0) + 1;
    char *buf = mem_alloc(arena, cap);
    if (!buf) return exprtk_val_num(0);
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && delim.len > 0) {
            memcpy(buf + pos, delim.data, delim.len);
            pos += delim.len;
        }
        int written = snprintf(buf + pos, cap - pos, "%g", args[0].data.vector.data[i]);
        if (written > 0) pos += (size_t)written;
    }
    return exprtk_val_str(tstr_v_from_buf(buf, pos));
}

static const exprtk_func_entry_t string_entries[] = {
    { "assert",        fn_assert },
    { "base64_decode", fn_base64_decode },
    { "base64_encode", fn_base64_encode },
    { "base64url_decode", fn_base64url_decode },
    { "base64url_encode", fn_base64url_encode },
    { "bytes",         fn_bytes },
    { "camel_case",    fn_camel_case },
    { "capitalize",    fn_capitalize },
    { "casefold",      fn_casefold },
    { "center",        fn_center },
    { "char_at",       fn_char_at },
    { "charAt",        fn_char_at },
    { "chr",           fn_chr },
    { "codepoint_at",  fn_ord },
    { "codePointAt",   fn_ord },
    { "contains",      fn_contains },
    { "constant_time_eq", fn_constant_time_eq },
    { "count_overlapping", fn_str_count_overlapping },
    { "csv_escape",    fn_csv_escape },
    { "csv_join_line", fn_csv_join_line },
    { "csv_split_line", fn_csv_split_line },
    { "csv_unescape",  fn_csv_unescape },
    { "ends_with",     fn_ends_with },
    { "ends_with_ci",  fn_ends_with_ci },
    { "endsWith",      fn_ends_with },
    { "expand_tabs",   fn_expand_tabs },
    { "expandTabs",    fn_expand_tabs },
    { "find",          fn_find },
    { "find_all",      fn_find_all },
    { "find_all_overlapping", fn_find_all_overlapping },
    { "format",        fn_format },
    { "from_bytes",    fn_from_bytes },
    { "from_codepoint", fn_from_codepoint },
    { "fromCodePoint", fn_from_codepoint },
    { "grapheme_len",  fn_grapheme_len },
    { "grapheme_reverse", fn_grapheme_reverse },
    { "grapheme_split", fn_grapheme_split },
    { "grapheme_substr", fn_grapheme_substr },
    { "hex_decode",    fn_hex_decode },
    { "hex_encode",    fn_hex_encode },
    { "html_escape",   fn_html_escape },
    { "html_unescape", fn_html_unescape },
    { "indent",        fn_indent },
    { "index_of",      fn_index_of },
    { "indexOf",       fn_index_of },
    { "includes",      fn_contains },
    { "is_alnum",      fn_is_alnum },
    { "is_alpha",      fn_is_alpha },
    { "is_ascii",      fn_is_ascii },
    { "is_blank",      fn_is_blank },
    { "is_digit",      fn_is_digit },
    { "is_empty",      fn_is_empty },
    { "is_hex",        fn_is_hex },
    { "is_lower",      fn_is_lower },
    { "is_printable",  fn_is_printable },
    { "is_space",      fn_is_space },
    { "is_upper",      fn_is_upper },
    { "join",          fn_join },
    { "json_escape",   fn_json_escape },
    { "json_unescape", fn_json_unescape },
    { "kebab_case",    fn_kebab_case },
    { "last_index_of", fn_last_index_of },
    { "lastIndexOf",   fn_last_index_of },
    { "left",          fn_left },
    { "line_count",    fn_line_count },
    { "lpad",          fn_pad_left },
    { "length",        fn_size },
    { "size",          fn_size },
    { "byte_at",       fn_byte_at },
    { "byte_length",   fn_size },
    { "lower",         fn_lower },
    { "toLower",       fn_lower },
    { "lstrip",        fn_ltrim },
    { "ltrim",         fn_ltrim },
    { "ltrim_chars",   fn_ltrim_chars },
    { "mustache_render", fn_template_render },
    { "normalize",     fn_unicode_normalize },
    { "normalize_space", fn_normalize_space },
    { "num_to_str",    fn_to_str },
    { "ord",           fn_ord },
    { "pad_left",      fn_pad_left },
    { "pad_right",     fn_pad_right },
    { "pad_start",     fn_pad_left },
    { "pad_end",       fn_pad_right },
    { "padStart",      fn_pad_left },
    { "padEnd",        fn_pad_right },
    { "replace",       fn_replace },
    { "replace_all",   fn_replace },
    { "replace_once",  fn_replace_once },
    { "replace_range", fn_replace_range },
    { "remove_prefix", fn_remove_prefix },
    { "remove_suffix", fn_remove_suffix },
    { "remove_chars",  fn_remove_chars },
    { "dedent",        fn_dedent },
    { "unindent",      fn_dedent },
    { "reverse",       fn_reverse },
    { "repeat",        fn_str_repeat },
    { "rfind",         fn_rfind },
    { "right",         fn_right },
    { "rsplit",        fn_rsplit },
    { "rsplit_once",   fn_rsplit_once },
    { "rpad",          fn_pad_right },
    { "rstrip",        fn_rtrim },
    { "rtrim",         fn_rtrim },
    { "rtrim_chars",   fn_rtrim_chars },
    { "slice",         fn_slice },
    { "shorten",       fn_shorten },
    { "split",         fn_split },
    { "split_limit",   fn_split_limit },
    { "split_once",    fn_split_once },
    { "split_str",     fn_str_split },
    { "split_whitespace", fn_str_words },
    { "starts_with",   fn_starts_with },
    { "starts_with_ci", fn_starts_with_ci },
    { "startsWith",    fn_starts_with },
    { "str_contains_ci", fn_str_contains_ci },
    { "str_count",     fn_token_count },
    { "str_count_overlapping", fn_str_count_overlapping },
    { "str_count_substr", fn_str_count_substr },
    { "str_eq_ci",     fn_str_eq_ci },
    { "str_format",    fn_str_format },
    { "str_join",      fn_str_join },
    { "str_lines",     fn_str_lines },
    { "split_lines",   fn_str_lines },
    { "str_partition", fn_str_partition },
    { "str_repeat",    fn_str_repeat },
    { "str_rpartition", fn_str_rpartition },
    { "str_split",     fn_str_split },
    { "str_words",     fn_str_words },
    { "str_to_num",    fn_to_num },
    { "str_take",      fn_left },
    { "str_drop",      fn_drop },
    { "snake_case",    fn_snake_case },
    { "slugify",       fn_slugify },
    { "surround",      fn_surround },
    { "string_format", fn_str_format },
    { "strip",         fn_trim },
    { "strip_prefix",  fn_remove_prefix },
    { "strip_suffix",  fn_remove_suffix },
    { "parse_num",     fn_to_num },
    { "str_token",     fn_tokenize },
    { "substr",        fn_substr },
    { "count",         fn_str_count_substr },
    { "chomp",         fn_chomp },
    { "to_bool",       fn_to_bool },
    { "to_double",     fn_to_double },
    { "to_int",        fn_to_int },
    { "to_num",        fn_to_num },
    { "to_str",        fn_to_str },
    { "token_count",   fn_token_count },
    { "tokenize",      fn_tokenize },
    { "template_render", fn_template_render },
    { "title",         fn_title },
    { "truncate",      fn_truncate },
    { "translate",     fn_translate },
    { "trim",          fn_trim },
    { "trim_chars",    fn_trim_chars },
    { "trim_start",    fn_ltrim },
    { "trim_end",      fn_rtrim },
    { "trimStart",     fn_ltrim },
    { "trimEnd",       fn_rtrim },
    { "unicode_normalize", fn_unicode_normalize },
    { "unwrap",        fn_unwrap },
    { "url_decode",    fn_url_decode },
    { "url_encode",    fn_url_encode },
    { "utf8_char_at",  fn_utf8_char_at },
    { "utf8_codepoint_at", fn_utf8_codepoint_at },
    { "utf8_index_of", fn_utf8_index_of },
    { "utf8_len",      fn_utf8_len },
    { "utf8_lower",    fn_utf8_lower },
    { "utf8_reverse",  fn_utf8_reverse },
    { "utf8_rindex_of", fn_utf8_rindex_of },
    { "utf8_slice",    fn_utf8_slice },
    { "utf8_substr",   fn_utf8_substr },
    { "utf8_upper",    fn_utf8_upper },
    { "utf8_valid",    fn_utf8_valid },
    { "upper",         fn_upper },
    { "word_wrap",     fn_word_wrap },
    { "zfill",         fn_zfill },
    { "insert",        fn_insert },
    { "delete_range",  fn_delete_range },
    { "swapcase",      fn_swapcase },
    { "toUpper",       fn_upper },
    { "keep_chars",    fn_keep_chars },
    { "pascal_case",   fn_pascal_case },
};

static const exprtk_module_t string_module = {
    "string", string_entries, sizeof(string_entries) / sizeof(string_entries[0])
};

const exprtk_module_t *exprtk_module_string(void) { return &string_module; }
