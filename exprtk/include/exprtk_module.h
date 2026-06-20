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
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Module function signature & descriptor types
 * ========================================================================= */

typedef exprtk_value_t (*exprtk_builtin_fn)(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, mem_pool_t *arena);

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
    exprtk_value_t val;
    val.type = EXPRTK_VAL_NUMBER;
    val.data.number = v;
    return val;
}

static inline exprtk_value_t exprtk_val_int(int64_t v) {
    exprtk_value_t val;
    val.type = EXPRTK_VAL_INTEGER;
    val.data.integer = v;
    return val;
}

static inline exprtk_value_t exprtk_val_vec(double *data, size_t size) {
    exprtk_value_t val;
    val.type = EXPRTK_VAL_VECTOR;
    val.data.vector.data = data;
    val.data.vector.size = size;
    return val;
}

static inline exprtk_value_t exprtk_val_str(tstr_v v) {
    exprtk_value_t val;
    val.type = EXPRTK_VAL_STRING;
    val.data.string = v;
    return val;
}

/* =========================================================================
 * Map helpers — O(1) hash table backed map
 *
 * Implementation lives in exprtk_map.c (needs MIR HTAB internals).
 * ========================================================================= */

exprtk_value_t  exprtk_val_map(void);
exprtk_value_t  exprtk_map_get(const exprtk_value_t *map, const char *key);
void            exprtk_map_set(exprtk_value_t *map, const char *key, exprtk_value_t value);
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
    val.data.list.items = NULL;
    val.data.list.count = 0;
    val.data.list.capacity = 0;
    val.data.list.heap_owned = 0;
    return val;
}

static inline exprtk_value_t exprtk_val_list_ex(exprtk_value_t *items, size_t n, int heap_owned) {
    exprtk_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = EXPRTK_VAL_LIST;
    val.data.list.items = items;
    val.data.list.count = n;
    val.data.list.capacity = n;
    val.data.list.heap_owned = heap_owned;
    return val;
}

static inline exprtk_value_t exprtk_val_list(exprtk_value_t *items, size_t n) {
    return exprtk_val_list_ex(items, n, 1);
}

static inline void exprtk_list_push(exprtk_value_t *list, exprtk_value_t item) {
    if (list->type != EXPRTK_VAL_LIST) return;
    if (list->data.list.count >= list->data.list.capacity) {
        size_t new_cap = list->data.list.capacity ? list->data.list.capacity * 2 : 4;
        exprtk_value_t *new_items = NULL;
        if (list->data.list.heap_owned) {
            new_items = (exprtk_value_t*)realloc(
                list->data.list.items, new_cap * sizeof(exprtk_value_t));
        } else {
            new_items = (exprtk_value_t*)malloc(new_cap * sizeof(exprtk_value_t));
            if (new_items && list->data.list.items && list->data.list.count > 0) {
                memcpy(new_items, list->data.list.items,
                       list->data.list.count * sizeof(exprtk_value_t));
            }
        }
        if (!new_items) return;
        list->data.list.items = new_items;
        list->data.list.capacity = new_cap;
        list->data.list.heap_owned = 1;
    }
    list->data.list.items[list->data.list.count++] = item;
}

static inline exprtk_value_t exprtk_list_get(const exprtk_value_t *list, size_t idx) {
    if (list->type != EXPRTK_VAL_LIST || idx >= list->data.list.count)
        return exprtk_val_num(0);
    return list->data.list.items[idx];
}

#endif /* EXPRTK_MODULE_H */
