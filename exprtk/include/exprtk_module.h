/**
 * @file exprtk_module.h
 * @brief Public module descriptor API for exprtk.
 *
 * External modules (e.g. fin under turbo_script) include this header
 * to define exprtk_module_t instances without depending on any private
 * exprtk internals.
 */
#ifndef EXPRTK_MODULE_H
#define EXPRTK_MODULE_H

#include "exprtk_types.h"
#include "turbo_buffer.h"
#include "turbo_vec.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Module function signature & descriptor types
 * ========================================================================= */

typedef exprtk_value_t (*exprtk_builtin_fn)(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, mem_pool_t *scratch);

typedef struct {
    const char *name;
    exprtk_builtin_fn fn;
} exprtk_func_entry_t;

struct exprtk_module_s {
    const char *module_name;
    const exprtk_func_entry_t *entries;
    size_t count;
};

/* =========================================================================
 * Value constructors (inline helpers for module implementations)
 * ========================================================================= */

static inline exprtk_value_t exprtk_val_num(double v) {
    exprtk_value_t val = {0};
    val.type = EXPRTK_VAL_NUMBER;
    val.data.number = v;
    return val;
}

static inline exprtk_value_t exprtk_val_int(int64_t v) {
    exprtk_value_t val = {0};
    val.type = EXPRTK_VAL_INTEGER;
    val.data.integer = v;
    return val;
}

static inline exprtk_value_t exprtk_val_bool(int v) {
    exprtk_value_t val = {0};
    val.type = EXPRTK_VAL_BOOL;
    val.data.boolean = v ? 1 : 0;
    return val;
}

static inline exprtk_value_t exprtk_val_vec(double *data, size_t size) {
    exprtk_value_t val = {0};
    val.type = EXPRTK_VAL_VECTOR;
    val.data.vector.data = data;
    val.data.vector.size = size;
    return val;
}

static inline exprtk_value_t exprtk_val_str(tstr_v v) {
    exprtk_value_t val = {0};
    val.type = EXPRTK_VAL_STRING;
    val.data.string = v;
    return val;
}

static inline exprtk_value_t exprtk_val_bytes(tstr_v v) {
    exprtk_value_t val = {0};
    val.type = EXPRTK_VAL_BYTES;
    val.data.bytes = v;
    return val;
}

static inline exprtk_value_t exprtk_val_uuid(turbo_uuid_t v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_UUID;
    val.data.uuid = v;
    return val;
}

static inline exprtk_value_t exprtk_val_datetime(turbo_datetime_t v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_DATETIME;
    val.data.datetime = v;
    return val;
}

static inline exprtk_value_t exprtk_val_date(exprtk_date_t v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_DATE;
    val.data.date = v;
    return val;
}

static inline exprtk_value_t exprtk_val_time(exprtk_time_t v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_TIME;
    val.data.time = v;
    return val;
}

static inline exprtk_value_t exprtk_val_duration(int64_t ms) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_DURATION;
    val.data.duration_ms = ms;
    return val;
}

static inline exprtk_value_t exprtk_val_decimal(exprtk_decimal_t v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_DECIMAL;
    val.data.decimal = v;
    return val;
}

static inline exprtk_value_t exprtk_val_bigint(tstr_v v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_BIGINT;
    val.data.bigint.text = v;
    return val;
}

static inline exprtk_value_t exprtk_val_money(exprtk_money_t v) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_MONEY;
    val.data.money = v;
    return val;
}

static inline exprtk_value_t exprtk_val_enum(tstr_v type_name, tstr_v symbol, int64_t value, int is_flags) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = is_flags ? EXPRTK_VAL_FLAGS : EXPRTK_VAL_ENUM;
    val.data.enum_val.type_name = type_name;
    val.data.enum_val.symbol = symbol;
    val.data.enum_val.value = value;
    val.data.enum_val.is_flags = is_flags ? 1 : 0;
    return val;
}

static inline exprtk_value_t exprtk_val_offset_datetime(turbo_datetime_t datetime,
                                                        int offset_minutes) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_OFFSET_DATETIME;
    val.data.offset_datetime.datetime = datetime;
    val.data.offset_datetime.offset_minutes = offset_minutes;
    return val;
}

static inline exprtk_value_t exprtk_val_typed_array(exprtk_typed_array_kind_t kind,
                                                    void *data, size_t count,
                                                    int heap_owned) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_TYPED_ARRAY;
    val.data.typed_array.kind = kind;
    val.data.typed_array.data = data;
    val.data.typed_array.count = count;
    val.data.typed_array.heap_owned = heap_owned;
    return val;
}

exprtk_value_t exprtk_typed_array_get_value(exprtk_value_t value, size_t index);

/* =========================================================================
 * Script value ownership
 *
 * Borrowed constructors above are valid only for the immediate call.  Values
 * crossing an env/container/task boundary must use copy/retain/release.  The
 * retain operation is for mem_buffer-backed scalar/vector payloads; containers
 * cross owner boundaries through copy_to_pool/copy_to_env.
 * ========================================================================= */

int exprtk_value_copy_to_pool(exprtk_value_t value, mem_pool_t *pool,
                              exprtk_value_t *out);
int exprtk_value_copy_to_env(exprtk_value_t value, exprtk_env_t *env,
                             exprtk_value_t *out);
exprtk_value_t exprtk_value_retain(exprtk_value_t value);
exprtk_value_t exprtk_value_borrow(exprtk_value_t value);
void exprtk_value_release_storage(exprtk_value_t *value);
void exprtk_value_destroy(exprtk_value_t *value);
void exprtk_values_destroy(exprtk_value_t *values, size_t count);

/* =========================================================================
 * Map helpers — O(1) hash table backed map
 *
 * Implementation lives in exprtk_map.c (needs MIR HTAB internals).
 * ========================================================================= */

exprtk_value_t  exprtk_val_map(void);
exprtk_value_t  exprtk_val_object(void);
int             exprtk_value_is_object_like(const exprtk_value_t *value);
exprtk_value_t  exprtk_map_get(const exprtk_value_t *map, const char *key);
int             exprtk_map_set(exprtk_value_t *map, const char *key, exprtk_value_t value);
int             exprtk_map_has(const exprtk_value_t *map, const char *key);
int             exprtk_map_delete(exprtk_value_t *map, const char *key);
size_t          exprtk_map_count(const exprtk_value_t *map);
void            exprtk_map_free(exprtk_value_t *map);

/* Returns a pointer to the value inside the hash table (for JIT direct memory access).
 * The pointer is stable as long as no insertions/deletions occur. */
exprtk_value_t *exprtk_map_get_ptr(const exprtk_value_t *map, const char *key);

/* Iteration: call exprtk_map_iter_begin, then loop while _next returns 1 */
typedef struct {
    void *htab;
    size_t pos;
    size_t bound;
} exprtk_map_iter_t;

exprtk_map_iter_t exprtk_map_iter_begin(const exprtk_value_t *map);
int               exprtk_map_iter_next(exprtk_map_iter_t *it, const char **key, exprtk_value_t *value);

/* =========================================================================
 * List helpers — heterogeneous array of exprtk_value_t
 * ========================================================================= */

static inline exprtk_value_t exprtk_val_list_empty(void) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_LIST;
    val.ownership = EXPRTK_VALUE_OWNED;
    val.data.list.items = NULL;
    val.data.list.count = 0;
    val.data.list.capacity = 0;
    val.data.list.heap_owned = 0;
    val.data.list.value_pool = NULL;
    return val;
}

static inline exprtk_value_t exprtk_val_set_empty(void) {
    exprtk_value_t val = exprtk_val_list_empty();
    val.type = EXPRTK_VAL_SET;
    return val;
}

static inline exprtk_value_t exprtk_val_list_ex(exprtk_value_t *items, size_t n, int heap_owned) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_LIST;
    val.ownership = heap_owned ? EXPRTK_VALUE_OWNED : EXPRTK_VALUE_BORROWED;
    val.data.list.items = items;
    val.data.list.count = n;
    val.data.list.capacity = n;
    val.data.list.heap_owned = heap_owned;
    val.data.list.value_pool = NULL;
    return val;
}

static inline exprtk_value_t exprtk_val_list(exprtk_value_t *items, size_t n) {
    return exprtk_val_list_ex(items, n, 1);
}

int exprtk_list_push(exprtk_value_t *list, exprtk_value_t item);

static inline exprtk_value_t exprtk_list_get(const exprtk_value_t *list, size_t idx) {
    if ((list->type != EXPRTK_VAL_LIST && list->type != EXPRTK_VAL_SET) ||
        idx >= list->data.list.count)
        return exprtk_val_num(0);
    return exprtk_value_borrow(list->data.list.items[idx]);
}

#endif /* EXPRTK_MODULE_H */
