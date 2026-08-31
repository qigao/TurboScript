/**
 * @file exprtk_value.c
 * @brief Ownership operations for view-backed scalar values.
 */

#include "exprtk_module.h"
#include <turbostl/vec.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int exprtk_copy_view(mem_pool_t *pool, vstr source, int nul_terminate,
                            mem_buffer_t **out_storage, vstr *out_view) {
    size_t capacity;
    mem_buffer_t *buffer;
    char *data;

    if (!pool || !out_storage || !out_view) return -1;
    if (!source.data && source.len != 0) return -1;
    if (source.len == 0) {
        *out_storage = NULL;
        *out_view = vstr_from_buf("", 0);
        return 0;
    }
    if (nul_terminate && source.len == SIZE_MAX) return -1;

    capacity = source.len + (nul_terminate ? 1U : 0U);
    buffer = mem_get_buffer(pool, capacity);
    if (!buffer) return -1;
    data = mem_buffer_data(buffer);
    memcpy(data, source.data, source.len);
    if (nul_terminate) data[source.len] = '\0';
    mem_set_used(buffer, capacity);
    *out_storage = buffer;
    *out_view = vstr_from_buf(data, source.len);
    return 0;
}

static int exprtk_copy_payload(mem_pool_t *pool, const void *source, size_t size,
                               mem_buffer_t **out_storage, void **out_data) {
    mem_buffer_t *buffer;
    void *data;

    if (!pool || !out_storage || !out_data || (!source && size != 0)) return -1;
    if (size == 0) {
        *out_storage = NULL;
        *out_data = NULL;
        return 0;
    }
    buffer = mem_get_buffer(pool, size);
    if (!buffer) return -1;
    data = mem_buffer_data(buffer);
    memcpy(data, source, size);
    mem_set_used(buffer, size);
    *out_storage = buffer;
    *out_data = data;
    return 0;
}

static int exprtk_value_uses_managed_payload(exprtk_value_type_t type) {
    switch (type) {
        case EXPRTK_VAL_STRING:
        case EXPRTK_VAL_BYTES:
        case EXPRTK_VAL_BIGINT:
        case EXPRTK_VAL_ENUM:
        case EXPRTK_VAL_FLAGS:
        case EXPRTK_VAL_VECTOR:
        case EXPRTK_VAL_TYPED_ARRAY:
            return 1;
        default:
            return 0;
    }
}

exprtk_value_t exprtk_value_retain(exprtk_value_t value) {
    /* A function value is a reference holder: retaining the value must retain
     * the captured scope so copies stay alive independently of the source. */
    if (value.type == EXPRTK_VAL_FUNCTION) {
        if (value.data.function.closure_env) exprtk_env_retain(value.data.function.closure_env);
        value.ownership = EXPRTK_VALUE_OWNED;
        return value;
    }
    if (!exprtk_value_uses_managed_payload(value.type)) {
        value.storage = NULL;
        value.storage_aux = NULL;
        value.ownership = EXPRTK_VALUE_BORROWED;
        return value;
    }
    if (value.storage) mem_buffer_retain(value.storage);
    if (value.storage_aux) mem_buffer_retain(value.storage_aux);
    value.ownership = EXPRTK_VALUE_OWNED;
    return value;
}

exprtk_value_t exprtk_value_borrow(exprtk_value_t value) {
    if (!exprtk_value_uses_managed_payload(value.type)) {
        value.storage = NULL;
        value.storage_aux = NULL;
    }
    value.ownership = EXPRTK_VALUE_BORROWED;
    return value;
}

void exprtk_value_release_storage(exprtk_value_t *value) {
    if (!value) return;
    if (exprtk_value_uses_managed_payload(value->type) &&
        value->ownership == EXPRTK_VALUE_OWNED) {
        if (value->storage) mem_buffer_release(value->storage);
        if (value->storage_aux) mem_buffer_release(value->storage_aux);
    }
    value->storage = NULL;
    value->storage_aux = NULL;
    value->ownership = EXPRTK_VALUE_BORROWED;
}

int exprtk_value_copy_to_pool(exprtk_value_t value, mem_pool_t *pool,
                              exprtk_value_t *out) {
    exprtk_value_t copied;
    size_t i;

    if (!pool || !out) return -1;
    copied = value;
    copied.storage = NULL;
    copied.storage_aux = NULL;

    if (exprtk_value_uses_managed_payload(value.type) && value.storage &&
        mem_buffer_pool(value.storage) == pool &&
        (!value.storage_aux || mem_buffer_pool(value.storage_aux) == pool)) {
        *out = exprtk_value_retain(value);
        return 0;
    }

    switch (value.type) {
        case EXPRTK_VAL_STRING:
            if (exprtk_copy_view(pool, value.data.string, 1, &copied.storage,
                                 &copied.data.string) != 0)
                return -1;
            break;
        case EXPRTK_VAL_BYTES:
            if (exprtk_copy_view(pool, value.data.bytes, 0, &copied.storage,
                                 &copied.data.bytes) != 0)
                return -1;
            break;
        case EXPRTK_VAL_BIGINT:
            if (exprtk_copy_view(pool, value.data.bigint.text, 1, &copied.storage,
                                 &copied.data.bigint.text) != 0)
                return -1;
            break;
        case EXPRTK_VAL_ENUM:
        case EXPRTK_VAL_FLAGS:
            if (exprtk_copy_view(pool, value.data.enum_val.type_name, 1,
                                 &copied.storage,
                                 &copied.data.enum_val.type_name) != 0)
                return -1;
            if (exprtk_copy_view(pool, value.data.enum_val.symbol, 1,
                                 &copied.storage_aux,
                                 &copied.data.enum_val.symbol) != 0) {
                exprtk_value_release_storage(&copied);
                return -1;
            }
            break;
        case EXPRTK_VAL_VECTOR:
            copied.data.vector.data = NULL;
            copied.data.vector.size = 0;
            if (value.data.vector.size > 0) {
                if (!value.data.vector.data) return -1;
                if (value.data.vector.size > SIZE_MAX / sizeof(double)) return -1;
                if (exprtk_copy_payload(
                        pool, value.data.vector.data,
                        value.data.vector.size * sizeof(double), &copied.storage,
                        (void **)&copied.data.vector.data) != 0)
                    return -1;
                copied.data.vector.size = value.data.vector.size;
            }
            break;
        case EXPRTK_VAL_TYPED_ARRAY: {
            size_t element_size;
            switch (value.data.typed_array.kind) {
                case EXPRTK_TYPED_I32: element_size = sizeof(int32_t); break;
                case EXPRTK_TYPED_I64: element_size = sizeof(int64_t); break;
                case EXPRTK_TYPED_F32: element_size = sizeof(float); break;
                case EXPRTK_TYPED_F64: element_size = sizeof(double); break;
                default: return -1;
            }
            copied.data.typed_array.data = NULL;
            copied.data.typed_array.heap_owned = 0;
            if (value.data.typed_array.count > 0) {
                if (!value.data.typed_array.data) return -1;
                if (value.data.typed_array.count > SIZE_MAX / element_size) return -1;
                if (exprtk_copy_payload(
                        pool, value.data.typed_array.data,
                        value.data.typed_array.count * element_size, &copied.storage,
                        &copied.data.typed_array.data) != 0)
                    return -1;
            }
            break;
        }
        case EXPRTK_VAL_LIST:
        case EXPRTK_VAL_SET:
            copied = value.type == EXPRTK_VAL_SET ? exprtk_val_set_empty()
                                                   : exprtk_val_list_empty();
            for (i = 0; i < value.data.list.count; ++i) {
                if (exprtk_list_push(&copied,
                                     exprtk_value_borrow(value.data.list.items[i])) != 0) {
                    exprtk_value_destroy(&copied);
                    return -1;
                }
            }
            break;
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT: {
            exprtk_map_iter_t iterator;
            const char *key;
            exprtk_value_t child;
            copied = value.type == EXPRTK_VAL_OBJECT ? exprtk_val_object()
                                                     : exprtk_val_map();
            if (!copied.data.map.htab) return -1;
            iterator = exprtk_map_iter_begin(&value);
            while (exprtk_map_iter_next(&iterator, &key, &child)) {
                if (exprtk_map_set(&copied, key, child) != 0) {
                    exprtk_value_destroy(&copied);
                    return -1;
                }
            }
            break;
        }
        default:
            copied = exprtk_value_retain(value);
            break;
    }

    if (copied.storage || copied.storage_aux)
        copied.ownership = EXPRTK_VALUE_OWNED;

    *out = copied;
    return 0;
}

int exprtk_value_copy_to_env(exprtk_value_t value, exprtk_env_t *env,
                             exprtk_value_t *out) {
    if (!env || !out) return -1;
    if (exprtk_value_copy_to_pool(value, &env->arena, out) != 0) {
        env->aborted = 1;
        snprintf(env->error_msg, sizeof(env->error_msg),
                 "failed to allocate owned value payload");
        return -1;
    }
    return 0;
}

static mem_pool_t *exprtk_list_pool(exprtk_value_t *list) {
    mem_pool_t *pool;

    if (list->data.list.value_pool) return list->data.list.value_pool;
    pool = (mem_pool_t *)malloc(sizeof(*pool));
    if (!pool) return NULL;
    if (mem_init(pool, 0) != 0) {
        free(pool);
        return NULL;
    }
    list->data.list.value_pool = pool;
    return pool;
}

int exprtk_list_push(exprtk_value_t *list, exprtk_value_t item) {
    mem_pool_t *pool;
    exprtk_value_t stored;
    vec_t vec = {0};
    size_t new_capacity;

    if (!list || (list->type != EXPRTK_VAL_LIST && list->type != EXPRTK_VAL_SET))
        return -1;
    pool = exprtk_list_pool(list);
    if (!pool || exprtk_value_copy_to_pool(item, pool, &stored) != 0) return -1;

    if (list->data.list.count < list->data.list.capacity) {
        list->data.list.items[list->data.list.count++] = stored;
        return 0;
    }
    if (list->data.list.capacity > SIZE_MAX / 2U) {
        exprtk_value_destroy(&stored);
        return -1;
    }
    new_capacity = list->data.list.capacity ? list->data.list.capacity * 2U : 4U;

    if (list->data.list.heap_owned) {
        vec.data = list->data.list.items;
        vec.size = list->data.list.count;
        vec.capacity = list->data.list.capacity;
        vec.elem_size = sizeof(exprtk_value_t);
        vec.elem_stride = sizeof(exprtk_value_t);
        vec.elem_align = _Alignof(exprtk_value_t);
        vec.element_limit = SIZE_MAX / sizeof(exprtk_value_t);
        vec.initialized = true;
    } else {
        if (vec_init_bytes(&vec, sizeof(exprtk_value_t), _Alignof(exprtk_value_t),
                           SIZE_MAX / sizeof(exprtk_value_t)) != STL_OK) {
            exprtk_value_destroy(&stored);
            return -1;
        }
        if (vec_reserve(&vec, new_capacity) != STL_OK) {
            vec_destroy(&vec);
            exprtk_value_destroy(&stored);
            return -1;
        }
        if (list->data.list.items && list->data.list.count > 0) {
            memcpy(vec.data, list->data.list.items,
                   list->data.list.count * sizeof(exprtk_value_t));
            vec.size = list->data.list.count;
        }
    }

    if (vec_reserve(&vec, new_capacity) != STL_OK ||
        vec_push(&vec, &stored) != STL_OK) {
        if (!list->data.list.heap_owned) vec_destroy(&vec);
        exprtk_value_destroy(&stored);
        return -1;
    }
    list->data.list.items = (exprtk_value_t *)vec.data;
    list->data.list.count = vec.size;
    list->data.list.capacity = vec.capacity;
    list->data.list.heap_owned = 1;
    list->ownership = EXPRTK_VALUE_OWNED;
    return 0;
}

void exprtk_value_destroy(exprtk_value_t *value) {
    size_t i;

    if (!value) return;
    if (value->ownership == EXPRTK_VALUE_BORROWED &&
        (value->type == EXPRTK_VAL_MAP || value->type == EXPRTK_VAL_OBJECT ||
         value->type == EXPRTK_VAL_LIST || value->type == EXPRTK_VAL_SET)) {
        memset(value, 0, sizeof(*value));
        value->type = EXPRTK_VAL_NULL;
        return;
    }
    exprtk_value_release_storage(value);
    if (value->type == EXPRTK_VAL_FUNCTION) {
        if (value->ownership == EXPRTK_VALUE_OWNED && value->data.function.closure_env) {
            exprtk_env_release(value->data.function.closure_env);
            value->data.function.closure_env = NULL;
        }
        memset(value, 0, sizeof(*value));
        value->type = EXPRTK_VAL_NULL;
        return;
    }
    if (value->type == EXPRTK_VAL_MAP || value->type == EXPRTK_VAL_OBJECT) {
        exprtk_map_free(value);
    } else if (value->type == EXPRTK_VAL_LIST || value->type == EXPRTK_VAL_SET) {
        for (i = 0; i < value->data.list.count; ++i)
            exprtk_value_destroy(&value->data.list.items[i]);
        if (value->data.list.heap_owned) {
            vec_t vec = {
                .data = value->data.list.items,
                .size = value->data.list.count,
                .capacity = value->data.list.capacity,
                .elem_size = sizeof(exprtk_value_t),
                .elem_stride = sizeof(exprtk_value_t),
                .elem_align = _Alignof(exprtk_value_t),
                .element_limit = SIZE_MAX / sizeof(exprtk_value_t),
                .initialized = true,
            };
            vec_destroy(&vec);
        }
        if (value->data.list.value_pool) {
            mem_destroy(value->data.list.value_pool);
            free(value->data.list.value_pool);
        }
    } else if (value->type == EXPRTK_VAL_TYPED_ARRAY &&
               value->data.typed_array.heap_owned) {
        free(value->data.typed_array.data);
    }
    memset(value, 0, sizeof(*value));
    value->type = EXPRTK_VAL_NULL;
}

void exprtk_values_destroy(exprtk_value_t *values, size_t count) {
    if (!values) return;
    for (size_t i = 0; i < count; ++i)
        exprtk_value_destroy(&values[i]);
}
