/**
 * @file mod_core.c
 * @brief Core language utility module: typeof, type predicates, range.
 *
 * These are universal functions that don't belong to math/string/stats
 * but are fundamental to the language itself.
 */

#include "exprtk_module.h"
#include "exprtk_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static uuid_state_t core_uuid_state;
static int core_uuid_seeded = 0;

static exprtk_value_t core_null_value(void);
static double exprtk_numeric_value(exprtk_value_t value);

static void core_uuid_seed_once(void) {
    if (!core_uuid_seeded) {
        uuid_seed(&core_uuid_state);
        core_uuid_seeded = 1;
    }
}

static int core_uuid_text(exprtk_value_t value, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    if (value.type == EXPRTK_VAL_UUID)
        return uuid_to_s(value.data.uuid, out, (int)out_size) ? 1 : 0;
    if (value.type == EXPRTK_VAL_STRING) {
        size_t len = value.data.string.len < out_size - 1 ? value.data.string.len : out_size - 1;
        if (!value.data.string.data) return 0;
        memcpy(out, value.data.string.data, len);
        out[len] = '\0';
        return 1;
    }
    return 0;
}

static exprtk_value_t core_string_value(mem_pool_t *arena, const char *text) {
    size_t len;
    char *buf;
    if (!text) text = "";
    len = strlen(text);
    buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, text, len + 1);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static int core_datetime_from_value(exprtk_value_t value, turbo_datetime_t *out) {
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_DATETIME) {
        *out = value.data.datetime;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
        return turbo_parse_datetime(value.data.string.data, value.data.string.len, out) == 0;
    }
    return 0;
}

static int core_offset_datetime_from_value(exprtk_value_t value,
                                           exprtk_offset_datetime_t *out) {
    turbo_datetime_t dt;
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_OFFSET_DATETIME) {
        *out = value.data.offset_datetime;
        return 1;
    }
    if (value.type == EXPRTK_VAL_DATETIME && value.data.datetime.has_tz) {
        out->datetime = value.data.datetime;
        out->offset_minutes = value.data.datetime.tz_offset;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data &&
        turbo_parse_datetime(value.data.string.data, value.data.string.len, &dt) == 0 &&
        dt.has_tz) {
        out->datetime = dt;
        out->offset_minutes = dt.tz_offset;
        return 1;
    }
    return 0;
}

static int core_date_from_value(exprtk_value_t value, exprtk_date_t *out) {
    turbo_datetime_t dt;
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_DATE) {
        *out = value.data.date;
        return 1;
    }
    if (value.type == EXPRTK_VAL_DATETIME) {
        out->year = value.data.datetime.year;
        out->month = value.data.datetime.month;
        out->day = value.data.datetime.day;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
        int y = 0, m = 0, d = 0;
        if (sscanf(value.data.string.data, "%d-%d-%d", &y, &m, &d) == 3 ||
            sscanf(value.data.string.data, "%d/%d/%d", &y, &m, &d) == 3) {
            if (y >= 1 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                out->year = y;
                out->month = m;
                out->day = d;
                return 1;
            }
        }
        if (turbo_parse_datetime(value.data.string.data, value.data.string.len, &dt) == 0) {
            out->year = dt.year;
            out->month = dt.month;
            out->day = dt.day;
            return 1;
        }
    }
    return 0;
}

static int core_time_from_value(exprtk_value_t value, exprtk_time_t *out) {
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_TIME) {
        *out = value.data.time;
        return 1;
    }
    if (value.type == EXPRTK_VAL_DATETIME) {
        out->hour = value.data.datetime.hour;
        out->minute = value.data.datetime.minute;
        out->second = value.data.datetime.second;
        out->millisecond = value.data.datetime.millisecond;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
        int h = 0, m = 0, s = 0, ms = 0;
        int consumed = 0;
        if (sscanf(value.data.string.data, "%d:%d:%d.%d%n", &h, &m, &s, &ms, &consumed) >= 3 ||
            sscanf(value.data.string.data, "%d:%d:%d%n", &h, &m, &s, &consumed) >= 3 ||
            sscanf(value.data.string.data, "%d:%d%n", &h, &m, &consumed) >= 2) {
            if (h >= 0 && h <= 23 && m >= 0 && m <= 59 && s >= 0 && s <= 60 &&
                ms >= 0 && ms <= 999) {
                out->hour = h;
                out->minute = m;
                out->second = s;
                out->millisecond = ms;
                return 1;
            }
        }
    }
    return 0;
}

static int core_duration_from_value(exprtk_value_t value, int64_t *out) {
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_DURATION) {
        *out = value.data.duration_ms;
        return 1;
    }
    if (value.type == EXPRTK_VAL_INTEGER) {
        *out = value.data.integer;
        return 1;
    }
    if (value.type == EXPRTK_VAL_NUMBER) {
        *out = (int64_t)value.data.number;
        return 1;
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data) {
        const char *p = value.data.string.data;
        const char *end = p + value.data.string.len;
        int64_t total = 0;
        while (p < end) {
            char *next = NULL;
            double n;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end) break;
            n = strtod(p, &next);
            if (next == p) return 0;
            p = next;
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end) {
                total += (int64_t)n;
                break;
            }
            if ((end - p) >= 2 && p[0] == 'm' && p[1] == 's') {
                total += (int64_t)n;
                p += 2;
            } else if (*p == 's') {
                total += (int64_t)(n * 1000.0);
                p++;
            } else if (*p == 'm') {
                total += (int64_t)(n * 60.0 * 1000.0);
                p++;
            } else if (*p == 'h') {
                total += (int64_t)(n * 60.0 * 60.0 * 1000.0);
                p++;
            } else if (*p == 'd') {
                total += (int64_t)(n * 24.0 * 60.0 * 60.0 * 1000.0);
                p++;
            } else {
                return 0;
            }
        }
        *out = total;
        return 1;
    }
    return 0;
}

static int core_date_text(exprtk_date_t date, char *out, size_t out_size) {
    return out && out_size > 0 &&
           snprintf(out, out_size, "%04d-%02d-%02d", date.year, date.month, date.day) > 0;
}

static int core_time_text(exprtk_time_t time, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    if (time.millisecond > 0)
        return snprintf(out, out_size, "%02d:%02d:%02d.%03d", time.hour, time.minute,
                        time.second, time.millisecond) > 0;
    return snprintf(out, out_size, "%02d:%02d:%02d", time.hour, time.minute,
                    time.second) > 0;
}

static int core_duration_text(int64_t ms, char *out, size_t out_size) {
    int64_t sign = ms < 0 ? -1 : 1;
    int64_t rem = ms < 0 ? -ms : ms;
    int64_t h = rem / 3600000;
    int64_t m;
    int64_t s;
    rem %= 3600000;
    m = rem / 60000;
    rem %= 60000;
    s = rem / 1000;
    rem %= 1000;
    if (!out || out_size == 0) return 0;
    return snprintf(out, out_size, "%s%lld:%02lld:%02lld.%03lld",
                    sign < 0 ? "-" : "", (long long)h, (long long)m,
                    (long long)s, (long long)rem) > 0;
}

static int core_decimal_normalize(exprtk_decimal_t *value) {
    if (!value || value->scale < 0) return 0;
    while (value->scale > 0 && value->mantissa % 10 == 0) {
        value->mantissa /= 10;
        value->scale--;
    }
    if (value->mantissa == 0) value->scale = 0;
    return 1;
}

static int core_decimal_from_text(const char *text, size_t len, exprtk_decimal_t *out) {
    const char *p;
    const char *end;
    int sign = 1;
    int saw_digit = 0;
    int saw_dot = 0;
    int32_t scale = 0;
    uint64_t acc = 0;
    uint64_t limit;

    if (!text || !out) return 0;
    p = text;
    end = text + len;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p < end && *p == '-') {
        sign = -1;
        p++;
    } else if (p < end && *p == '+') {
        p++;
    }
    limit = sign < 0 ? (uint64_t)INT64_MAX + 1ULL : (uint64_t)INT64_MAX;
    while (p < end) {
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
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p == end) break;
        }
        return 0;
    }
    if (!saw_digit) return 0;
    out->mantissa = sign < 0
                        ? (acc == (uint64_t)INT64_MAX + 1ULL ? INT64_MIN : -(int64_t)acc)
                        : (int64_t)acc;
    out->scale = scale;
    return core_decimal_normalize(out);
}

static int core_decimal_from_value(exprtk_value_t value, exprtk_decimal_t *out) {
    char buf[64];
    int n;
    if (!out) return 0;
    if (value.type == EXPRTK_VAL_DECIMAL) {
        *out = value.data.decimal;
        return core_decimal_normalize(out);
    }
    if (value.type == EXPRTK_VAL_INTEGER) {
        out->mantissa = value.data.integer;
        out->scale = 0;
        return 1;
    }
    if (value.type == EXPRTK_VAL_NUMBER) {
        n = snprintf(buf, sizeof(buf), "%.17g", value.data.number);
        return n > 0 && core_decimal_from_text(buf, (size_t)n, out);
    }
    if (value.type == EXPRTK_VAL_STRING && value.data.string.data)
        return core_decimal_from_text(value.data.string.data, value.data.string.len, out);
    return 0;
}

static int core_decimal_text(exprtk_decimal_t value, char *out, size_t out_size) {
    char digits[32];
    char *p = digits + sizeof(digits);
    uint64_t mag;
    size_t digit_count;
    size_t pos = 0;
    int negative;

    if (!out || out_size == 0 || !core_decimal_normalize(&value)) return 0;
    negative = value.mantissa < 0;
    mag = negative ? (uint64_t)(-(value.mantissa + 1)) + 1ULL : (uint64_t)value.mantissa;
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
    if (value.scale == 0) {
        if (pos + digit_count >= out_size) return 0;
        memcpy(out + pos, p, digit_count + 1);
        return 1;
    }
    if ((size_t)value.scale >= digit_count) {
        size_t zeros = (size_t)value.scale - digit_count;
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
        size_t whole = digit_count - (size_t)value.scale;
        if (pos + digit_count + 1 >= out_size) return 0;
        memcpy(out + pos, p, whole);
        pos += whole;
        out[pos++] = '.';
        memcpy(out + pos, p + whole, (size_t)value.scale);
        pos += (size_t)value.scale;
        out[pos] = '\0';
        return 1;
    }
}

static int core_bigint_valid_text(tstr_v text) {
    size_t i = 0;
    if (!text.data || text.len == 0) return 0;
    if (text.data[0] == '-' || text.data[0] == '+') i = 1;
    if (i == text.len) return 0;
    for (; i < text.len; ++i) {
        if (!isdigit((unsigned char)text.data[i])) return 0;
    }
    return 1;
}

static exprtk_value_t core_bigint_from_value(exprtk_value_t value, mem_pool_t *arena) {
    char buf[64];
    const char *text = NULL;
    size_t len = 0;
    char *copy;

    if (value.type == EXPRTK_VAL_BIGINT) return value;
    if (value.type == EXPRTK_VAL_INTEGER) {
        int n = snprintf(buf, sizeof(buf), "%lld", (long long)value.data.integer);
        if (n <= 0) return core_null_value();
        text = buf;
        len = (size_t)n;
    } else if (value.type == EXPRTK_VAL_STRING && core_bigint_valid_text(value.data.string)) {
        text = value.data.string.data;
        len = value.data.string.len;
    } else {
        return core_null_value();
    }
    copy = (char *)mem_alloc(arena, len + 1);
    if (!copy) return core_null_value();
    memcpy(copy, text, len);
    copy[len] = '\0';
    return exprtk_val_bigint(tstr_v_from_buf(copy, len));
}

static int core_money_text(exprtk_money_t money, char *out, size_t out_size) {
    char amount[64];
    if (!core_decimal_text(money.amount, amount, sizeof(amount))) return 0;
    return out && out_size > 0 &&
           snprintf(out, out_size, "%c%c%c %s", money.currency[0], money.currency[1],
                    money.currency[2], amount) > 0;
}

static int core_offset_datetime_text(exprtk_offset_datetime_t value,
                                     char *out, size_t out_size) {
    int offset = value.offset_minutes;
    char sign = '+';
    if (!out || out_size == 0) return 0;
    if (offset < 0) {
        sign = '-';
        offset = -offset;
    }
    return snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
                    value.datetime.year, value.datetime.month, value.datetime.day,
                    value.datetime.hour, value.datetime.minute, value.datetime.second,
                    sign, offset / 60, offset % 60) > 0;
}

static size_t core_typed_array_element_size(exprtk_typed_array_kind_t kind) {
    switch (kind) {
        case EXPRTK_TYPED_I32: return sizeof(int32_t);
        case EXPRTK_TYPED_I64: return sizeof(int64_t);
        case EXPRTK_TYPED_F32: return sizeof(float);
        case EXPRTK_TYPED_F64: return sizeof(double);
        default: return 0;
    }
}

static exprtk_value_t core_typed_array_create(exprtk_typed_array_kind_t kind,
                                              size_t argc, exprtk_value_t *args,
                                              mem_pool_t *arena) {
    size_t count = argc;
    size_t elem_size = core_typed_array_element_size(kind);
    void *data;
    exprtk_value_t source = argc == 1 ? args[0] : core_null_value();

    if (argc == 1 && source.type == EXPRTK_VAL_LIST) {
        count = source.data.list.count;
        args = source.data.list.items;
    } else if (argc == 1 && source.type == EXPRTK_VAL_VECTOR) {
        count = source.data.vector.size;
    } else if (argc == 1 && source.type == EXPRTK_VAL_TYPED_ARRAY) {
        return source;
    }
    if (elem_size == 0) return core_null_value();
    data = mem_alloc(arena, count * elem_size);
    if (!data && count > 0) return core_null_value();

    for (size_t i = 0; i < count; ++i) {
        exprtk_value_t item = (argc == 1 && source.type == EXPRTK_VAL_VECTOR)
                                  ? exprtk_val_num(source.data.vector.data[i])
                                  : args[i];
        double n = exprtk_numeric_value(item);
        switch (kind) {
            case EXPRTK_TYPED_I32: ((int32_t *)data)[i] = (int32_t)n; break;
            case EXPRTK_TYPED_I64: ((int64_t *)data)[i] = (int64_t)n; break;
            case EXPRTK_TYPED_F32: ((float *)data)[i] = (float)n; break;
            case EXPRTK_TYPED_F64: ((double *)data)[i] = n; break;
            default: break;
        }
    }
    return exprtk_val_typed_array(kind, data, count, 0);
}

static exprtk_value_t core_null_value(void) {
    exprtk_value_t value;
    memset(&value, 0, sizeof(value));
    value.type = EXPRTK_VAL_NULL;
    return value;
}

static int exprtk_is_numeric(exprtk_value_t value) {
    return value.type == EXPRTK_VAL_NUMBER || value.type == EXPRTK_VAL_INTEGER ||
           value.type == EXPRTK_VAL_BOOL;
}

static double exprtk_numeric_value(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_INTEGER) return (double)value.data.integer;
    if (value.type == EXPRTK_VAL_NUMBER) return value.data.number;
    if (value.type == EXPRTK_VAL_BOOL) return value.data.boolean ? 1.0 : 0.0;
    return 0.0;
}

static int exprtk_value_truthy(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_NUMBER) return fabs(value.data.number) > 1e-9;
    if (value.type == EXPRTK_VAL_INTEGER) return value.data.integer != 0;
    if (value.type == EXPRTK_VAL_BOOL) return value.data.boolean != 0;
    if (value.type == EXPRTK_VAL_STRING) return value.data.string.len > 0;
    if (value.type == EXPRTK_VAL_BYTES) return value.data.bytes.len > 0;
    if (value.type == EXPRTK_VAL_UUID) return 1;
    if (value.type == EXPRTK_VAL_DATETIME || value.type == EXPRTK_VAL_DATE ||
        value.type == EXPRTK_VAL_TIME || value.type == EXPRTK_VAL_DURATION ||
        value.type == EXPRTK_VAL_DECIMAL || value.type == EXPRTK_VAL_BIGINT ||
        value.type == EXPRTK_VAL_MONEY || value.type == EXPRTK_VAL_ENUM ||
        value.type == EXPRTK_VAL_FLAGS || value.type == EXPRTK_VAL_OFFSET_DATETIME)
        return 1;
    if (value.type == EXPRTK_VAL_VECTOR) return value.data.vector.size > 0;
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_SET)
        return value.data.list.count > 0;
    if (value.type == EXPRTK_VAL_TYPED_ARRAY) return value.data.typed_array.count > 0;
    if (exprtk_value_is_object_like(&value)) return exprtk_map_count(&value) > 0;
    if (value.type == EXPRTK_VAL_FUNCTION || value.type == EXPRTK_VAL_CLASS ||
        value.type == EXPRTK_VAL_INSTANCE || value.type == EXPRTK_VAL_BOUND_METHOD)
        return 1;
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

static exprtk_value_t stream_make_text(exprtk_value_t text) {
    exprtk_value_t stream = stream_make(text);
    exprtk_map_set(&stream, "source_kind", exprtk_val_str(tstr_v_from_cstr("text")));
    exprtk_map_set(&stream, "text_text", text);
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

    if (!exprtk_value_is_object_like(&source)) return values;
    it = exprtk_map_iter_begin(&source);
    while (exprtk_map_iter_next(&it, NULL, &value)) {
        exprtk_list_push(&values, value);
    }
    return values;
}

static exprtk_value_t stream_source_from_value(exprtk_value_t value) {
    if (exprtk_value_is_object_like(&value)) return stream_map_entries(value);
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_VECTOR) return value;
    return exprtk_val_list_empty();
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

static int stream_value_equals_cstr(exprtk_value_t value, const char *text) {
    size_t len = text ? strlen(text) : 0;
    return value.type == EXPRTK_VAL_STRING &&
           value.data.string.len == len &&
           value.data.string.data &&
           memcmp(value.data.string.data, text, len) == 0;
}

static int stream_source_kind_value(exprtk_value_t stream, exprtk_value_t *out) {
    exprtk_value_t kind;

    if (!out || !stream_is_value(stream) || !exprtk_map_has(&stream, "source_kind"))
        return 0;
    kind = exprtk_map_get(&stream, "source_kind");
    if (kind.type != EXPRTK_VAL_STRING || !kind.data.string.data) return 0;
    *out = kind;
    return 1;
}

static int stream_dispatch_provider_member(exprtk_value_t stream, const char *method,
                                           size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, mem_pool_t *arena,
                                           exprtk_value_t *out) {
    exprtk_value_t kind;
    char fn_name[160];
    exprtk_value_t stack_args[8];
    exprtk_value_t *call_args = stack_args;
    size_t total_argc;

    if (!method || !env || !out || !stream_source_kind_value(stream, &kind)) return 0;
    if (kind.data.string.len + strlen(method) + strlen("stream..") >= sizeof(fn_name))
        return 0;
    snprintf(fn_name, sizeof(fn_name), "stream.%.*s.%s",
             (int)kind.data.string.len, kind.data.string.data, method);
    if (!exprtk_env_has_func(env, fn_name) && !exprtk_find_builtin(fn_name, env)) return 0;

    total_argc = argc + 1;
    if (total_argc > sizeof(stack_args) / sizeof(stack_args[0])) {
        call_args = (exprtk_value_t *)malloc(total_argc * sizeof(exprtk_value_t));
        if (!call_args) return 0;
    }
    call_args[0] = stream;
    for (size_t i = 0; i < argc; ++i) call_args[i + 1] = args[i];

    *out = exprtk_call_internal(fn_name, total_argc, call_args, env, arena);
    if (call_args != stack_args) free(call_args);
    return 1;
}

static exprtk_value_t stream_collect_value(exprtk_value_t stream) {
    if (exprtk_value_is_object_like(&stream) && exprtk_map_has(&stream, "source"))
        return exprtk_map_get(&stream, "source");
    return exprtk_val_list_empty();
}

static size_t stream_count_source(exprtk_value_t source) {
    if (source.type == EXPRTK_VAL_LIST) return source.data.list.count;
    if (source.type == EXPRTK_VAL_VECTOR) return source.data.vector.size;
    if (exprtk_value_is_object_like(&source)) return exprtk_map_count(&source);
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
    exprtk_value_t kind;
    exprtk_value_t provider_result;
    if (!method || !stream_is_value(stream))
        return exprtk_val_num(0);

    if (stream_dispatch_provider_member(stream, method, argc, args, env, arena,
                                        &provider_result))
        return provider_result;
    int has_source_kind = stream_source_kind_value(stream, &kind);

    if (strcmp(method, "lines") == 0 && argc == 0) {
        if (has_source_kind && !stream_value_equals_cstr(kind, "text"))
            return stream_make(exprtk_val_list_empty());
        if (!stream_text_value(stream, &text)) return stream_make(exprtk_val_list_empty());
        return stream_make(stream_lines_from_string(text, arena));
    }
    if (strcmp(method, "split") == 0 && argc == 1) {
        if (has_source_kind && !stream_value_equals_cstr(kind, "text"))
            return stream_make(exprtk_val_list_empty());
        if (!stream_text_value(stream, &text)) return stream_make(exprtk_val_list_empty());
        return stream_make(stream_split_string(text, args[0], arena));
    }
    if (strcmp(method, "filter") == 0 && argc == 1)
        return stream_filter_value(stream, args[0], env, arena);
    if ((strcmp(method, "filterExpr") == 0 || strcmp(method, "where") == 0) && argc == 1)
        return stream_make(exprtk_val_list_empty());
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
            case EXPRTK_VAL_INTEGER: name = "int64"; len = 5; break;
            case EXPRTK_VAL_BOOL:   name = "bool";   len = 4; break;
            case EXPRTK_VAL_STRING: name = "string"; len = 6; break;
            case EXPRTK_VAL_BYTES:  name = "bytes";  len = 5; break;
            case EXPRTK_VAL_UUID:   name = "uuid";   len = 4; break;
            case EXPRTK_VAL_DATETIME: name = "datetime"; len = 8; break;
            case EXPRTK_VAL_DATE:   name = "date";   len = 4; break;
            case EXPRTK_VAL_TIME:   name = "time";   len = 4; break;
            case EXPRTK_VAL_DURATION: name = "duration"; len = 8; break;
            case EXPRTK_VAL_DECIMAL: name = "decimal"; len = 7; break;
            case EXPRTK_VAL_BIGINT: name = "bigint"; len = 6; break;
            case EXPRTK_VAL_MONEY: name = "money"; len = 5; break;
            case EXPRTK_VAL_ENUM: name = "enum"; len = 4; break;
            case EXPRTK_VAL_FLAGS: name = "flags"; len = 5; break;
            case EXPRTK_VAL_SET: name = "set"; len = 3; break;
            case EXPRTK_VAL_OFFSET_DATETIME: name = "offset_datetime"; len = 15; break;
            case EXPRTK_VAL_TYPED_ARRAY: name = "typed_array"; len = 11; break;
            case EXPRTK_VAL_VECTOR: name = "vector"; len = 6; break;
            case EXPRTK_VAL_MAP:    name = "map";    len = 3; break;
            case EXPRTK_VAL_OBJECT: name = "object"; len = 6; break;
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

static exprtk_value_t fn_is_int64(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_INTEGER ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_bool(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_BOOL ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_string(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_STRING ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_bytes(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_BYTES ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_uuid(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_UUID ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_datetime(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_DATETIME ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_date(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_DATE ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_time(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_TIME ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_duration(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_DURATION ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_decimal(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_DECIMAL ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_bigint(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_BIGINT ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_money(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_MONEY ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_enum(size_t argc, exprtk_value_t *args,
                                 exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_ENUM ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_flags(size_t argc, exprtk_value_t *args,
                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_FLAGS ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_offset_datetime(size_t argc, exprtk_value_t *args,
                                            exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_OFFSET_DATETIME ? 1.0 : 0.0);
}

static exprtk_value_t fn_is_typed_array(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_TYPED_ARRAY ? 1.0 : 0.0);
}

static exprtk_value_t fn_uuid(size_t argc, exprtk_value_t *args,
                              exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    char text[UUID4_STR_BUFFER_SIZE];
    uuid_t id;

    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING ||
        !core_uuid_text(args[0], text, sizeof(text)) || !uuid_from_s(text, &id))
        return exprtk_val_num(0);
    return exprtk_val_uuid(id);
}

static exprtk_value_t fn_uuid4(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)argc; (void)args; (void)env; (void)arena;
    uuid_t id;
    core_uuid_seed_once();
    uuid4_gen(&core_uuid_state, &id);
    return exprtk_val_uuid(id);
}

static exprtk_value_t fn_uuid7(size_t argc, exprtk_value_t *args,
                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)argc; (void)args; (void)env; (void)arena;
    uuid_t id;
    core_uuid_seed_once();
    uuid7_gen(&core_uuid_state, &id);
    return exprtk_val_uuid(id);
}

static exprtk_value_t fn_uuid_string(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    char text[UUID4_STR_BUFFER_SIZE];

    if (argc != 1 || !core_uuid_text(args[0], text, sizeof(text)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, text);
}

static exprtk_value_t fn_datetime_parse(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    turbo_datetime_t dt;
    if (argc != 1 || !core_datetime_from_value(args[0], &dt))
        return core_null_value();
    return exprtk_val_datetime(dt);
}

static exprtk_value_t fn_datetime_timestamp(size_t argc, exprtk_value_t *args,
                                            exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    turbo_datetime_t dt;
    time_t ts;
    if (argc != 1 || !core_datetime_from_value(args[0], &dt))
        return exprtk_val_num(-1.0);
    ts = turbo_datetime_to_time(&dt);
    return exprtk_val_num((double)ts);
}

static exprtk_value_t fn_datetime_string(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    turbo_datetime_t dt;
    time_t ts;
    char buf[64];
    if (argc != 1 || !core_datetime_from_value(args[0], &dt))
        return exprtk_val_str(tstr_v_from_cstr(""));
    ts = turbo_datetime_to_time(&dt);
    if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, buf, sizeof(buf)) < 0)
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_datetime_format_rfc822(size_t argc, exprtk_value_t *args,
                                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    time_t ts;
    char buf[64];
    if (argc != 1)
        return exprtk_val_str(tstr_v_from_cstr(""));
    if (args[0].type == EXPRTK_VAL_INTEGER)
        ts = (time_t)args[0].data.integer;
    else if (args[0].type == EXPRTK_VAL_NUMBER)
        ts = (time_t)args[0].data.number;
    else if (args[0].type == EXPRTK_VAL_DATETIME)
        ts = turbo_datetime_to_time(&args[0].data.datetime);
    else
        return exprtk_val_str(tstr_v_from_cstr(""));
    if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, buf, sizeof(buf)) < 0)
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_offset_datetime_parse(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_offset_datetime_t value;
    if (argc != 1 || !core_offset_datetime_from_value(args[0], &value))
        return core_null_value();
    return exprtk_val_offset_datetime(value.datetime, value.offset_minutes);
}

static exprtk_value_t fn_offset_datetime_timestamp(size_t argc, exprtk_value_t *args,
                                                  exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_offset_datetime_t value;
    if (argc != 1 || !core_offset_datetime_from_value(args[0], &value))
        return exprtk_val_num(-1.0);
    return exprtk_val_num((double)turbo_datetime_to_time(&value.datetime));
}

static exprtk_value_t fn_offset_datetime_string(size_t argc, exprtk_value_t *args,
                                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    exprtk_offset_datetime_t value;
    char buf[64];
    if (argc != 1 || !core_offset_datetime_from_value(args[0], &value) ||
        !core_offset_datetime_text(value, buf, sizeof(buf)))
        return core_string_value(arena, "");
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_date_parse(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_date_t date;
    if (argc != 1 || !core_date_from_value(args[0], &date))
        return core_null_value();
    return exprtk_val_date(date);
}

static exprtk_value_t fn_date_string(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    exprtk_date_t date;
    char buf[32];
    if (argc != 1 || !core_date_from_value(args[0], &date) ||
        !core_date_text(date, buf, sizeof(buf)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_time_parse(size_t argc, exprtk_value_t *args,
                                    exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_time_t time;
    if (argc != 1 || !core_time_from_value(args[0], &time))
        return core_null_value();
    return exprtk_val_time(time);
}

static exprtk_value_t fn_time_string(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    exprtk_time_t time;
    char buf[32];
    if (argc != 1 || !core_time_from_value(args[0], &time) ||
        !core_time_text(time, buf, sizeof(buf)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_duration_parse(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    int64_t ms;
    if (argc != 1 || !core_duration_from_value(args[0], &ms))
        return core_null_value();
    return exprtk_val_duration(ms);
}

static exprtk_value_t fn_duration_milliseconds(size_t argc, exprtk_value_t *args,
                                               exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    int64_t ms;
    if (argc != 1 || !core_duration_from_value(args[0], &ms))
        return exprtk_val_num(0.0);
    return exprtk_val_int(ms);
}

static exprtk_value_t fn_duration_seconds(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    int64_t ms;
    if (argc != 1 || !core_duration_from_value(args[0], &ms))
        return exprtk_val_num(0.0);
    return exprtk_val_num((double)ms / 1000.0);
}

static exprtk_value_t fn_duration_string(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    int64_t ms;
    char buf[64];
    if (argc != 1 || !core_duration_from_value(args[0], &ms) ||
        !core_duration_text(ms, buf, sizeof(buf)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_decimal_parse(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_decimal_t value;
    if (argc != 1 || !core_decimal_from_value(args[0], &value))
        return core_null_value();
    return exprtk_val_decimal(value);
}

static exprtk_value_t fn_decimal_to_string(size_t argc, exprtk_value_t *args,
                                           exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    exprtk_decimal_t value;
    char buf[64];
    if (argc != 1 || !core_decimal_from_value(args[0], &value) ||
        !core_decimal_text(value, buf, sizeof(buf)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_decimal_mantissa(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_decimal_t value;
    if (argc != 1 || !core_decimal_from_value(args[0], &value))
        return exprtk_val_int(0);
    return exprtk_val_int(value.mantissa);
}

static exprtk_value_t fn_decimal_scale(size_t argc, exprtk_value_t *args,
                                       exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_decimal_t value;
    if (argc != 1 || !core_decimal_from_value(args[0], &value))
        return exprtk_val_int(0);
    return exprtk_val_int(value.scale);
}

static exprtk_value_t fn_bigint_parse(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    if (argc != 1) return core_null_value();
    return core_bigint_from_value(args[0], arena);
}

static exprtk_value_t fn_bigint_to_string(size_t argc, exprtk_value_t *args,
                                          exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    exprtk_value_t value;
    if (argc != 1) return core_string_value(arena, "");
    value = core_bigint_from_value(args[0], arena);
    if (value.type != EXPRTK_VAL_BIGINT) return core_string_value(arena, "");
    return core_string_value(arena, value.data.bigint.text.data ? value.data.bigint.text.data : "");
}

static exprtk_value_t fn_money_create(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    exprtk_decimal_t amount;
    exprtk_money_t money;
    if (argc != 2 || !core_decimal_from_value(args[0], &amount) ||
        args[1].type != EXPRTK_VAL_STRING || args[1].data.string.len != 3)
        return core_null_value();
    memset(&money, 0, sizeof(money));
    money.amount = amount;
    memcpy(money.currency, args[1].data.string.data, 3);
    money.currency[3] = '\0';
    (void)arena;
    return exprtk_val_money(money);
}

static exprtk_value_t fn_money_to_string(size_t argc, exprtk_value_t *args,
                                         exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    char buf[96];
    if (argc != 1 || args[0].type != EXPRTK_VAL_MONEY ||
        !core_money_text(args[0].data.money, buf, sizeof(buf)))
        return core_string_value(arena, "");
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_money_amount(size_t argc, exprtk_value_t *args,
                                      exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_MONEY) return exprtk_val_decimal((exprtk_decimal_t){0, 0});
    return exprtk_val_decimal(args[0].data.money.amount);
}

static exprtk_value_t fn_money_currency(size_t argc, exprtk_value_t *args,
                                        exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    char buf[4];
    if (argc != 1 || args[0].type != EXPRTK_VAL_MONEY) return core_string_value(arena, "");
    memcpy(buf, args[0].data.money.currency, 3);
    buf[3] = '\0';
    return core_string_value(arena, buf);
}

static exprtk_value_t fn_typed_i32(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    return core_typed_array_create(EXPRTK_TYPED_I32, argc, args, arena);
}

static exprtk_value_t fn_typed_i64(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    return core_typed_array_create(EXPRTK_TYPED_I64, argc, args, arena);
}

static exprtk_value_t fn_typed_f32(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    return core_typed_array_create(EXPRTK_TYPED_F32, argc, args, arena);
}

static exprtk_value_t fn_typed_f64(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env;
    return core_typed_array_create(EXPRTK_TYPED_F64, argc, args, arena);
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

static exprtk_value_t fn_is_object(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_OBJECT ? 1.0 : 0.0);
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

static exprtk_value_t fn_is_set(size_t argc, exprtk_value_t *args,
                                exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    return exprtk_val_num(argc > 0 && args[0].type == EXPRTK_VAL_SET ? 1.0 : 0.0);
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

    if (exprtk_value_is_object_like(&args[0]) && exprtk_map_has(&args[0], "status") &&
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
            case EXPRTK_VAL_BOOL: printf("%s", args[i].data.boolean ? "true" : "false"); break;
            case EXPRTK_VAL_STRING: printf("%.*s", (int)args[i].data.string.len, args[i].data.string.data); break;
            case EXPRTK_VAL_BYTES: printf("bytes(%zu)", args[i].data.bytes.len); break;
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
            case EXPRTK_VAL_OBJECT: printf("{object:%zu}", exprtk_map_count(&args[i])); break;
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
    if (!exprtk_value_truthy(args[0])) {
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

static exprtk_value_t fn_set(size_t argc, exprtk_value_t *args,
                             exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    exprtk_value_t set = exprtk_val_set_empty();
    for (size_t i = 0; i < argc; ++i) {
        int exists = 0;
        for (size_t j = 0; j < set.data.list.count; ++j) {
            if (values_match(set.data.list.items[j], args[i])) {
                exists = 1;
                break;
            }
        }
        if (!exists) exprtk_list_push(&set, args[i]);
    }
    return set;
}

static exprtk_value_t fn_stream_of(size_t argc, exprtk_value_t *args,
                                   exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1) return stream_make(exprtk_val_list_empty());
    return stream_of_value(args[0]);
}

static exprtk_value_t fn_stream_text(size_t argc, exprtk_value_t *args,
                                     exprtk_env_t *env, mem_pool_t *arena) {
    (void)env; (void)arena;
    if (argc != 1 || args[0].type != EXPRTK_VAL_STRING)
        return stream_make_text(exprtk_val_str(tstr_v_from_buf("", 0)));
    return stream_make_text(args[0]);
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
    exprtk_value_t out = row.type == EXPRTK_VAL_OBJECT ? exprtk_val_object() : exprtk_val_map();
    if (!exprtk_value_is_object_like(&row)) return out;
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
        if (!exprtk_value_is_object_like(&row)) continue;
        exprtk_value_t selected = row.type == EXPRTK_VAL_OBJECT ? exprtk_val_object() : exprtk_val_map();
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
        if (!exprtk_value_is_object_like(&row) || !exprtk_map_has(&row, key_field)) continue;
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
        if (!exprtk_value_is_object_like(&left) || !exprtk_map_has(&left, key_field)) continue;
        char lk[128];
        if (!table_key_from_value(exprtk_map_get(&left, key_field), lk, sizeof(lk))) continue;
        for (size_t j = 0; j < args[1].data.list.count; ++j) {
            exprtk_value_t right = args[1].data.list.items[j];
            if (!exprtk_value_is_object_like(&right) || !exprtk_map_has(&right, key_field)) continue;
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
    { "is_bool",   fn_is_bool },
    { "is_bigint", fn_is_bigint },
    { "is_bytes",  fn_is_bytes },
    { "is_class",  fn_is_class },
    { "is_date",   fn_is_date },
    { "is_datetime", fn_is_datetime },
    { "is_decimal", fn_is_decimal },
    { "is_duration", fn_is_duration },
    { "is_enum",   fn_is_enum },
    { "is_flags",  fn_is_flags },
    { "is_function", fn_is_function },
    { "is_int64",  fn_is_int64 },
    { "is_instance", fn_is_instance },
    { "is_list",   fn_is_list },
    { "is_map",    fn_is_map },
    { "is_null",   fn_is_null },
    { "is_number", fn_is_number },
    { "is_object", fn_is_object },
    { "is_offset_datetime", fn_is_offset_datetime },
    { "is_string", fn_is_string },
    { "is_set",    fn_is_set },
    { "is_time",   fn_is_time },
    { "is_typed_array", fn_is_typed_array },
    { "is_uuid",   fn_is_uuid },
    { "is_vector", fn_is_vector },
    { "lag",       fn_lag },
    { "list",      fn_list },
    { "map",       fn_map },
    { "money",     fn_money_create },
    { "money.create", fn_money_create },
    { "money.to_string", fn_money_to_string },
    { "money.amount", fn_money_amount },
    { "money.currency", fn_money_currency },
    { "print",     fn_print },
    { "reduce",    fn_reduce },
    { "range",     fn_range },
    { "stream.text", fn_stream_text },
    { "set",       fn_set },
    { "stream.of", fn_stream_of },
    { "table.filter", fn_table_filter },
    { "table.groupby", fn_table_groupby },
    { "table.join", fn_table_join },
    { "table.select", fn_table_select },
    { "take",      fn_take },
    { "typeof",    fn_typeof },
    { "datetime",  fn_datetime_parse },
    { "datetime.parse", fn_datetime_parse },
    { "datetime.to_string", fn_datetime_string },
    { "datetime.to_time", fn_datetime_timestamp },
    { "datetime.timestamp", fn_datetime_timestamp },
    { "datetime.format_rfc822", fn_datetime_format_rfc822 },
    { "datetime_string", fn_datetime_string },
    { "offset_datetime", fn_offset_datetime_parse },
    { "offset_datetime.parse", fn_offset_datetime_parse },
    { "offset_datetime.to_string", fn_offset_datetime_string },
    { "offset_datetime.to_time", fn_offset_datetime_timestamp },
    { "offset_datetime.timestamp", fn_offset_datetime_timestamp },
    { "decimal.parse", fn_decimal_parse },
    { "decimal.to_string", fn_decimal_to_string },
    { "decimal.mantissa", fn_decimal_mantissa },
    { "decimal.scale", fn_decimal_scale },
    { "bigint", fn_bigint_parse },
    { "bigint.parse", fn_bigint_parse },
    { "bigint.to_string", fn_bigint_to_string },
    { "typed.i32", fn_typed_i32 },
    { "typed.i64", fn_typed_i64 },
    { "typed.f32", fn_typed_f32 },
    { "typed.f64", fn_typed_f64 },
    { "typed_array.i32", fn_typed_i32 },
    { "typed_array.i64", fn_typed_i64 },
    { "typed_array.f32", fn_typed_f32 },
    { "typed_array.f64", fn_typed_f64 },
    { "decimal_string", fn_decimal_to_string },
    { "date.parse", fn_date_parse },
    { "date.to_string", fn_date_string },
    { "date_string", fn_date_string },
    { "time.parse", fn_time_parse },
    { "time.to_string", fn_time_string },
    { "time_string", fn_time_string },
    { "duration.parse", fn_duration_parse },
    { "duration.milliseconds", fn_duration_milliseconds },
    { "duration.seconds", fn_duration_seconds },
    { "duration.to_string", fn_duration_string },
    { "duration_string", fn_duration_string },
    { "uuid",      fn_uuid },
    { "uuid.to_string", fn_uuid_string },
    { "uuid4",     fn_uuid4 },
    { "uuid7",     fn_uuid7 },
    { "uuid_string", fn_uuid_string },
};

static const exprtk_module_t core_module = {
    "core", core_entries, sizeof(core_entries) / sizeof(core_entries[0])
};

const exprtk_module_t *exprtk_module_core(void) { return &core_module; }
