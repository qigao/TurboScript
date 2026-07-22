/**
 * @file exprtk_runtime.c
 * @brief runtime environment, value ownership, and symbol table support
 */

#include "exprtk.h"
#include "exprtk_internal.h"
#include "exprtk_class.h"
#include "mir-htab.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
// MIR allocator wrapper for standard malloc/free
static void* mir_std_malloc(size_t size, void *user_data) {
    (void)user_data;
    return malloc(size);
}

static void* mir_std_calloc(size_t num, size_t size, void *user_data) {
    (void)user_data;
    return calloc(num, size);
}

static void* mir_std_realloc(void *ptr, size_t old_size, size_t new_size, void *user_data) {
    (void)user_data;
    (void)old_size;
    return realloc(ptr, new_size);
}

static void mir_std_free(void *ptr, void *user_data) {
    (void)user_data;
    free(ptr);
}

static const struct MIR_alloc mir_std_alloc_struct = {
    mir_std_malloc,
    mir_std_calloc,
    mir_std_realloc,
    mir_std_free,
    NULL
};

static MIR_alloc_t mir_std_alloc = (MIR_alloc_t)&mir_std_alloc_struct;

// Hash table entry for variables
typedef struct {
    char *name;
    exprtk_value_t value;
    int is_constant;
    int is_borrowed;
} exprtk_var_entry_t;

// Define hash table type for variables
DEF_HTAB(exprtk_var_entry_t)

// Hash table helper functions for variable storage
static htab_hash_t var_hash(exprtk_var_entry_t entry, void *arg) {
    (void)arg;
    const char *s = entry.name;
    htab_hash_t h = 0;
    while (*s) h = h * 31 + (unsigned char)*s++;
    return h;
}

static int var_eq(exprtk_var_entry_t e1, exprtk_var_entry_t e2, void *arg) {
    (void)arg;
    return strcmp(e1.name, e2.name) == 0;
}

typedef struct {
    void **items;
    size_t count;
    size_t capacity;
} exprtk_ptr_set_t;

typedef struct {
    exprtk_ptr_set_t lists;
    exprtk_ptr_set_t maps;
    exprtk_ptr_set_t classes;
    exprtk_ptr_set_t instances;
    mem_pool_t *owner_arena;
} exprtk_release_state_t;

static int ptr_set_mark_seen(exprtk_ptr_set_t *set, void *ptr) {
    if (!set || !ptr) return 0;

    for (size_t i = 0; i < set->count; ++i) {
        if (set->items[i] == ptr) return 0;
    }

    if (set->count == set->capacity) {
        size_t new_capacity = set->capacity ? set->capacity * 2 : 8;
        void **new_items = (void**)realloc(set->items, new_capacity * sizeof(void*));
        if (!new_items) return 0;
        set->items = new_items;
        set->capacity = new_capacity;
    }

    set->items[set->count++] = ptr;
    return 1;
}

static void exprtk_release_state_destroy(exprtk_release_state_t *state) {
    if (!state) return;

    free(state->lists.items);
    free(state->maps.items);
    free(state->classes.items);
    free(state->instances.items);
    state->lists.items = NULL;
    state->maps.items = NULL;
    state->classes.items = NULL;
    state->instances.items = NULL;
    state->owner_arena = NULL;
    state->lists.count = 0;
    state->maps.count = 0;
    state->classes.count = 0;
    state->instances.count = 0;
    state->lists.capacity = 0;
    state->maps.capacity = 0;
    state->classes.capacity = 0;
    state->instances.capacity = 0;
}

static void exprtk_release_value(exprtk_value_t *value, exprtk_release_state_t *state);

typedef struct {
    exprtk_instance_t *instance;
    exprtk_env_t *env;
} exprtk_instance_clone_ctx_t;

typedef struct {
    exprtk_class_t *source;
    exprtk_class_t *klass;
    exprtk_env_t *env;
} exprtk_class_clone_ctx_t;

exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value, exprtk_env_t *dst_env);

static void exprtk_clone_instance_field(const char *name, exprtk_value_t *value,
                                        void *user_data) {
    exprtk_instance_clone_ctx_t *ctx = (exprtk_instance_clone_ctx_t*)user_data;
    if (!ctx || !ctx->instance || !ctx->env || !name || !value) return;

    exprtk_value_t cloned = exprtk_value_clone_to_env(*value, ctx->env);
    exprtk_instance_set_field(ctx->instance, name, cloned);
}

static void exprtk_clone_class_static_field(const char *name, exprtk_value_t *value,
                                            void *user_data) {
    exprtk_class_clone_ctx_t *ctx = (exprtk_class_clone_ctx_t*)user_data;
    if (!ctx || !ctx->source || !ctx->klass || !ctx->env || !name || !value) return;

    exprtk_value_t cloned;
    if (value->type == EXPRTK_VAL_CLASS &&
        value->data.class_val.klass == ctx->source) {
        cloned = exprtk_val_class(ctx->klass);
    } else {
        cloned = exprtk_value_clone_to_env(*value, ctx->env);
    }

    exprtk_class_set_static_field(ctx->klass, name, cloned);
}

static void exprtk_clone_class_static_fields(exprtk_class_t *source,
                                             exprtk_class_t *klass,
                                             exprtk_env_t *env) {
    if (!source || !klass || !env) return;

    if (source->prototype && klass->prototype) {
        exprtk_clone_class_static_fields(source->prototype, klass->prototype, env);
    }

    exprtk_class_clone_ctx_t ctx = {
        .source = source,
        .klass = klass,
        .env = env
    };
    exprtk_class_foreach_named_static_field(source, exprtk_clone_class_static_field, &ctx);
}

exprtk_value_t exprtk_value_clone_to_env(exprtk_value_t value, exprtk_env_t *dst_env) {
    if (!dst_env) return value;

    switch (value.type) {
        case EXPRTK_VAL_STRING: {
            if (!value.data.string.data) return value;
            char *buf = (char*)mem_alloc(&dst_env->arena, value.data.string.len + 1);
            if (!buf) {
                tstr_v empty = {0};
                return exprtk_val_str(empty);
            }
            memcpy(buf, value.data.string.data, value.data.string.len);
            buf[value.data.string.len] = '\0';
            tstr_v copied;
            copied.data = buf;
            copied.len = value.data.string.len;
            return exprtk_val_str(copied);
        }
        case EXPRTK_VAL_BIGINT: {
            if (!value.data.bigint.text.data) return value;
            char *buf = (char*)mem_alloc(&dst_env->arena, value.data.bigint.text.len + 1);
            if (!buf) return exprtk_val_bigint(tstr_v_from_cstr(""));
            memcpy(buf, value.data.bigint.text.data, value.data.bigint.text.len);
            buf[value.data.bigint.text.len] = '\0';
            return exprtk_val_bigint(tstr_v_from_buf(buf, value.data.bigint.text.len));
        }
        case EXPRTK_VAL_ENUM:
        case EXPRTK_VAL_FLAGS: {
            char *type_buf = NULL;
            char *symbol_buf = NULL;
            tstr_v type_name = {0};
            tstr_v symbol = {0};
            if (value.data.enum_val.type_name.data) {
                type_buf = (char*)mem_alloc(&dst_env->arena, value.data.enum_val.type_name.len + 1);
                if (type_buf) {
                    memcpy(type_buf, value.data.enum_val.type_name.data, value.data.enum_val.type_name.len);
                    type_buf[value.data.enum_val.type_name.len] = '\0';
                    type_name = tstr_v_from_buf(type_buf, value.data.enum_val.type_name.len);
                }
            }
            if (value.data.enum_val.symbol.data) {
                symbol_buf = (char*)mem_alloc(&dst_env->arena, value.data.enum_val.symbol.len + 1);
                if (symbol_buf) {
                    memcpy(symbol_buf, value.data.enum_val.symbol.data, value.data.enum_val.symbol.len);
                    symbol_buf[value.data.enum_val.symbol.len] = '\0';
                    symbol = tstr_v_from_buf(symbol_buf, value.data.enum_val.symbol.len);
                }
            }
            return exprtk_val_enum(type_name, symbol, value.data.enum_val.value,
                                   value.type == EXPRTK_VAL_FLAGS);
        }
        case EXPRTK_VAL_BYTES: {
            if (!value.data.bytes.data) return value;
            char *buf = (char*)mem_alloc(&dst_env->arena, value.data.bytes.len);
            if (!buf && value.data.bytes.len > 0) {
                tstr_v empty = {0};
                return exprtk_val_bytes(empty);
            }
            if (value.data.bytes.len > 0) memcpy(buf, value.data.bytes.data, value.data.bytes.len);
            tstr_v copied;
            copied.data = buf;
            copied.len = value.data.bytes.len;
            return exprtk_val_bytes(copied);
        }
        case EXPRTK_VAL_VECTOR: {
            if (!value.data.vector.data || value.data.vector.size == 0) return value;
            double *data = (double*)mem_alloc(&dst_env->arena, value.data.vector.size * sizeof(double));
            if (!data) return exprtk_val_vec(NULL, 0);
            memcpy(data, value.data.vector.data, value.data.vector.size * sizeof(double));
            return exprtk_val_vec(data, value.data.vector.size);
        }
        case EXPRTK_VAL_TYPED_ARRAY: {
            size_t elem_size = 0;
            void *data;
            if (!value.data.typed_array.data || value.data.typed_array.count == 0) return value;
            switch (value.data.typed_array.kind) {
                case EXPRTK_TYPED_I32: elem_size = sizeof(int32_t); break;
                case EXPRTK_TYPED_I64: elem_size = sizeof(int64_t); break;
                case EXPRTK_TYPED_F32: elem_size = sizeof(float); break;
                case EXPRTK_TYPED_F64: elem_size = sizeof(double); break;
                default: return exprtk_val_typed_array(value.data.typed_array.kind, NULL, 0, 0);
            }
            data = mem_alloc(&dst_env->arena, value.data.typed_array.count * elem_size);
            if (!data) return exprtk_val_typed_array(value.data.typed_array.kind, NULL, 0, 0);
            memcpy(data, value.data.typed_array.data, value.data.typed_array.count * elem_size);
            return exprtk_val_typed_array(value.data.typed_array.kind, data,
                                          value.data.typed_array.count, 0);
        }
        case EXPRTK_VAL_LIST:
        case EXPRTK_VAL_SET: {
            exprtk_value_t cloned = value.type == EXPRTK_VAL_SET ? exprtk_val_set_empty()
                                                                 : exprtk_val_list_empty();
            if (!value.data.list.items || value.data.list.count == 0) return cloned;

            exprtk_value_t *items =
                (exprtk_value_t*)mem_alloc(&dst_env->arena, value.data.list.count * sizeof(exprtk_value_t));
            if (!items) return cloned;

            for (size_t i = 0; i < value.data.list.count; ++i) {
                items[i] = exprtk_value_clone_to_env(value.data.list.items[i], dst_env);
            }
            cloned = exprtk_val_list_ex(items, value.data.list.count, 0);
            if (value.type == EXPRTK_VAL_SET) cloned.type = EXPRTK_VAL_SET;
            return cloned;
        }
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT: {
            exprtk_value_t cloned = value.type == EXPRTK_VAL_OBJECT
                ? exprtk_val_object()
                : exprtk_val_map();
            exprtk_map_iter_t it = exprtk_map_iter_begin(&value);
            const char *key;
            exprtk_value_t child;
            while (exprtk_map_iter_next(&it, &key, &child)) {
                exprtk_value_t cloned_child = exprtk_value_clone_to_env(child, dst_env);
                exprtk_map_set(&cloned, key, cloned_child);
            }
            return cloned;
        }
        case EXPRTK_VAL_CLASS: {
            exprtk_class_t *src = value.data.class_val.klass;
            if (!src || src->arena == &dst_env->arena) return value;

            exprtk_class_t *copy = exprtk_class_clone_to_arena(src, &dst_env->arena);
            if (!copy) {
                exprtk_value_t null_value;
                memset(&null_value, 0, sizeof(null_value));
                null_value.type = EXPRTK_VAL_NULL;
                return null_value;
            }

            exprtk_clone_class_static_fields(src, copy, dst_env);
            return exprtk_val_class(copy);
        }
        case EXPRTK_VAL_INSTANCE: {
            exprtk_instance_t *src = value.data.instance_val.instance;
            if (!src || src->arena == &dst_env->arena) return value;

            exprtk_class_t *klass = exprtk_class_clone_to_arena(src->klass, &dst_env->arena);
            if (!klass) {
                exprtk_value_t null_value;
                memset(&null_value, 0, sizeof(null_value));
                null_value.type = EXPRTK_VAL_NULL;
                return null_value;
            }
            exprtk_clone_class_static_fields(src->klass, klass, dst_env);

            exprtk_instance_t *copy = exprtk_instance_create(klass, &dst_env->arena);
            if (!copy) {
                exprtk_value_t null_value;
                memset(&null_value, 0, sizeof(null_value));
                null_value.type = EXPRTK_VAL_NULL;
                return null_value;
            }

            exprtk_instance_clone_ctx_t ctx = { .instance = copy, .env = dst_env };
            exprtk_instance_foreach_named_field(src, exprtk_clone_instance_field, &ctx);
            return exprtk_val_instance(copy);
        }
        case EXPRTK_VAL_BOUND_METHOD: {
            exprtk_value_t instance_value =
                exprtk_val_instance(value.data.bound_method_val.instance);
            exprtk_value_t cloned_instance =
                exprtk_value_clone_to_env(instance_value, dst_env);
            if (cloned_instance.type != EXPRTK_VAL_INSTANCE) return cloned_instance;

            exprtk_func_t *method = value.data.bound_method_val.method;
            if (method && method->owner_class &&
                method->owner_class != cloned_instance.data.instance_val.instance->klass) {
                exprtk_func_t *cloned_method =
                    exprtk_class_lookup_method_arity(
                        cloned_instance.data.instance_val.instance->klass,
                        method->name, 0, method->data.script.arg_count);
                if (cloned_method) method = cloned_method;
            }

            return exprtk_val_bound_method(cloned_instance.data.instance_val.instance,
                                           method);
        }
        default:
            return value;
    }
}

static int exprtk_value_is_reference_type(exprtk_value_t value) {
    switch (value.type) {
        case EXPRTK_VAL_CLASS:
        case EXPRTK_VAL_INSTANCE:
        case EXPRTK_VAL_BOUND_METHOD:
        case EXPRTK_VAL_FUNCTION:
        case EXPRTK_VAL_COROUTINE:
            return 1;
        default:
            return 0;
    }
}

static exprtk_value_t exprtk_value_store_to_env(exprtk_value_t value, exprtk_env_t *dst_env) {
    if (exprtk_value_is_reference_type(value)) return value;

    /* Containers own their storage but retain object identity for nested reference
     * values. This is required for collections of observers, callbacks, and class
     * instances; deep-cloning those elements would change identity and can bind
     * them to the lifetime of a temporary method environment. */
    if (value.type == EXPRTK_VAL_LIST || value.type == EXPRTK_VAL_SET) {
        exprtk_value_t stored = value.type == EXPRTK_VAL_SET ? exprtk_val_set_empty()
                                                             : exprtk_val_list_empty();
        if (!value.data.list.items || value.data.list.count == 0) return stored;
        stored.data.list.items = (exprtk_value_t *)mem_alloc_array(
            &dst_env->arena, sizeof(*stored.data.list.items), value.data.list.count);
        if (!stored.data.list.items) return stored;
        stored.data.list.count = value.data.list.count;
        stored.data.list.capacity = value.data.list.count;
        for (size_t i = 0; i < value.data.list.count; ++i)
            stored.data.list.items[i] = exprtk_value_store_to_env(
                value.data.list.items[i], dst_env);
        return stored;
    }
    if (exprtk_value_is_object_like(&value)) {
        exprtk_value_t stored = value.type == EXPRTK_VAL_OBJECT ? exprtk_val_object()
                                                                : exprtk_val_map();
        exprtk_map_iter_t it = exprtk_map_iter_begin(&value);
        const char *key;
        exprtk_value_t child;
        while (exprtk_map_iter_next(&it, &key, &child))
            exprtk_map_set(&stored, key, exprtk_value_store_to_env(child, dst_env));
        return stored;
    }
    return exprtk_value_clone_to_env(value, dst_env);
}

static void exprtk_env_release_stored_value(exprtk_env_t *env, exprtk_value_t *value) {
    exprtk_release_state_t state;

    if (!env || !value) return;
    if (exprtk_value_is_reference_type(*value)) return;

    memset(&state, 0, sizeof(state));
    state.owner_arena = &env->arena;
    exprtk_release_value(value, &state);
    exprtk_release_state_destroy(&state);
}

static void exprtk_env_release_entry_value(exprtk_env_t *env, exprtk_var_entry_t *entry) {
    if (!entry || entry->is_borrowed) return;
    exprtk_env_release_stored_value(env, &entry->value);
}

void exprtk_env_set_local(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    if (!env || !name) return;

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;

    if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        exprtk_value_t stored = exprtk_value_store_to_env(value, env);
        exprtk_env_release_entry_value(env, &result);
        result.value = stored;
        result.is_borrowed = 0;
        HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        exprtk_value_t stored = exprtk_value_store_to_env(value, env);
        exprtk_var_entry_t new_entry = {
            .name = strdup(name),
            .value = stored,
            .is_constant = 0,
            .is_borrowed = 0
        };
        HTAB_OP(exprtk_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

static void exprtk_env_set_local_cloned(exprtk_env_t *env, const char *name,
                                        exprtk_value_t value) {
    if (!env || !name) return;

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;

    if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        exprtk_value_t stored = exprtk_value_clone_to_env(value, env);
        exprtk_env_release_entry_value(env, &result);
        result.value = stored;
        result.is_borrowed = 0;
        HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        exprtk_value_t stored = exprtk_value_clone_to_env(value, env);
        exprtk_var_entry_t new_entry = {
            .name = strdup(name),
            .value = stored,
            .is_constant = 0,
            .is_borrowed = 0
        };
        HTAB_OP(exprtk_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

void exprtk_env_set_local_borrowed(exprtk_env_t *env, const char *name,
                                          exprtk_value_t value) {
    if (!env || !name) return;

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;

    if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        if (result.is_constant) return;
        exprtk_env_release_entry_value(env, &result);
        result.value = value;
        result.is_borrowed = 1;
        HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    } else {
        exprtk_var_entry_t new_entry = {
            .name = strdup(name),
            .value = value,
            .is_constant = 0,
            .is_borrowed = 1
        };
        HTAB_OP(exprtk_var_entry_t, do)(htab, new_entry, HTAB_INSERT, &result);
    }
}

void exprtk_env_set(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    if (!env || !name) return;

    exprtk_env_t *curr_env = env;
    while (curr_env) {
        HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_env->vars;
        exprtk_var_entry_t key = { .name = (char*)name };
        exprtk_var_entry_t result;

        if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
            if (result.is_constant) return;
            exprtk_value_t stored = exprtk_value_store_to_env(value, curr_env);
            exprtk_env_release_entry_value(curr_env, &result);
            result.value = stored;
            result.is_borrowed = 0;
            HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
            return;
        }
        curr_env = curr_env->parent;
    }

    // Not found in any scope, create in current scope
    exprtk_env_set_local(env, name, value);
}


void eval_destructure(exprtk_node_t *target, exprtk_value_t rhs, exprtk_env_t *env, int is_constant) {
    if (!target || !env) return;

    if (target->type == EXPRTK_NODE_VARIABLE) {
        if (is_constant) exprtk_env_set_constant(env, target->data.variable.name, rhs);
        else exprtk_env_set_local(env, target->data.variable.name, rhs);
    } else if (target->type == EXPRTK_NODE_VECTOR) {
        if (rhs.type != EXPRTK_VAL_VECTOR) return;
        size_t rhs_idx = 0;
        for (size_t i = 0; i < target->data.vector.count; ++i) {
            exprtk_node_t *el = target->data.vector.elements[i];
            if (el->type == EXPRTK_NODE_SPREAD) {
                exprtk_node_t *child = el->data.spread.child;
                if (child->type == EXPRTK_NODE_VARIABLE) {
                    size_t rest_sz = (rhs_idx < rhs.data.vector.size) ? (rhs.data.vector.size - rhs_idx) : 0;
                    double *rest_data = (double*)mem_alloc(target->arena, rest_sz * sizeof(double));
                    if (rest_sz > 0) {
                        memcpy(rest_data, rhs.data.vector.data + rhs_idx, rest_sz * sizeof(double));
                    }
                    exprtk_value_t rest_val = exprtk_val_vec(rest_data, rest_sz);
                    if (is_constant) exprtk_env_set_constant(env, child->data.variable.name, rest_val);
                    else exprtk_env_set(env, child->data.variable.name, rest_val);
                    rhs_idx = rhs.data.vector.size;
                }
            } else if (el->type == EXPRTK_NODE_NULL) {
                rhs_idx++;
            } else {
                exprtk_value_t val = (rhs_idx < rhs.data.vector.size) ?
                    exprtk_val_num(rhs.data.vector.data[rhs_idx++]) : exprtk_val_num(0);
                eval_destructure(el, val, env, is_constant);
            }
        }
    } else if (target->type == EXPRTK_NODE_MAP_LITERAL) {
        if (!exprtk_value_is_object_like(&rhs)) return;
        exprtk_node_t *rest_node = NULL;
        for (size_t i = 0; i < target->data.map_literal.count; ++i) {
            char *key = target->data.map_literal.keys[i];
            if (key) {
                exprtk_value_t prop_val = exprtk_map_get(&rhs, key);
                eval_destructure(target->data.map_literal.values[i], prop_val, env, is_constant);
            } else {
                rest_node = target->data.map_literal.values[i];
            }
        }
        if (rest_node) {
            exprtk_value_t rest_map = exprtk_val_map();
            exprtk_map_iter_t it = exprtk_map_iter_begin(&rhs);
            const char *rhs_key;
            exprtk_value_t rhs_val;
            while (exprtk_map_iter_next(&it, &rhs_key, &rhs_val)) {
                int matched = 0;
                for (size_t i = 0; i < target->data.map_literal.count; ++i) {
                    if (target->data.map_literal.keys[i] && strcmp(target->data.map_literal.keys[i], rhs_key) == 0) {
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    exprtk_map_set(&rest_map, rhs_key, rhs_val);
                }
            }
            eval_destructure(rest_node, rest_map, env, is_constant);
        }
    } else if (target->type == EXPRTK_NODE_SPREAD) {
        exprtk_node_t *child = target->data.spread.child;
        if (child->type == EXPRTK_NODE_VARIABLE) {
            if (is_constant) exprtk_env_set_constant(env, child->data.variable.name, rhs);
            else exprtk_env_set(env, child->data.variable.name, rhs);
        }
    }
}

// Symbol Table Implementation
void exprtk_env_init_local(exprtk_env_t *env) {
    if (env) {
        memset(env, 0, sizeof(exprtk_env_t));

        // Create hash table for variables with standard allocator
        // Note: We pass NULL for free_func to avoid double-free issues
        HTAB(exprtk_var_entry_t) *htab = NULL;
        HTAB_OP(exprtk_var_entry_t, create)(&htab, mir_std_alloc, 16,
                                            var_hash, var_eq, NULL, NULL);
        env->vars = htab;
        env->funcs = NULL;

        env->modules = NULL;
        env->module_count = 0;
        env->flow = exprtk_FLOW_NORMAL;
        env->return_value.type = EXPRTK_VAL_NUMBER;
        env->return_value.data.number = 0.0;
        env->parent = NULL;
        mem_init(&env->arena, 65536); // 64KB initial

        // Safety Limits Defaults
        env->max_recursion = 100;
        env->curr_recursion = 0;
        env->max_loop_iterations = 10000;
        env->curr_loop_iterations = 0;
        env->max_nodes = 100000;
        env->curr_nodes = 0;
        env->aborted = 0;

        // Error reporting
        env->error_msg[0] = '\0';
        env->error_line = 0;
        env->error_column = 0;
    }
}

void exprtk_env_init(exprtk_env_t *env) {
    if (env) {
        exprtk_env_init_local(env);

        // Built-in constants (set_constant calls env_set internally)
        exprtk_env_set_constant(env, "pi", exprtk_val_num(3.14159265358979323846));
        exprtk_env_set_constant(env, "e", exprtk_val_num(2.71828182845904523536));
        exprtk_env_set_constant(env, "inf", exprtk_val_num(INFINITY));
        exprtk_env_set_constant(env, "nan", exprtk_val_num(NAN));
        exprtk_env_set_constant(env, "true", exprtk_val_bool(1));
        exprtk_env_set_constant(env, "false", exprtk_val_bool(0));
    }
}

exprtk_env_t* exprtk_env_snapshot(exprtk_env_t *env) {
    if (!env) return NULL;
    exprtk_env_t *root = env;
    while(root->parent) root = root->parent;

    exprtk_env_t *new_env = (exprtk_env_t *)malloc(sizeof(exprtk_env_t));
    if (!new_env) return NULL;

    // Initialize with hash table
    exprtk_env_init_local(new_env);
    new_env->eval_node = env->eval_node;
    new_env->exec_script_body = env->exec_script_body;

    // Copy all variables from env and its parents
    exprtk_env_t *curr_old = env;
    while (curr_old) {
        if (curr_old->vars) {
            // Copy from hash table
            HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_old->vars;
            HTAB_EL(exprtk_var_entry_t) *els_addr = VARR_ADDR(HTAB_EL(exprtk_var_entry_t), htab->els);
            htab_size_t bound = htab->els_bound;

            for (htab_size_t i = 0; i < bound; i++) {
                if (els_addr[i].hash != HTAB_DELETED_HASH) {
                    exprtk_var_entry_t entry = els_addr[i].el;
                    // Check if already exists in new_env via direct hash lookup
                    HTAB(exprtk_var_entry_t) *new_htab = (HTAB(exprtk_var_entry_t)*)new_env->vars;
                    exprtk_var_entry_t probe = { .name = entry.name };
                    exprtk_var_entry_t found;
                    int exists = HTAB_OP(exprtk_var_entry_t, do)(new_htab, probe, HTAB_FIND, &found);

                    if (!exists) {
                        exprtk_env_set_local_cloned(new_env, entry.name, entry.value);
                        if (entry.is_constant) {
                            exprtk_env_set_constant(new_env, entry.name, entry.value);
                        }
                    }
                }
            }
        }
        curr_old = curr_old->parent;
    }

    /* Attach to root's closure list for automatic cleanup on script exit */
    new_env->next_closure = root->next_closure;
    root->next_closure = new_env;

    return new_env;
}

static void exprtk_release_map_value(exprtk_value_t *map, exprtk_release_state_t *state) {
    if (!exprtk_value_is_object_like(map) || !map->data.map.htab) return;
    if (!ptr_set_mark_seen(&state->maps, map->data.map.htab)) return;

    exprtk_map_iter_t it = exprtk_map_iter_begin(map);
    exprtk_value_t child;
    while (exprtk_map_iter_next(&it, NULL, &child)) {
        exprtk_release_value(&child, state);
    }

    exprtk_map_free(map);
}

static void exprtk_release_list_value(exprtk_value_t *list, exprtk_release_state_t *state) {
    if (!list || (list->type != EXPRTK_VAL_LIST && list->type != EXPRTK_VAL_SET) ||
        !list->data.list.items)
        return;
    if (!ptr_set_mark_seen(&state->lists, list->data.list.items)) return;

    for (size_t i = 0; i < list->data.list.count; ++i) {
        exprtk_release_value(&list->data.list.items[i], state);
    }

    if (list->data.list.heap_owned) {
        turbo_vec_t vec = {
            list->data.list.items,
            list->data.list.count,
            list->data.list.capacity,
            sizeof(exprtk_value_t)
        };
        turbo_vec_destroy(&vec);
    }
    list->data.list.items = NULL;
    list->data.list.count = 0;
    list->data.list.capacity = 0;
    list->data.list.heap_owned = 0;
}

static void exprtk_release_oop_field(exprtk_value_t *value, void *user_data) {
    exprtk_release_value(value, (exprtk_release_state_t*)user_data);
}

static void exprtk_release_owned_instance(exprtk_instance_t *instance,
                                          exprtk_release_state_t *state) {
    if (!instance || !state || instance->arena != state->owner_arena) return;
    if (!ptr_set_mark_seen(&state->instances, instance)) return;

    exprtk_instance_foreach_field(instance, exprtk_release_oop_field, state);
    exprtk_instance_destroy(instance);
}

static void exprtk_release_value(exprtk_value_t *value, exprtk_release_state_t *state) {
    if (!value || !state) return;

    switch (value->type) {
        case EXPRTK_VAL_LIST:
        case EXPRTK_VAL_SET:
            exprtk_release_list_value(value, state);
            break;
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT:
            exprtk_release_map_value(value, state);
            break;
        case EXPRTK_VAL_CLASS: {
            exprtk_class_t *klass = value->data.class_val.klass;
            if (klass && klass->arena == state->owner_arena &&
                ptr_set_mark_seen(&state->classes, klass)) {
                exprtk_class_foreach_static_field(klass, exprtk_release_oop_field, state);
                exprtk_class_destroy(klass);
            }
            break;
        }
        case EXPRTK_VAL_INSTANCE:
            exprtk_release_owned_instance(value->data.instance_val.instance, state);
            break;
        case EXPRTK_VAL_BOUND_METHOD:
            exprtk_release_owned_instance(value->data.bound_method_val.instance, state);
            break;
        case EXPRTK_VAL_TYPED_ARRAY:
            if (value->data.typed_array.heap_owned) free(value->data.typed_array.data);
            value->data.typed_array.data = NULL;
            value->data.typed_array.count = 0;
            value->data.typed_array.heap_owned = 0;
            break;
        default:
            break;
    }
}

// Helper to manually free hash table entries
static void free_htab_entries(HTAB(exprtk_var_entry_t) *htab, exprtk_release_state_t *state) {
    if (!htab) return;

    // Iterate through all entries and free them manually
    HTAB_EL(exprtk_var_entry_t) *els_addr = VARR_ADDR(HTAB_EL(exprtk_var_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; i++) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            exprtk_var_entry_t entry = els_addr[i].el;
            free(entry.name);
            if (!entry.is_borrowed) {
                exprtk_release_value(&entry.value, state);
            }
        }
    }
}

static void exprtk_env_free_internal(exprtk_env_t *env, exprtk_release_state_t *state) {
    if (!env) return;

    mem_pool_t *previous_owner_arena = state->owner_arena;
    state->owner_arena = &env->arena;

    /* Free all captured closure environments */
    exprtk_env_t *closure = env->next_closure;
    while (closure) {
        exprtk_env_t *next = closure->next_closure;
        closure->next_closure = NULL;
        exprtk_env_free_internal(closure, state);
        free(closure);
        closure = next;
    }
    env->next_closure = NULL;
    if (env->modules) {
        free((void *)env->modules);
        env->modules = NULL;
    }
    env->module_count = 0;
    if (env->mod_cache) {
        free(env->mod_cache);
        env->mod_cache = NULL;
    }
    env->mod_cache_count = 0;
    if (env->regex_ctx) {
        exprtk_regex_ctx_destroy(env->regex_ctx);
        env->regex_ctx = NULL;
    }

    // Destroy hash table (manually free entries first)
    if (env->vars) {
        HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
        free_htab_entries(htab, state);
        HTAB_OP(exprtk_var_entry_t, destroy)(&htab);
        env->vars = NULL;
    }
    exprtk_func_t *fcurr = env->funcs;
    while (fcurr) {
        exprtk_func_t *fnext = fcurr->next;
        free(fcurr->name);
        if (fcurr->is_script) {
            if (fcurr->data.script.arg_params) {
                free(fcurr->data.script.arg_params);
            }
        }
        free(fcurr);
        fcurr = fnext;
    }
    env->funcs = NULL;
    mem_destroy(&env->arena);
    state->owner_arena = previous_owner_arena;
}

void exprtk_env_free(exprtk_env_t *env) {
    exprtk_release_state_t state;

    memset(&state, 0, sizeof(state));
    exprtk_env_free_internal(env, &state);
    exprtk_release_state_destroy(&state);
}

exprtk_value_t exprtk_env_get(exprtk_env_t *env, const char *name) {
    exprtk_value_t val = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!env || !name) return val;

    exprtk_env_t *curr_env = env;
    while (curr_env) {
        HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_env->vars;
        exprtk_var_entry_t key = { .name = (char*)name };
        exprtk_var_entry_t result;

        if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
            return result.value;
        }
        curr_env = curr_env->parent;
    }
    return val;
}

int exprtk_env_has(exprtk_env_t *env, const char *name) {
    if (!env || !name) return 0;
    exprtk_env_t *curr_env = env;
    while (curr_env) {
        if (curr_env->vars) {
            HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)curr_env->vars;
            exprtk_var_entry_t key = { .name = (char*)name };
            exprtk_var_entry_t result;
            if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result))
                return 1;
        }
        curr_env = curr_env->parent;
    }
    return 0;
}

static int exprtk_env_has_local_var(exprtk_env_t *env, const char *name) {
    if (!env || !name || !env->vars) return 0;

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;
    return HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result) ? 1 : 0;
}

void exprtk_env_import_vars(exprtk_env_t *dst, exprtk_env_t *src) {
    if (!dst || !src || !src->vars) return;

    HTAB(exprtk_var_entry_t) *src_htab = (HTAB(exprtk_var_entry_t)*)src->vars;
    HTAB_EL(exprtk_var_entry_t) *els_addr =
        VARR_ADDR(HTAB_EL(exprtk_var_entry_t), src_htab->els);
    htab_size_t bound = src_htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash == HTAB_DELETED_HASH) continue;
        exprtk_var_entry_t entry = els_addr[i].el;
        if (!entry.name || exprtk_env_has_local_var(dst, entry.name)) continue;
        if (strcmp(entry.name, "this") == 0) continue;

        exprtk_env_set_local(dst, entry.name, entry.value);
        if (entry.is_constant) exprtk_env_set_constant(dst, entry.name, entry.value);
    }
}

exprtk_value_t exprtk_env_eval_node(const exprtk_node_t *node, exprtk_env_t *env) {
    if (env && env->eval_node) return env->eval_node(node, env);
    return exprtk_eval(node, env);
}

void exprtk_env_register_func(exprtk_env_t *env, const char *name, exprtk_native_fn fn, void *user_data) {
    if (!env || !name || !fn) return;
    exprtk_func_t *new_func = (exprtk_func_t*)calloc(1, sizeof(exprtk_func_t));
    new_func->name = strdup(name);
    new_func->is_script = 0;
    new_func->access_level = EXPRTK_ACCESS_PUBLIC;
    new_func->data.native.fn = fn;
    new_func->data.native.user_data = user_data;
    new_func->next = env->funcs;
    env->funcs = new_func;
}

int exprtk_env_has_func(exprtk_env_t *env, const char *name) {
    if (!env || !name) return 0;
    for (exprtk_env_t *curr_env = env; curr_env; curr_env = curr_env->parent) {
        for (exprtk_func_t *f = curr_env->funcs; f; f = f->next) {
            if (f->name && strcmp(f->name, name) == 0) return 1;
        }
    }
    return 0;
}

void exprtk_env_set_constant(exprtk_env_t *env, const char *name, exprtk_value_t value) {
    exprtk_env_set(env, name, value);

    HTAB(exprtk_var_entry_t) *htab = (HTAB(exprtk_var_entry_t)*)env->vars;
    exprtk_var_entry_t key = { .name = (char*)name };
    exprtk_var_entry_t result;

    if (HTAB_OP(exprtk_var_entry_t, do)(htab, key, HTAB_FIND, &result)) {
        result.is_constant = 1;
        HTAB_OP(exprtk_var_entry_t, do)(htab, result, HTAB_REPLACE, &result);
    }
}

void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod) {
    if (!env || !mod) return;
    const exprtk_module_t **new_arr = (const exprtk_module_t **)realloc(
        (void *)env->modules, (env->module_count + 1) * sizeof(exprtk_module_t *));
    if (!new_arr) return;
    new_arr[env->module_count] = mod;
    env->modules = new_arr;
    env->module_count++;

    /* Invalidate sorted cache — will be rebuilt on next call */
    if (env->mod_cache) { free(env->mod_cache); env->mod_cache = NULL; }
    env->mod_cache_count = 0;
}

// Error throwing helper (Phase 2)
exprtk_value_t throw_error(exprtk_env_t *env, const exprtk_node_t *node, const char *fmt, ...) {
    if (!env) return exprtk_val_num(0);

    va_list args;
    va_start(args, fmt);
    vsnprintf(env->error_msg, sizeof(env->error_msg), fmt, args);
    va_end(args);

    env->error_line = node ? node->line : env->last_line;
    env->error_column = node ? node->column : env->last_column;
    env->flow = exprtk_FLOW_THROW;
    env->error_value = exprtk_val_str(tstr_v_from_cstr(env->error_msg));

    return exprtk_val_num(0);
}

// Type name helper (Phase 2)
const char* type_name(int type) {
    switch (type) {
        case EXPRTK_VAL_NUMBER: return "number";
        case EXPRTK_VAL_INTEGER: return "int64";
        case EXPRTK_VAL_BOOL: return "bool";
        case EXPRTK_VAL_STRING: return "string";
        case EXPRTK_VAL_BYTES: return "bytes";
        case EXPRTK_VAL_VECTOR: return "vector";
        case EXPRTK_VAL_MAP: return "map";
        case EXPRTK_VAL_OBJECT: return "object";
        case EXPRTK_VAL_NULL: return "null";
        case EXPRTK_VAL_LIST: return "list";
        case EXPRTK_VAL_SET: return "set";
        case EXPRTK_VAL_FUNCTION: return "function";
        case EXPRTK_VAL_CLASS: return "class";
        case EXPRTK_VAL_INSTANCE: return "instance";
        case EXPRTK_VAL_BOUND_METHOD: return "bound_method";
        case EXPRTK_VAL_UUID: return "uuid";
        case EXPRTK_VAL_DATETIME: return "datetime";
        case EXPRTK_VAL_DATE: return "date";
        case EXPRTK_VAL_TIME: return "time";
        case EXPRTK_VAL_DURATION: return "duration";
        case EXPRTK_VAL_DECIMAL: return "decimal";
        case EXPRTK_VAL_BIGINT: return "bigint";
        case EXPRTK_VAL_MONEY: return "money";
        case EXPRTK_VAL_ENUM: return "enum";
        case EXPRTK_VAL_FLAGS: return "flags";
        case EXPRTK_VAL_OFFSET_DATETIME: return "offset_datetime";
        case EXPRTK_VAL_TYPED_ARRAY: return "typed_array";
        default: return "unknown";
    }
}

// Convert value to double (handles INTEGER and NUMBER)
double val_to_double(exprtk_value_t val) {
    if (val.type == EXPRTK_VAL_INTEGER) return (double)val.data.integer;
    if (val.type == EXPRTK_VAL_NUMBER) return val.data.number;
    if (val.type == EXPRTK_VAL_BOOL) return val.data.boolean ? 1.0 : 0.0;
    return 0.0;
}

int values_match(exprtk_value_t lhs, exprtk_value_t rhs) {
    int lhs_numeric = (lhs.type == EXPRTK_VAL_NUMBER || lhs.type == EXPRTK_VAL_INTEGER);
    int rhs_numeric = (rhs.type == EXPRTK_VAL_NUMBER || rhs.type == EXPRTK_VAL_INTEGER);

    if (lhs_numeric && rhs_numeric)
        return fabs(val_to_double(lhs) - val_to_double(rhs)) < 1e-9;
    if (lhs.type == EXPRTK_VAL_BOOL && rhs.type == EXPRTK_VAL_BOOL)
        return lhs.data.boolean == rhs.data.boolean;
    if (lhs.type == EXPRTK_VAL_STRING && rhs.type == EXPRTK_VAL_STRING)
        return tstr_v_eq(lhs.data.string, rhs.data.string);
    if (lhs.type == EXPRTK_VAL_BYTES && rhs.type == EXPRTK_VAL_BYTES)
        return lhs.data.bytes.len == rhs.data.bytes.len &&
               (lhs.data.bytes.len == 0 ||
                memcmp(lhs.data.bytes.data, rhs.data.bytes.data, lhs.data.bytes.len) == 0);
    if (lhs.type == EXPRTK_VAL_UUID && rhs.type == EXPRTK_VAL_UUID)
        return memcmp(lhs.data.uuid.bytes, rhs.data.uuid.bytes, sizeof(lhs.data.uuid.bytes)) == 0;
    if (lhs.type == EXPRTK_VAL_DATETIME && rhs.type == EXPRTK_VAL_DATETIME)
        return memcmp(&lhs.data.datetime, &rhs.data.datetime, sizeof(lhs.data.datetime)) == 0;
    if (lhs.type == EXPRTK_VAL_OFFSET_DATETIME && rhs.type == EXPRTK_VAL_OFFSET_DATETIME)
        return lhs.data.offset_datetime.offset_minutes == rhs.data.offset_datetime.offset_minutes &&
               memcmp(&lhs.data.offset_datetime.datetime, &rhs.data.offset_datetime.datetime,
                      sizeof(lhs.data.offset_datetime.datetime)) == 0;
    if (lhs.type == EXPRTK_VAL_DATE && rhs.type == EXPRTK_VAL_DATE)
        return lhs.data.date.year == rhs.data.date.year &&
               lhs.data.date.month == rhs.data.date.month &&
               lhs.data.date.day == rhs.data.date.day;
    if (lhs.type == EXPRTK_VAL_TIME && rhs.type == EXPRTK_VAL_TIME)
        return lhs.data.time.hour == rhs.data.time.hour &&
               lhs.data.time.minute == rhs.data.time.minute &&
               lhs.data.time.second == rhs.data.time.second &&
               lhs.data.time.millisecond == rhs.data.time.millisecond;
    if (lhs.type == EXPRTK_VAL_DURATION && rhs.type == EXPRTK_VAL_DURATION)
        return lhs.data.duration_ms == rhs.data.duration_ms;
    if (lhs.type == EXPRTK_VAL_DECIMAL && rhs.type == EXPRTK_VAL_DECIMAL) {
        exprtk_decimal_t l = lhs.data.decimal;
        exprtk_decimal_t r = rhs.data.decimal;
        while (l.scale > 0 && l.mantissa % 10 == 0) {
            l.mantissa /= 10;
            l.scale--;
        }
        while (r.scale > 0 && r.mantissa % 10 == 0) {
            r.mantissa /= 10;
            r.scale--;
        }
        if (l.mantissa == 0) l.scale = 0;
        if (r.mantissa == 0) r.scale = 0;
        return l.mantissa == r.mantissa && l.scale == r.scale;
    }
    if (lhs.type == EXPRTK_VAL_BIGINT && rhs.type == EXPRTK_VAL_BIGINT)
        return tstr_v_eq(lhs.data.bigint.text, rhs.data.bigint.text);
    if (lhs.type == EXPRTK_VAL_MONEY && rhs.type == EXPRTK_VAL_MONEY)
        return lhs.data.money.amount.mantissa == rhs.data.money.amount.mantissa &&
               lhs.data.money.amount.scale == rhs.data.money.amount.scale &&
               memcmp(lhs.data.money.currency, rhs.data.money.currency, 4) == 0;
    if ((lhs.type == EXPRTK_VAL_ENUM || lhs.type == EXPRTK_VAL_FLAGS) && lhs.type == rhs.type)
        return lhs.data.enum_val.value == rhs.data.enum_val.value &&
               tstr_v_eq(lhs.data.enum_val.type_name, rhs.data.enum_val.type_name);
    if (lhs.type == EXPRTK_VAL_NULL && rhs.type == EXPRTK_VAL_NULL)
        return 1;

    return lhs.type == rhs.type;
}

static int is_private_member_name(const char *member) {
    return member && member[0] == '_';
}

exprtk_class_t *eval_current_class(exprtk_env_t *env);
static int can_access_private_member(exprtk_env_t *env,
                                     const exprtk_node_t *object_node);

static int exprtk_class_is_subclass_of(exprtk_class_t *klass, exprtk_class_t *base) {
    for (exprtk_class_t *curr = klass; curr; curr = curr->prototype) {
        if (curr == base) return 1;
    }
    return 0;
}

static int can_access_declared_member(exprtk_env_t *env, exprtk_class_t *owner_class,
                                      int access_level) {
    if (access_level == EXPRTK_ACCESS_PUBLIC) return 1;
    if (!owner_class) return 0;

    exprtk_class_t *current_class = eval_current_class(env);
    if (!current_class) return 0;
    if (current_class == owner_class) return 1;
    if (access_level == EXPRTK_ACCESS_PROTECTED) {
        return exprtk_class_is_subclass_of(current_class, owner_class);
    }
    return 0;
}

int can_access_method(exprtk_env_t *env, exprtk_func_t *method,
                             const exprtk_node_t *object_node) {
    if (!method) return 0;
    if (is_private_member_name(method->name) &&
        !can_access_private_member(env, object_node)) {
        return 0;
    }
    return can_access_declared_member(env, method->owner_class, method->access_level);
}

exprtk_value_t throw_method_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                                const char *method_name,
                                                exprtk_func_t *method) {
    if ((method_name && is_private_member_name(method_name)) ||
        (method && method->access_level == EXPRTK_ACCESS_PRIVATE)) {
        return throw_error(env, node, "Private method '%s' is not accessible", method_name);
    }
    if (method && method->access_level == EXPRTK_ACCESS_PROTECTED) {
        return throw_error(env, node, "Protected method '%s' is not accessible", method_name);
    }
    return throw_error(env, node, "Method '%s' is not accessible", method_name);
}

exprtk_value_t throw_field_access_error(exprtk_env_t *env, const exprtk_node_t *node,
                                               const char *field_name, int access_level) {
    if ((field_name && is_private_member_name(field_name)) ||
        access_level == EXPRTK_ACCESS_PRIVATE) {
        return throw_error(env, node, "Private field '%s' is not accessible", field_name);
    }
    if (access_level == EXPRTK_ACCESS_PROTECTED) {
        return throw_error(env, node, "Protected field '%s' is not accessible", field_name);
    }
    return throw_error(env, node, "Field '%s' is not accessible", field_name);
}

int can_access_declared_field(exprtk_env_t *env, exprtk_class_t *owner_class,
                                     int access_level,
                                     const exprtk_node_t *object_node,
                                     const char *field_name) {
    if (is_private_member_name(field_name) &&
        !can_access_private_member(env, object_node)) {
        return 0;
    }
    return can_access_declared_member(env, owner_class, access_level);
}

static int can_access_private_member(exprtk_env_t *env,
                                     const exprtk_node_t *object_node) {
    if (!object_node) return 0;
    if (object_node->type == EXPRTK_NODE_THIS) return 1;
    if (object_node->type != EXPRTK_NODE_VARIABLE) return 0;

    exprtk_class_t *current_class = eval_current_class(env);
    const char *name = object_node->data.variable.name;
    if (!current_class || !name) return 0;

    if (current_class->name && strcmp(current_class->name, name) == 0) return 1;

    exprtk_value_t value = exprtk_env_get(env, name);
    return value.type == EXPRTK_VAL_CLASS &&
           value.data.class_val.klass == current_class;
}

static exprtk_node_t *eval_param_binding_node(exprtk_node_t *param) {
    if (param && param->type == EXPRTK_NODE_MEMBER_ACCESS &&
        param->data.member_access.object &&
        param->data.member_access.object->type == EXPRTK_NODE_VARIABLE) {
        return param->data.member_access.object;
    }
    return param;
}

exprtk_value_t eval_script_function(exprtk_func_t *func, size_t argc,
                                           exprtk_value_t *args, exprtk_env_t *parent_env,
                                           exprtk_env_t *caller_env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!func || !func->is_script || !parent_env || !caller_env) return zero;

    caller_env->curr_recursion++;
    if (caller_env->curr_recursion > caller_env->max_recursion) {
        caller_env->aborted = 1;
        caller_env->curr_recursion--;
        return zero;
    }

    exprtk_env_t *local_env = (exprtk_env_t *)malloc(sizeof(exprtk_env_t));
    if (!local_env) {
        caller_env->aborted = 1;
        caller_env->curr_recursion--;
        return zero;
    }

    exprtk_env_init_local(local_env);
    local_env->parent = parent_env;
    local_env->eval_node = caller_env->eval_node ? caller_env->eval_node : parent_env->eval_node;
    local_env->exec_script_body =
        caller_env->exec_script_body ? caller_env->exec_script_body : parent_env->exec_script_body;
    exprtk_env_import_vars(local_env, func->closure_env);
    local_env->current_class = func->owner_class ? func->owner_class : parent_env->current_class;
    local_env->current_method_is_static =
        func->owner_class ? func->is_static_method : parent_env->current_method_is_static;
    local_env->current_method_is_constructor =
        func->owner_class && !func->is_static_method && func->name &&
        strcmp(func->name, "constructor") == 0;
    local_env->max_recursion = caller_env->max_recursion;
    local_env->curr_recursion = caller_env->curr_recursion;
    local_env->max_loop_iterations = caller_env->max_loop_iterations;
    local_env->curr_loop_iterations = caller_env->curr_loop_iterations;
    local_env->max_nodes = caller_env->max_nodes;
    local_env->curr_nodes = caller_env->curr_nodes;

    exprtk_node_t **params = func->data.script.arg_params;
    size_t param_count = func->data.script.arg_count;
    int is_variadic = 0;
    if (param_count > 0 && params && params[param_count - 1] &&
        params[param_count - 1]->type == EXPRTK_NODE_SPREAD) {
        is_variadic = 1;
    }

    size_t std_args = is_variadic ? (param_count - 1) : param_count;
    for (size_t i = 0; i < std_args; ++i) {
        exprtk_node_t *param = params ? params[i] : NULL;
        exprtk_node_t *binding_param = eval_param_binding_node(param);
        exprtk_value_t val = zero;
        if (i < argc) {
            val = args[i];
        } else if (param && param->type == EXPRTK_NODE_ASSIGNMENT && param->data.assignment.value) {
            val = exprtk_env_eval_node(param->data.assignment.value, parent_env);
        }

        if (!binding_param) continue;
        if (binding_param->type == EXPRTK_NODE_ASSIGNMENT) {
            exprtk_env_set(local_env, binding_param->data.assignment.name, val);
        } else {
            eval_destructure(binding_param, val, local_env, 0);
        }
    }

    if (is_variadic) {
        size_t rest_sz = (argc > std_args) ? (argc - std_args) : 0;
        exprtk_value_t *rest_items = NULL;
        if (rest_sz > 0) {
            rest_items = (exprtk_value_t*)mem_alloc(&local_env->arena, rest_sz * sizeof(exprtk_value_t));
        }
        if (rest_sz == 0 || rest_items) {
            for (size_t i = 0; i < rest_sz; ++i) {
                rest_items[i] = exprtk_value_clone_to_env(args[std_args + i], local_env);
            }
            eval_destructure(params[param_count - 1], exprtk_val_list_ex(rest_items, rest_sz, 0),
                             local_env, 0);
        }
    }

    exprtk_value_t result = zero;
    if (!local_env->exec_script_body ||
        !local_env->exec_script_body(func, local_env, caller_env, &result)) {
        result = exprtk_env_eval_node(func->data.script.body, local_env);
    }
    result = exprtk_value_clone_to_env(result, caller_env);

    caller_env->curr_nodes = local_env->curr_nodes;
    caller_env->curr_loop_iterations = local_env->curr_loop_iterations;
    caller_env->aborted = local_env->aborted;
    caller_env->curr_recursion--;

    if (local_env->flow == exprtk_FLOW_RETURN) {
        result = exprtk_value_clone_to_env(local_env->return_value, caller_env);
    } else if (local_env->flow == exprtk_FLOW_THROW) {
        caller_env->flow = exprtk_FLOW_THROW;
        caller_env->error_value = exprtk_value_clone_to_env(local_env->error_value, caller_env);
        snprintf(caller_env->error_msg, sizeof(caller_env->error_msg), "%s",
                 local_env->error_msg);
        caller_env->error_line = local_env->error_line;
        caller_env->error_column = local_env->error_column;
    } else if (local_env->flow == exprtk_FLOW_YIELD) {
        caller_env->flow = exprtk_FLOW_YIELD;
    }

    exprtk_env_free(local_env);
    free(local_env);
    return result;
}

static int grow_value_array(exprtk_value_t **vals, size_t *cap, size_t needed) {
    if (!vals || !cap) return 0;
    if (needed <= *cap) return 1;

    size_t new_cap = (*cap > 0) ? *cap : 4;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    exprtk_value_t *new_vals = (exprtk_value_t*)realloc(*vals, new_cap * sizeof(exprtk_value_t));
    if (!new_vals) return 0;

    *vals = new_vals;
    *cap = new_cap;
    return 1;
}

exprtk_value_t* eval_expand_args(exprtk_node_t **nodes, size_t count, exprtk_env_t *env, size_t *out_count) {
    size_t cap = count > 0 ? count : 4;
    exprtk_value_t *vals = (exprtk_value_t*)malloc(cap * sizeof(exprtk_value_t));
    if (!vals) {
        *out_count = 0;
        return NULL;
    }
    size_t actual = 0;

    for (size_t i = 0; i < count; ++i) {
        if (nodes[i]->type == EXPRTK_NODE_SPREAD) {
            exprtk_value_t el = exprtk_env_eval_node(nodes[i]->data.spread.child, env);
            if (el.type == EXPRTK_VAL_VECTOR) {
                if (!grow_value_array(&vals, &cap, actual + el.data.vector.size)) {
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                for (size_t j = 0; j < el.data.vector.size; ++j) {
                    vals[actual++] = exprtk_val_num(el.data.vector.data[j]);
                }
            } else if (el.type == EXPRTK_VAL_LIST) {
                if (!grow_value_array(&vals, &cap, actual + el.data.list.count)) {
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                for (size_t j = 0; j < el.data.list.count; ++j) {
                    vals[actual++] = el.data.list.items[j];
                }
            } else {
                if (!grow_value_array(&vals, &cap, actual + 1)) {
                    free(vals);
                    *out_count = 0;
                    return NULL;
                }
                vals[actual++] = el;
            }
        } else {
            if (!grow_value_array(&vals, &cap, actual + 1)) {
                free(vals);
                *out_count = 0;
                return NULL;
            }
            vals[actual++] = exprtk_env_eval_node(nodes[i], env);
        }
        if (env && (env->flow != exprtk_FLOW_NORMAL || env->aborted)) break;
    }
    *out_count = actual;
    return vals;
}

exprtk_class_t *eval_current_class(exprtk_env_t *env) {
    while (env) {
        if (env->current_class) return env->current_class;
        env = env->parent;
    }
    return NULL;
}

static exprtk_func_t *eval_find_constructor(exprtk_class_t *klass) {
    while (klass) {
        if (klass->constructor) return klass->constructor;
        klass = klass->prototype;
    }
    return NULL;
}

exprtk_func_t *eval_find_constructor_typed(exprtk_class_t *klass, size_t argc,
                                                  exprtk_value_t *args) {
    exprtk_func_t *constructor = exprtk_class_lookup_constructor_typed(klass, argc, args);
    return constructor ? constructor : eval_find_constructor(klass);
}

static exprtk_func_t *eval_make_class_func(exprtk_node_t *method_node,
                                           exprtk_class_t *klass,
                                           exprtk_env_t *class_closure,
                                           exprtk_env_t *env) {
    if (!method_node || !klass || !env) return NULL;

    exprtk_func_t *method_func =
        (exprtk_func_t *)mem_alloc(&env->arena, sizeof(exprtk_func_t));
    if (!method_func) return NULL;

    method_func->name = method_node->data.method.name;
    method_func->is_script = 1;
    method_func->owner_class = klass;
    method_func->closure_env = class_closure;
    method_func->is_static_method = method_node->data.method.is_static;
    method_func->access_level = method_node->data.method.access_level;
    method_func->is_override = method_node->data.method.is_override;
    method_func->is_final = method_node->data.method.is_final;
    method_func->data.script.arg_params = method_node->data.method.arg_params;
    method_func->data.script.arg_count = method_node->data.method.arg_count;
    method_func->data.script.body = method_node->data.method.body;
    method_func->next = NULL;
    return method_func;
}

static exprtk_func_t *eval_lookup_parent_method_signature(exprtk_class_t *parent,
                                                          exprtk_node_t *method_node,
                                                          exprtk_class_t *owner_class,
                                                          exprtk_env_t *class_closure) {
    if (!parent || !method_node) return NULL;

    exprtk_func_t signature;
    memset(&signature, 0, sizeof(signature));
    signature.name = method_node->data.method.name;
    signature.is_script = 1;
    signature.owner_class = owner_class;
    signature.closure_env = class_closure;
    signature.is_static_method = method_node->data.method.is_static;
    signature.data.script.arg_params = method_node->data.method.arg_params;
    signature.data.script.arg_count = method_node->data.method.arg_count;

    return exprtk_class_lookup_method_signature(parent, &signature);
}

exprtk_value_t eval_class_value_instantiation(exprtk_class_t *klass,
                                                     const char *class_name,
                                                     exprtk_node_t **arg_nodes,
                                                     size_t arg_count,
                                                     exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!klass || !env) return zero;

    if (exprtk_class_is_interface(klass)) {
        return throw_error(env, NULL, "Cannot instantiate interface '%s'", class_name);
    }
    if (exprtk_class_is_abstract(klass)) {
        return throw_error(env, NULL, "Cannot instantiate abstract class '%s'", class_name);
    }

    size_t actual_count = 0;
    exprtk_value_t *args = eval_expand_args(arg_nodes, arg_count, env, &actual_count);
    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
        free(args);
        return zero;
    }

    exprtk_instance_t *instance = exprtk_instance_create(klass, &env->arena);
    if (!instance) {
        free(args);
        return zero;
    }

    exprtk_func_t *constructor = eval_find_constructor_typed(klass, actual_count, args);
    if (constructor) {
        exprtk_env_t ctor_env;
        exprtk_env_init_local(&ctor_env);
        ctor_env.parent = env;
        ctor_env.eval_node = env->eval_node;
        ctor_env.exec_script_body = env->exec_script_body;
        ctor_env.current_class = klass;
        ctor_env.current_method_is_static = 0;
        ctor_env.current_method_is_constructor = 1;
        ctor_env.max_recursion = env->max_recursion;
        ctor_env.curr_recursion = env->curr_recursion;
        ctor_env.max_loop_iterations = env->max_loop_iterations;
        ctor_env.curr_loop_iterations = env->curr_loop_iterations;
        ctor_env.max_nodes = env->max_nodes;
        ctor_env.curr_nodes = env->curr_nodes;
        exprtk_env_set_local_borrowed(&ctor_env, "this", exprtk_val_instance(instance));

        eval_script_function(constructor, actual_count, args, &ctor_env, env);

        exprtk_env_free(&ctor_env);

        if (env->flow != exprtk_FLOW_NORMAL || env->aborted) {
            free(args);
            return zero;
        }
    }

    free(args);
    return exprtk_val_instance(instance);
}

exprtk_value_t eval_class_instantiation(const char *class_name, exprtk_node_t **arg_nodes,
                                               size_t arg_count, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!class_name || !env) return zero;

    exprtk_class_t *klass = NULL;
    exprtk_value_t class_value = exprtk_env_get(env, class_name);
    if (class_value.type == EXPRTK_VAL_CLASS) {
        klass = class_value.data.class_val.klass;
    }
    if (!klass) {
        return throw_error(env, NULL, "Undefined class '%s'", class_name);
    }

    return eval_class_value_instantiation(klass, class_name, arg_nodes, arg_count, env);
}

exprtk_value_t eval_instance_script_method(exprtk_instance_t *instance,
                                                  exprtk_func_t *method,
                                                  size_t argc,
                                                  exprtk_value_t *args,
                                                  exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!instance || !method || !env) return zero;

    exprtk_env_t method_env;
    exprtk_env_init_local(&method_env);
    method_env.parent = env;
    method_env.eval_node = env->eval_node;
    method_env.exec_script_body = env->exec_script_body;
    method_env.current_class = method->owner_class;
    method_env.current_method_is_static = 0;
    method_env.current_method_is_constructor = 0;
    method_env.max_recursion = env->max_recursion;
    method_env.curr_recursion = env->curr_recursion;
    method_env.max_loop_iterations = env->max_loop_iterations;
    method_env.curr_loop_iterations = env->curr_loop_iterations;
    method_env.max_nodes = env->max_nodes;
    method_env.curr_nodes = env->curr_nodes;
    exprtk_env_set_local_borrowed(&method_env, "this", exprtk_val_instance(instance));

    exprtk_value_t result = eval_script_function(method, argc, args, &method_env, env);
    exprtk_env_free(&method_env);
    return result;
}

exprtk_value_t eval_class_def_node(const exprtk_node_t *node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!node || !env || node->type != EXPRTK_NODE_CLASS_DEF) return zero;

    const char *class_name = node->data.class_def.name;
    if (!class_name) return zero;

    exprtk_class_t *parent = NULL;
    if (node->data.class_def.parent_name) {
        exprtk_value_t parent_value = exprtk_env_get(env, node->data.class_def.parent_name);
        if (parent_value.type == EXPRTK_VAL_CLASS) {
            parent = parent_value.data.class_val.klass;
        }
        if (!parent) {
            return throw_error(env, node, "Undefined parent class '%s'",
                               node->data.class_def.parent_name);
        }
    }

    exprtk_class_t *klass = exprtk_class_create(&env->arena, class_name,
                                                node->data.class_def.constructor,
                                                node->data.class_def.methods,
                                                node->data.class_def.method_count);
    if (!klass) return zero;
    exprtk_class_set_abstract(klass, node->data.class_def.is_abstract);
    exprtk_class_set_interface(klass, node->data.class_def.is_interface);
    exprtk_env_t *class_closure = exprtk_env_snapshot(env);

    if (parent && exprtk_class_is_final(parent)) {
        return throw_error(env, node, "Cannot extend final class '%s'",
                           parent->name ? parent->name : "<class>");
    }

    if (parent) {
        exprtk_class_set_prototype(klass, parent);
    }

    if (node->data.class_def.constructor) {
        exprtk_node_t *ctor = node->data.class_def.constructor;
        exprtk_func_t *ctor_func = (exprtk_func_t *)mem_alloc(&env->arena, sizeof(exprtk_func_t));
        if (ctor_func) {
            ctor_func->name = "constructor";
            ctor_func->is_script = 1;
            ctor_func->owner_class = klass;
            ctor_func->closure_env = class_closure;
            ctor_func->is_static_method = 0;
            ctor_func->access_level = EXPRTK_ACCESS_PUBLIC;
            ctor_func->is_override = 0;
            ctor_func->is_final = 0;
            ctor_func->data.script.arg_params = ctor->data.func_def.arg_params;
            ctor_func->data.script.arg_count = ctor->data.func_def.arg_count;
            ctor_func->data.script.body = ctor->data.func_def.body;
            ctor_func->next = NULL;
            klass->constructor = ctor_func;
            exprtk_class_add_constructor(klass, ctor_func);
        }
    }

    for (size_t i = 0; i < node->data.class_def.method_count; ++i) {
        exprtk_node_t *method_node = node->data.class_def.methods[i];
        if (method_node->type == EXPRTK_NODE_FIELD_DECL) {
            exprtk_value_t field_value;
            memset(&field_value, 0, sizeof(field_value));
            if (method_node->data.field_decl.initializer) {
                field_value = exprtk_env_eval_node(method_node->data.field_decl.initializer, env);
                if (env->flow != exprtk_FLOW_NORMAL || env->aborted) return zero;
            } else {
                field_value.type = EXPRTK_VAL_NULL;
            }

            if (method_node->data.field_decl.is_static) {
                exprtk_class_declare_static_field(
                    klass, method_node->data.field_decl.name, field_value,
                    method_node->data.field_decl.access_level);
            } else {
                exprtk_class_declare_instance_field(
                    klass, method_node->data.field_decl.name, field_value,
                    method_node->data.field_decl.initializer != NULL,
                    method_node->data.field_decl.access_level);
            }
            continue;
        }
        if (method_node->type != EXPRTK_NODE_METHOD) continue;

        if (method_node->data.method.name &&
            strcmp(method_node->data.method.name, "constructor") == 0) {
            if (method_node->data.method.is_abstract) {
                return throw_error(env, method_node, "Constructor cannot be abstract");
            }
            exprtk_func_t *ctor_func =
                eval_make_class_func(method_node, klass, class_closure, env);
            if (ctor_func) {
                ctor_func->access_level = EXPRTK_ACCESS_PUBLIC;
                ctor_func->is_static_method = 0;
                exprtk_class_add_constructor(klass, ctor_func);
            }
            continue;
        }

        exprtk_func_t *parent_method =
            eval_lookup_parent_method_signature(parent, method_node, klass, class_closure);
        if (method_node->data.method.is_override && !parent_method) {
            return throw_error(env, method_node,
                               "Method '%s' is marked override but no parent method matches",
                               method_node->data.method.name);
        }
        if (parent_method && parent_method->is_final) {
            return throw_error(env, method_node, "Cannot override final method '%s'",
                               method_node->data.method.name);
        }

        if (method_node->data.method.is_abstract) {
            exprtk_class_add_abstract_method_signature(
                klass, method_node->data.method.name,
                method_node->data.method.arg_params,
                method_node->data.method.arg_count,
                class_closure,
                method_node->data.method.is_static);
            continue;
        }

        exprtk_func_t *method_func =
            eval_make_class_func(method_node, klass, class_closure, env);
        if (!method_func) continue;

        exprtk_class_add_method(klass, method_node->data.method.name, method_func,
                                method_node->data.method.is_static);
    }

    for (size_t i = 0; i < node->data.class_def.interface_count; ++i) {
        const char *interface_name = node->data.class_def.interface_names[i];
        exprtk_value_t interface_value = interface_name
            ? exprtk_env_get(env, interface_name)
            : zero;
        if (interface_value.type != EXPRTK_VAL_CLASS) {
            return throw_error(env, node, "Undefined interface '%s'",
                               interface_name ? interface_name : "<unknown>");
        }

        exprtk_class_t *interface_class = interface_value.data.class_val.klass;
        if (!exprtk_class_is_interface(interface_class)) {
            return throw_error(env, node, "Class '%s' is not an interface",
                               interface_name ? interface_name : "<unknown>");
        }
        exprtk_class_add_interface(klass, interface_class);
        exprtk_class_add_abstract_methods_from(klass, interface_class);
    }
    exprtk_class_finalize_abstract_methods(klass);
    exprtk_class_set_final(klass, node->data.class_def.is_final);

    exprtk_env_register_class(env, class_name, klass);
    return zero;
}

static exprtk_value_t *numeric_args_to_values(size_t argc, const double *argv) {
    if (argc == 0) return NULL;
    exprtk_value_t *args = (exprtk_value_t *)calloc(argc, sizeof(exprtk_value_t));
    if (!args) return NULL;
    for (size_t i = 0; i < argc; ++i) {
        args[i] = exprtk_val_num(argv ? argv[i] : 0.0);
    }
    return args;
}

static exprtk_value_t *numeric_args_to_values_buffer(size_t argc, const double *argv,
                                                     exprtk_value_t *stack_args,
                                                     size_t stack_cap, int *needs_free) {
    if (needs_free) *needs_free = 0;
    if (argc == 0) return NULL;

    exprtk_value_t *args = stack_args;
    if (argc > stack_cap) {
        args = (exprtk_value_t *)malloc(argc * sizeof(exprtk_value_t));
        if (!args) return NULL;
        if (needs_free) *needs_free = 1;
    }

    for (size_t i = 0; i < argc; ++i) {
        args[i] = exprtk_val_num(argv ? argv[i] : 0.0);
    }
    return args;
}

CXX_C_API exprtk_value_t exprtk_oop_define_class(const exprtk_node_t *node, exprtk_env_t *env) {
    return eval_class_def_node(node, env);
}

CXX_C_API exprtk_value_t exprtk_oop_alias_class(const char *target_name, const char *source_name,
                                                exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!target_name || !source_name || !env) return zero;

    exprtk_class_t *klass = NULL;
    exprtk_value_t source_value = exprtk_env_get(env, source_name);
    if (source_value.type == EXPRTK_VAL_CLASS) klass = source_value.data.class_val.klass;

    if (!klass) return throw_error(env, NULL, "Undefined class '%s'", source_name);

    exprtk_value_t class_value = exprtk_val_class(klass);
    exprtk_env_set(env, target_name, class_value);
    return class_value;
}

CXX_C_API exprtk_value_t exprtk_oop_instantiate_numeric(const char *class_name, size_t argc,
                                                        const double *argv, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!class_name || !env) return zero;

    exprtk_class_t *klass = NULL;
    exprtk_value_t class_value = exprtk_env_get(env, class_name);
    if (class_value.type == EXPRTK_VAL_CLASS) klass = class_value.data.class_val.klass;
    if (!klass) return throw_error(env, NULL, "Undefined class '%s'", class_name);
    if (exprtk_class_is_interface(klass)) {
        return throw_error(env, NULL, "Cannot instantiate interface '%s'", class_name);
    }
    if (exprtk_class_is_abstract(klass)) {
        return throw_error(env, NULL, "Cannot instantiate abstract class '%s'", class_name);
    }

    exprtk_instance_t *instance = exprtk_instance_create(klass, &env->arena);
    if (!instance) return zero;

    exprtk_value_t *args = numeric_args_to_values(argc, argv);
    if (argc > 0 && !args) return zero;

    exprtk_func_t *constructor = eval_find_constructor_typed(klass, argc, args);
    if (constructor) {
        exprtk_env_t ctor_env;
        exprtk_env_init_local(&ctor_env);
        ctor_env.parent = env;
        ctor_env.eval_node = env->eval_node;
        ctor_env.exec_script_body = env->exec_script_body;
        ctor_env.current_class = klass;
        ctor_env.current_method_is_static = 0;
        ctor_env.current_method_is_constructor = 1;
        ctor_env.max_recursion = env->max_recursion;
        ctor_env.curr_recursion = env->curr_recursion;
        ctor_env.max_loop_iterations = env->max_loop_iterations;
        ctor_env.curr_loop_iterations = env->curr_loop_iterations;
        ctor_env.max_nodes = env->max_nodes;
        ctor_env.curr_nodes = env->curr_nodes;
        exprtk_env_set_local_borrowed(&ctor_env, "this", exprtk_val_instance(instance));

        eval_script_function(constructor, argc, args, &ctor_env, env);
        exprtk_env_free(&ctor_env);
    }

    free(args);
    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) return zero;
    return exprtk_val_instance(instance);
}

CXX_C_API exprtk_value_t exprtk_oop_instantiate_class_value(exprtk_value_t class_value,
                                                            const char *class_name, size_t argc,
                                                            exprtk_value_t *args,
                                                            exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (class_value.type != EXPRTK_VAL_CLASS || !class_value.data.class_val.klass || !env) {
        return zero;
    }

    exprtk_class_t *klass = class_value.data.class_val.klass;
    if (exprtk_class_is_interface(klass)) {
        return throw_error(env, NULL, "Cannot instantiate interface '%s'",
                           class_name ? class_name : "<class>");
    }
    if (exprtk_class_is_abstract(klass)) {
        return throw_error(env, NULL, "Cannot instantiate abstract class '%s'",
                           class_name ? class_name : "<class>");
    }

    exprtk_instance_t *instance = exprtk_instance_create(klass, &env->arena);
    if (!instance) return zero;

    exprtk_func_t *constructor = eval_find_constructor_typed(klass, argc, args);
    if (constructor) {
        exprtk_env_t ctor_env;
        exprtk_env_init_local(&ctor_env);
        ctor_env.parent = env;
        ctor_env.eval_node = env->eval_node;
        ctor_env.exec_script_body = env->exec_script_body;
        ctor_env.current_class = klass;
        ctor_env.current_method_is_static = 0;
        ctor_env.current_method_is_constructor = 1;
        ctor_env.max_recursion = env->max_recursion;
        ctor_env.curr_recursion = env->curr_recursion;
        ctor_env.max_loop_iterations = env->max_loop_iterations;
        ctor_env.curr_loop_iterations = env->curr_loop_iterations;
        ctor_env.max_nodes = env->max_nodes;
        ctor_env.curr_nodes = env->curr_nodes;
        exprtk_env_set_local_borrowed(&ctor_env, "this", exprtk_val_instance(instance));

        eval_script_function(constructor, argc, args, &ctor_env, env);
        exprtk_env_free(&ctor_env);
    }

    if (env->flow != exprtk_FLOW_NORMAL || env->aborted) return zero;
    return exprtk_val_instance(instance);
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_checked_numeric(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, const double *argv, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    exprtk_value_t *args = numeric_args_to_values(argc, argv);
    if (argc > 0 && !args) return zero;

    exprtk_value_t result = exprtk_oop_call_method_checked_values(
        object_name, method_name, object_node, argc, args, env);
    free(args);
    return result;
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_checked_values(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, exprtk_value_t *args, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !method_name || !env || (argc > 0 && !args)) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);
    exprtk_value_t result = zero;
    if (object.type == EXPRTK_VAL_INSTANCE) {
        exprtk_instance_t *instance = object.data.instance_val.instance;
        exprtk_func_t *method =
            exprtk_class_lookup_method_typed(instance->klass, method_name, 0, argc, args);
        if (!method) {
            result = throw_error(env, NULL, "Instance has no method '%s'", method_name);
        } else if (!can_access_method(env, method, object_node)) {
            result = throw_method_access_error(env, object_node, method_name, method);
        } else {
            result = eval_instance_script_method(instance, method, argc, args, env);
        }
    } else if (object.type == EXPRTK_VAL_CLASS) {
        exprtk_class_t *klass = object.data.class_val.klass;
        exprtk_func_t *method =
            exprtk_class_lookup_method_typed(klass, method_name, 1, argc, args);
        if (!method) {
            result = throw_error(env, NULL, "Class has no static method '%s'", method_name);
        } else if (!can_access_method(env, method, object_node)) {
            result = throw_method_access_error(env, object_node, method_name, method);
        } else {
            result = eval_script_function(method, argc, args, env, env);
        }
    } else {
        result = throw_error(env, NULL, "Method call '%s' is invalid for %s", method_name,
                             type_name(object.type));
    }

    return result;
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_checked_value_nodes(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    const exprtk_node_t *call_node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!call_node || call_node->type != EXPRTK_NODE_MEMBER_CALL || !env) return zero;

    size_t actual_count = 0;
    exprtk_value_t *args = eval_expand_args(call_node->data.member_call.args,
                                            call_node->data.member_call.arg_count,
                                            env, &actual_count);
    if (!args && actual_count == 0) return zero;

    exprtk_value_t result = exprtk_oop_call_method_checked_values(
        object_name, method_name, object_node, actual_count, args, env);
    free(args);
    return result;
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_numeric(const char *object_name,
                                                        const char *method_name, size_t argc,
                                                        const double *argv, exprtk_env_t *env) {
    return exprtk_oop_call_method_checked_numeric(object_name, method_name, NULL, argc, argv,
                                                  env);
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_mono_checked_numeric(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const exprtk_node_t *object_node, size_t argc, const double *argv, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !expected_class_name || !method_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);
    exprtk_class_t *expected_class = NULL;
    exprtk_value_t class_value = exprtk_env_get(env, expected_class_name);
    if (class_value.type == EXPRTK_VAL_CLASS) expected_class = class_value.data.class_val.klass;

    if (object.type == EXPRTK_VAL_INSTANCE && expected_class &&
        object.data.instance_val.instance->klass == expected_class) {
        exprtk_value_t *args = numeric_args_to_values(argc, argv);
        if (argc > 0 && !args) return zero;

        exprtk_func_t *method =
            exprtk_class_lookup_method_typed(expected_class, method_name, 0, argc, args);
        if (!method) {
            free(args);
            return throw_error(env, NULL, "Instance has no method '%s'", method_name);
        }
        if (!can_access_method(env, method, object_node)) {
            free(args);
            return throw_method_access_error(env, object_node, method_name, method);
        }

        exprtk_value_t result =
            eval_instance_script_method(object.data.instance_val.instance, method, argc, args, env);
        free(args);
        return result;
    }

    return exprtk_oop_call_method_checked_numeric(object_name, method_name, object_node, argc,
                                                  argv, env);
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_mono_checked_value_nodes(
    const char *object_name, const char *expected_class_name, const char *method_name,
    const exprtk_node_t *object_node, const exprtk_node_t *call_node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !expected_class_name || !method_name || !call_node || !env) return zero;
    if (call_node->type != EXPRTK_NODE_MEMBER_CALL) return zero;

    size_t actual_count = 0;
    exprtk_value_t *args = eval_expand_args(call_node->data.member_call.args,
                                            call_node->data.member_call.arg_count,
                                            env, &actual_count);
    if (!args && actual_count == 0) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);
    exprtk_class_t *expected_class = NULL;
    exprtk_value_t class_value = exprtk_env_get(env, expected_class_name);
    if (class_value.type == EXPRTK_VAL_CLASS) expected_class = class_value.data.class_val.klass;

    exprtk_value_t result = zero;
    if (object.type == EXPRTK_VAL_INSTANCE && expected_class &&
        object.data.instance_val.instance->klass == expected_class) {
        exprtk_func_t *method =
            exprtk_class_lookup_method_typed(expected_class, method_name, 0,
                                             actual_count, args);
        if (!method) {
            result = throw_error(env, NULL, "Instance has no method '%s'", method_name);
        } else if (!can_access_method(env, method, object_node)) {
            result = throw_method_access_error(env, object_node, method_name, method);
        } else {
            result = eval_instance_script_method(object.data.instance_val.instance, method,
                                                 actual_count, args, env);
        }
    } else {
        result = exprtk_oop_call_method_checked_values(object_name, method_name, object_node,
                                                       actual_count, args, env);
    }

    free(args);
    return result;
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_mono_numeric(const char *object_name,
                                                              const char *expected_class_name,
                                                              const char *method_name, size_t argc,
                                                              const double *argv,
                                                              exprtk_env_t *env) {
    return exprtk_oop_call_method_mono_checked_numeric(object_name, expected_class_name,
                                                       method_name, NULL, argc, argv, env);
}

static uint64_t oop_method_arg_signature(size_t argc, const exprtk_value_t *args) {
    uint64_t hash = 1469598103934665603ULL;
    hash ^= (uint64_t)argc;
    hash *= 1099511628211ULL;

    for (size_t i = 0; i < argc; ++i) {
        uint64_t part = (uint64_t)args[i].type;
        if (args[i].type == EXPRTK_VAL_INSTANCE && args[i].data.instance_val.instance &&
            args[i].data.instance_val.instance->klass) {
            part = (part << 32) ^ args[i].data.instance_val.instance->klass->type_id;
        } else if (args[i].type == EXPRTK_VAL_CLASS && args[i].data.class_val.klass) {
            part = (part << 32) ^ args[i].data.class_val.klass->type_id;
        }
        hash ^= part;
        hash *= 1099511628211ULL;
    }

    return hash ? hash : 1;
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_cached_checked_numeric(
    const char *object_name, const char *method_name, exprtk_oop_method_cache_t *cache,
    const exprtk_node_t *object_node, size_t argc, const double *argv, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !method_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);

    exprtk_class_t *klass = NULL;
    int is_static = 0;
    if (object.type == EXPRTK_VAL_INSTANCE) {
        klass = object.data.instance_val.instance->klass;
    } else if (object.type == EXPRTK_VAL_CLASS) {
        klass = object.data.class_val.klass;
        is_static = 1;
    } else {
        return exprtk_oop_call_method_checked_numeric(object_name, method_name, object_node,
                                                      argc, argv, env);
    }

    exprtk_value_t stack_args[8];
    int args_need_free = 0;
    exprtk_value_t *args =
        numeric_args_to_values_buffer(argc, argv, stack_args, 8, &args_need_free);
    if (argc > 0 && !args) return zero;
    uint64_t arg_signature = oop_method_arg_signature(argc, args);

    exprtk_func_t *method = NULL;
    if (cache && cache->last_class == klass && cache->is_static == is_static &&
        cache->argc == argc && cache->arg_signature == arg_signature) {
        method = cache->method;
    }
    if (!method) {
        method = exprtk_class_lookup_method_typed(klass, method_name, is_static, argc, args);
        if (cache) {
            cache->last_class = klass;
            cache->method = method;
            cache->arg_signature = arg_signature;
            cache->argc = argc;
            cache->is_static = is_static;
        }
    }
    if (!method) {
        if (args_need_free) free(args);
        if (is_static) return throw_error(env, NULL, "Class has no static method '%s'", method_name);
        return throw_error(env, NULL, "Instance has no method '%s'", method_name);
    }
    if (!can_access_method(env, method, object_node)) {
        if (args_need_free) free(args);
        return throw_method_access_error(env, object_node, method_name, method);
    }

    exprtk_value_t result = zero;
    if (is_static) {
        result = eval_script_function(method, argc, args, env, env);
    } else {
        result = eval_instance_script_method(object.data.instance_val.instance, method, argc, args, env);
    }
    if (args_need_free) free(args);
    return result;
}

CXX_C_API exprtk_value_t exprtk_oop_call_method_cached_numeric(
    const char *object_name, const char *method_name, exprtk_oop_method_cache_t *cache,
    size_t argc, const double *argv, exprtk_env_t *env) {
    return exprtk_oop_call_method_cached_checked_numeric(object_name, method_name, cache, NULL,
                                                         argc, argv, env);
}

CXX_C_API exprtk_value_t exprtk_oop_call_bound_method(exprtk_value_t bound_method, size_t argc,
                                                      exprtk_value_t *args, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (bound_method.type != EXPRTK_VAL_BOUND_METHOD || !env) return zero;

    return eval_instance_script_method(bound_method.data.bound_method_val.instance,
                                       bound_method.data.bound_method_val.method,
                                       argc, args, env);
}

CXX_C_API exprtk_value_t exprtk_call_function_value(exprtk_value_t function_value, size_t argc,
                                                    exprtk_value_t *args, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (function_value.type != EXPRTK_VAL_FUNCTION ||
        !function_value.data.function.body || !env) {
        return zero;
    }

    exprtk_func_t func;
    memset(&func, 0, sizeof(func));
    func.name = "<function>";
    func.is_script = 1;
    func.owner_class = function_value.data.function.owner_class;
    func.closure_env = function_value.data.function.closure_env;
    func.is_static_method = function_value.data.function.is_static_method;
    func.access_level = function_value.data.function.access_level;
    func.data.script.arg_params = function_value.data.function.arg_params;
    func.data.script.arg_count = function_value.data.function.arg_count;
    func.data.script.body = function_value.data.function.body;

    exprtk_env_t *parent_env =
        function_value.data.function.closure_env ? function_value.data.function.closure_env : env;
    return eval_script_function(&func, argc, args, parent_env, env);
}

CXX_C_API exprtk_value_t exprtk_oop_get_member_checked(const char *object_name,
                                                       const char *member_name,
                                                       const exprtk_node_t *object_node,
                                                       exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !member_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);
    if (object.type == EXPRTK_VAL_INSTANCE) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        exprtk_value_t field_value;
        exprtk_instance_t *instance = object.data.instance_val.instance;
        if (exprtk_instance_get_field(instance, member_name, &field_value)) {
            exprtk_class_t *field_owner = NULL;
            int access = exprtk_class_get_instance_field_access(instance->klass, member_name,
                                                                &field_owner);
            if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            return field_value;
        }

        exprtk_func_t *method = exprtk_instance_get_method(instance, member_name);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            return exprtk_val_bound_method(instance, method);
        }
        return throw_error(env, NULL, "Instance has no field or method '%s'", member_name);
    }
    if (object.type == EXPRTK_VAL_CLASS) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        exprtk_value_t static_field;
        exprtk_class_t *klass = object.data.class_val.klass;
        if (exprtk_class_get_static_field(klass, member_name, &static_field)) {
            exprtk_class_t *field_owner = NULL;
            int access = exprtk_class_get_static_field_access(klass, member_name, &field_owner);
            if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            return static_field;
        }

        exprtk_func_t *method = exprtk_class_lookup_method(klass, member_name, 1);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            exprtk_value_t func_val = { EXPRTK_VAL_FUNCTION, {0} };
            func_val.data.function.arg_params = method->data.script.arg_params;
            func_val.data.function.arg_count = method->data.script.arg_count;
            func_val.data.function.body = method->data.script.body;
            func_val.data.function.closure_env = method->closure_env;
            func_val.data.function.owner_class = method->owner_class;
            func_val.data.function.is_static_method = 1;
            func_val.data.function.access_level = method->access_level;
            return func_val;
        }
        return throw_error(env, NULL, "Class has no static field or method '%s'", member_name);
    }
    return zero;
}

CXX_C_API exprtk_value_t exprtk_oop_get_member_cached_checked(
    const char *object_name, const char *member_name, exprtk_oop_field_cache_t *cache,
    const exprtk_node_t *object_node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !member_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);

    if (object.type == EXPRTK_VAL_INSTANCE) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        exprtk_instance_t *instance = object.data.instance_val.instance;
        if (cache && !cache->is_static && cache->owner == instance &&
            cache->version == instance->field_version && cache->slot) {
            if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                           object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name,
                                                cache->access_level);
            }
            return *cache->slot;
        }

        exprtk_class_t *field_owner = NULL;
        int access = exprtk_class_get_instance_field_access(instance->klass, member_name,
                                                            &field_owner);
        exprtk_value_t *slot = exprtk_instance_get_field_slot(instance, member_name);
        if (slot) {
            if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            if (cache) {
                cache->owner = instance;
                cache->field_owner = field_owner;
                cache->version = instance->field_version;
                cache->slot = slot;
                cache->access_level = access;
                cache->is_static = 0;
            }
            return *slot;
        }

        exprtk_func_t *method = exprtk_instance_get_method(instance, member_name);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            return exprtk_val_bound_method(instance, method);
        }
        return throw_error(env, NULL, "Instance has no field or method '%s'", member_name);
    }

    if (object.type == EXPRTK_VAL_CLASS) {
        if (is_private_member_name(member_name) &&
            !can_access_private_member(env, object_node)) {
            return throw_error(env, object_node, "Private member '%s' is not accessible",
                               member_name);
        }

        exprtk_class_t *klass = object.data.class_val.klass;
        exprtk_class_t *owner = NULL;
        if (cache && cache->is_static && cache->owner) {
            exprtk_class_t *cached_owner = (exprtk_class_t *)cache->owner;
            if (cache->version == cached_owner->static_field_version && cache->slot) {
                if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                               object_node, member_name)) {
                    return throw_field_access_error(env, object_node, member_name,
                                                    cache->access_level);
                }
                return *cache->slot;
            }
        }

        exprtk_value_t *slot =
            exprtk_class_get_static_field_slot(klass, member_name, &owner);
        if (slot) {
            int access = exprtk_class_get_static_field_access(klass, member_name, &owner);
            if (!can_access_declared_field(env, owner, access, object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name, access);
            }
            if (cache) {
                cache->owner = owner;
                cache->field_owner = owner;
                cache->version = owner ? owner->static_field_version : 0;
                cache->slot = slot;
                cache->access_level = access;
                cache->is_static = 1;
            }
            return *slot;
        }

        exprtk_func_t *method = exprtk_class_lookup_method(klass, member_name, 1);
        if (method) {
            if (!can_access_method(env, method, object_node)) {
                return throw_method_access_error(env, object_node, member_name, method);
            }
            exprtk_value_t func_val = { EXPRTK_VAL_FUNCTION, {0} };
            func_val.data.function.arg_params = method->data.script.arg_params;
            func_val.data.function.arg_count = method->data.script.arg_count;
            func_val.data.function.body = method->data.script.body;
            func_val.data.function.closure_env = method->closure_env;
            func_val.data.function.owner_class = method->owner_class;
            func_val.data.function.is_static_method = 1;
            func_val.data.function.access_level = method->access_level;
            return func_val;
        }
        return throw_error(env, NULL, "Class has no static field or method '%s'", member_name);
    }

    return exprtk_oop_get_member_checked(object_name, member_name, object_node, env);
}

CXX_C_API exprtk_value_t exprtk_oop_get_member(const char *object_name, const char *member_name,
                                               exprtk_env_t *env) {
    return exprtk_oop_get_member_checked(object_name, member_name, NULL, env);
}

CXX_C_API exprtk_value_t exprtk_oop_set_member_checked_numeric(
    const char *object_name, const char *member_name, double value,
    const exprtk_node_t *object_node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !member_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);

    exprtk_value_t field_value = exprtk_val_num(value);
    if (exprtk_value_is_object_like(&object)) {
        exprtk_map_set(&object, member_name, field_value);
        exprtk_env_set(env, object_name, object);
        return field_value;
    }
    if (object.type == EXPRTK_VAL_INSTANCE) {
        exprtk_instance_t *instance = object.data.instance_val.instance;
        exprtk_class_t *field_owner = NULL;
        int access = exprtk_class_get_instance_field_access(instance->klass, member_name,
                                                            &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }

        exprtk_instance_set_field(instance, member_name, field_value);
        return field_value;
    }
    if (object.type == EXPRTK_VAL_CLASS) {
        exprtk_class_t *klass = object.data.class_val.klass;
        exprtk_class_t *field_owner = NULL;
        int access = exprtk_class_get_static_field_access(klass, member_name, &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }

        exprtk_class_set_static_field(klass, member_name, field_value);
        return field_value;
    }
    return zero;
}

CXX_C_API exprtk_value_t exprtk_oop_set_member_cached_checked_numeric(
    const char *object_name, const char *member_name, double value,
    exprtk_oop_field_cache_t *cache, const exprtk_node_t *object_node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !member_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);

    exprtk_value_t field_value = exprtk_val_num(value);
    if (exprtk_value_is_object_like(&object)) {
        exprtk_map_set(&object, member_name, field_value);
        exprtk_env_set(env, object_name, object);
        return field_value;
    }

    if (object.type == EXPRTK_VAL_INSTANCE) {
        exprtk_instance_t *instance = object.data.instance_val.instance;
        if (cache && !cache->is_static && cache->owner == instance &&
            cache->version == instance->field_version && cache->slot) {
            if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                           object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name,
                                                cache->access_level);
            }
            *cache->slot = field_value;
            return field_value;
        }

        exprtk_class_t *field_owner = NULL;
        int access = exprtk_class_get_instance_field_access(instance->klass, member_name,
                                                            &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }
        exprtk_value_t *slot = exprtk_instance_get_field_slot(instance, member_name);
        if (slot) {
            *slot = field_value;
        } else {
            exprtk_instance_set_field(instance, member_name, field_value);
            slot = exprtk_instance_get_field_slot(instance, member_name);
        }
        if (cache && slot) {
            cache->owner = instance;
            cache->field_owner = field_owner;
            cache->version = instance->field_version;
            cache->slot = slot;
            cache->access_level = access;
            cache->is_static = 0;
        }
        return field_value;
    }

    if (object.type == EXPRTK_VAL_CLASS) {
        exprtk_class_t *klass = object.data.class_val.klass;
        if (cache && cache->is_static && cache->owner == klass &&
            cache->version == klass->static_field_version && cache->slot) {
            if (!can_access_declared_field(env, cache->field_owner, cache->access_level,
                                           object_node, member_name)) {
                return throw_field_access_error(env, object_node, member_name,
                                                cache->access_level);
            }
            *cache->slot = field_value;
            return field_value;
        }

        exprtk_class_t *field_owner = NULL;
        int access = exprtk_class_get_static_field_access(klass, member_name, &field_owner);
        if (!can_access_declared_field(env, field_owner, access, object_node, member_name)) {
            return throw_field_access_error(env, object_node, member_name, access);
        }
        exprtk_class_set_static_field(klass, member_name, field_value);
        exprtk_value_t *slot = exprtk_class_get_static_field_slot(klass, member_name, NULL);
        if (cache && slot) {
            cache->owner = klass;
            cache->field_owner = field_owner;
            cache->version = klass->static_field_version;
            cache->slot = slot;
            cache->access_level = access;
            cache->is_static = 1;
        }
        return field_value;
    }

    return zero;
}

CXX_C_API exprtk_value_t exprtk_oop_set_member_numeric(const char *object_name,
                                                       const char *member_name, double value,
                                                       exprtk_env_t *env) {
    return exprtk_oop_set_member_checked_numeric(object_name, member_name, value, NULL, env);
}

CXX_C_API exprtk_value_t exprtk_oop_instanceof_name(const char *object_name,
                                                    const char *class_name, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !class_name || !env) return zero;

    exprtk_value_t object = exprtk_env_get(env, object_name);
    exprtk_class_t *klass = NULL;
    exprtk_value_t class_value = exprtk_env_get(env, class_name);
    if (class_value.type == EXPRTK_VAL_CLASS) klass = class_value.data.class_val.klass;
    if (object.type != EXPRTK_VAL_INSTANCE || !klass) return exprtk_val_num(0.0);
    return exprtk_val_num((double)exprtk_instance_of(object.data.instance_val.instance, klass));
}

/* =========================================================================
 * MEMBER_CALL dispatch helpers
 * ========================================================================= */



/* Helper: get mutable variable from env when obj_node is a VARIABLE */
static int mc_get_var(mc_ctx_t *mc, exprtk_value_t *out, const char **name) {
    if (!mc || !mc->obj_node || mc->obj_node->type != EXPRTK_NODE_VARIABLE) return 0;
    *name = mc->obj_node->data.variable.name;
    *out = exprtk_env_get(mc->env, *name);
    return 1;
}

static exprtk_value_t *mc_prepare_call_args(mc_ctx_t *mc, exprtk_value_t *stack_args, size_t stack_cap) {
    size_t call_argc = mc->argc + 1;
    exprtk_value_t *call_args = stack_args;

    if (call_argc > stack_cap) {
        call_args = (exprtk_value_t*)malloc(call_argc * sizeof(exprtk_value_t));
        if (!call_args) return NULL;
    }

    call_args[0] = mc->obj;
    for (size_t i = 0; i < mc->argc; ++i) {
        call_args[i + 1] = mc->args[i];
        if (call_args[i + 1].type == EXPRTK_VAL_INTEGER) {
            call_args[i + 1] = exprtk_val_num((double)call_args[i + 1].data.integer);
        }
    }
    return call_args;
}

static exprtk_value_t unknown_method_error(mc_ctx_t *mc, const char *type_name_str) {
    return throw_error(mc ? mc->env : NULL, mc ? mc->obj_node : NULL,
                       "Unknown %s method '%s'",
                       type_name_str ? type_name_str : "object",
                       (mc && mc->method) ? mc->method : "<null>");
}

static int eval_callable_value(exprtk_value_t callee, const char *name, size_t argc,
                               exprtk_value_t *args, exprtk_env_t *env,
                               exprtk_value_t *out) {
    if (!out) return 0;

    if (callee.type == EXPRTK_VAL_CLASS) {
        *out = exprtk_oop_instantiate_class_value(callee, name, argc, args, env);
        return 1;
    }
    if (callee.type == EXPRTK_VAL_BOUND_METHOD) {
        *out = eval_instance_script_method(callee.data.bound_method_val.instance,
                                           callee.data.bound_method_val.method,
                                           argc, args, env);
        return 1;
    }
    if (callee.type == EXPRTK_VAL_FUNCTION && callee.data.function.body) {
        *out = exprtk_call_function_value(callee, argc, args, env);
        return 1;
    }

    return 0;
}

static int rt_dispatch_provider_method(mc_ctx_t *mc, exprtk_value_t *out) {
    exprtk_value_t provider;
    char full_name[160];
    exprtk_value_t stack_args[8];
    exprtk_value_t *call_args;
    size_t call_argc;

    if (!mc || !out || !mc->method || !mc->env ||
        !exprtk_value_is_object_like(&mc->obj) ||
        !exprtk_map_has(&mc->obj, "__ts_method_provider")) {
        return 0;
    }

    provider = exprtk_map_get(&mc->obj, "__ts_method_provider");
    if (provider.type != EXPRTK_VAL_STRING || !provider.data.string.data ||
        provider.data.string.len == 0) {
        return 0;
    }

    if (provider.data.string.len + strlen(mc->method) + 2 >= sizeof(full_name))
        return 0;

    snprintf(full_name, sizeof(full_name), "%.*s.%s",
             (int)provider.data.string.len, provider.data.string.data, mc->method);
    if (!exprtk_env_has_func(mc->env, full_name) && !exprtk_find_builtin(full_name, mc->env))
        return 0;

    call_argc = mc->argc + 1;
    call_args = mc_prepare_call_args(mc, stack_args, sizeof(stack_args) / sizeof(stack_args[0]));
    if (!call_args) return 0;

    *out = exprtk_call_internal(full_name, call_argc, call_args, mc->env, mc->arena);
    if (call_args != stack_args) free(call_args);
    return 1;
}

extern exprtk_value_t exprtk_stream_member_call(exprtk_value_t stream, const char *method,
                                                size_t argc, exprtk_value_t *args,
                                                exprtk_env_t *env, mem_pool_t *arena);

static exprtk_value_t rt_stream_make(exprtk_value_t source) {
    exprtk_value_t stream = exprtk_val_map();
    exprtk_map_set(&stream, "__ts_stream_kind",
                   exprtk_val_str(tstr_v_from_cstr("TurboScript.Stream.v1")));
    exprtk_map_set(&stream, "source", source);
    return stream;
}

static int rt_stream_is_value(exprtk_value_t stream) {
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

static exprtk_value_t rt_stream_map_entries(exprtk_value_t source) {
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

static exprtk_value_t rt_stream_string_lines(exprtk_value_t text, mem_pool_t *arena) {
    exprtk_value_t lines = exprtk_val_list_empty();
    size_t start = 0;

    if (text.type != EXPRTK_VAL_STRING || !arena) return lines;
    for (size_t i = 0; i <= text.data.string.len; ++i) {
        if (i == text.data.string.len || text.data.string.data[i] == '\n') {
            size_t end = i;
            char *buf;
            if (end > start && text.data.string.data[end - 1] == '\r') end--;
            buf = (char *)mem_alloc(arena, end - start + 1);
            if (!buf) return lines;
            memcpy(buf, text.data.string.data + start, end - start);
            buf[end - start] = '\0';
            exprtk_list_push(&lines, exprtk_val_str(tstr_v_from_buf(buf, end - start)));
            start = i + 1;
        }
    }
    return lines;
}

exprtk_value_t eval_list_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    exprtk_env_t *env = mc->env;

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(mc->obj);

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.list.count);

    if (mc->obj.type == EXPRTK_VAL_LIST && strcmp(m, "push") == 0 && mc->argc > 0) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == EXPRTK_VAL_LIST) {
            exprtk_list_push(&var, mc->args[0]);
            exprtk_env_set(mc->env, vn, var);
            return exprtk_val_num((double)var.data.list.count);
        }
    }

    if (mc->obj.type == EXPRTK_VAL_LIST && strcmp(m, "pop") == 0 && mc->obj.data.list.count > 0) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == EXPRTK_VAL_LIST && var.data.list.count > 0) {
            exprtk_value_t popped = var.data.list.items[var.data.list.count - 1];
            var.data.list.count--;
            exprtk_env_set(mc->env, vn, var);
            return popped;
        }
    }

    if (strcmp(m, "indexOf") == 0 && mc->argc > 0) {
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            exprtk_value_t item = mc->obj.data.list.items[i];
            if (values_match(item, mc->args[0]))
                return exprtk_val_num((double)i);
        }
        return exprtk_val_num(-1);
    }

    if (strcmp(m, "contains") == 0 && mc->argc > 0) {
        for (size_t i = 0; i < mc->obj.data.list.count; ++i) {
            exprtk_value_t item = mc->obj.data.list.items[i];
            if (values_match(item, mc->args[0]))
                return exprtk_val_num(1);
        }
        return exprtk_val_num(0.0);
    }

    if (strcmp(m, "reverse") == 0) {
        exprtk_value_t *items = env
            ? (exprtk_value_t*)mem_alloc(&env->arena, mc->obj.data.list.count * sizeof(exprtk_value_t))
            : (exprtk_value_t*)malloc(mc->obj.data.list.count * sizeof(exprtk_value_t));
        if (items) {
            for (size_t i = 0; i < mc->obj.data.list.count; ++i)
                items[i] = mc->obj.data.list.items[mc->obj.data.list.count - 1 - i];
            return exprtk_val_list_ex(items, mc->obj.data.list.count, env ? 0 : 1);
        }
    }

    return unknown_method_error(mc, mc->obj.type == EXPRTK_VAL_SET ? "set" : "list");
}

exprtk_value_t eval_map_method(mc_ctx_t *mc) {
    const char *m = mc->method;

    if (rt_stream_is_value(mc->obj))
        return exprtk_stream_member_call(mc->obj, m, mc->argc, mc->args, mc->env, mc->arena);

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(rt_stream_map_entries(mc->obj));

    if (exprtk_map_has(&mc->obj, m)) {
        exprtk_value_t callee = exprtk_map_get(&mc->obj, m);
        exprtk_value_t result = { EXPRTK_VAL_NUMBER, {0.0} };
        if (eval_callable_value(callee, m, mc->argc, mc->args, mc->env, &result)) {
            return result;
        }
    }

    {
        exprtk_value_t result = { EXPRTK_VAL_NUMBER, {0.0} };
        if (rt_dispatch_provider_method(mc, &result)) {
            return result;
        }
    }

    if (strcmp(m, "size") == 0 || strcmp(m, "length") == 0)
        return exprtk_val_num((double)exprtk_map_count(&mc->obj));

    if (strcmp(m, "keys") == 0) {
        size_t count = exprtk_map_count(&mc->obj);
        exprtk_value_t *items = mc->env
            ? (exprtk_value_t*)mem_alloc(&mc->env->arena, count * sizeof(exprtk_value_t))
            : (exprtk_value_t*)malloc(count * sizeof(exprtk_value_t));
        if (items) {
            exprtk_map_iter_t it = exprtk_map_iter_begin(&mc->obj);
            const char *key; size_t idx = 0;
            while (exprtk_map_iter_next(&it, &key, NULL)) {
                tstr_v sv; sv.data = (char*)key; sv.len = strlen(key);
                items[idx++] = exprtk_val_str(sv);
            }
            return exprtk_val_list_ex(items, idx, mc->env ? 0 : 1);
        }
    }

    if (strcmp(m, "values") == 0) {
        size_t count = exprtk_map_count(&mc->obj);
        exprtk_value_t *items = mc->env
            ? (exprtk_value_t*)mem_alloc(&mc->env->arena, count * sizeof(exprtk_value_t))
            : (exprtk_value_t*)malloc(count * sizeof(exprtk_value_t));
        if (items) {
            exprtk_map_iter_t it = exprtk_map_iter_begin(&mc->obj);
            exprtk_value_t val; size_t idx = 0;
            while (exprtk_map_iter_next(&it, NULL, &val))
                items[idx++] = val;
            return exprtk_val_list_ex(items, idx, mc->env ? 0 : 1);
        }
    }

    if (strcmp(m, "has") == 0 && mc->argc > 0 && mc->args[0].type == EXPRTK_VAL_STRING) {
        char key_buf[256];
        size_t klen = mc->args[0].data.string.len < 255 ? mc->args[0].data.string.len : 255;
        memcpy(key_buf, mc->args[0].data.string.data, klen);
        key_buf[klen] = '\0';
        return exprtk_val_num(exprtk_map_has(&mc->obj, key_buf) ? 1.0 : 0.0);
    }

    if (strcmp(m, "delete") == 0 && mc->argc > 0 && mc->args[0].type == EXPRTK_VAL_STRING) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && exprtk_value_is_object_like(&var)) {
            char key_buf[256];
            size_t klen = mc->args[0].data.string.len < 255 ? mc->args[0].data.string.len : 255;
            memcpy(key_buf, mc->args[0].data.string.data, klen);
            key_buf[klen] = '\0';
            exprtk_value_t r = exprtk_val_num(exprtk_map_delete(&var, key_buf) ? 1.0 : 0.0);
            exprtk_env_set(mc->env, vn, var);
            return r;
        }
    }

    return unknown_method_error(mc, "map");
}

exprtk_value_t eval_string_method(mc_ctx_t *mc) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    const char *m = mc->method;
    exprtk_value_t stack_args[8];

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.string.len);

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(rt_stream_string_lines(mc->obj, mc->arena));

    /* Dispatch to registry: string methods expect (this, ...args) */
    size_t call_argc = mc->argc + 1;
    exprtk_value_t *call_args = mc_prepare_call_args(mc, stack_args, 8);
    if (!call_args) return zero;

    char full_name[128];
    snprintf(full_name, sizeof(full_name), "string.%s", m);
    exprtk_builtin_fn fn = exprtk_registry_find(full_name);
    if (!fn) fn = exprtk_registry_find(m);

    exprtk_value_t result = zero;
    int handled = 0;
    if (fn) {
        result = fn(call_argc, call_args, mc->env, mc->arena);
        handled = 1;
    } else {
        /* Inline implementations for essential methods when registry is empty */
        if (strcmp(m, "indexOf") == 0 && mc->argc > 0 && mc->args[0].type == EXPRTK_VAL_STRING) {
            result = exprtk_val_num(-1);
            if (mc->args[0].data.string.len <= mc->obj.data.string.len) {
                for (size_t i = 0; i <= mc->obj.data.string.len - mc->args[0].data.string.len; ++i) {
                    if (strncmp(mc->obj.data.string.data + i, mc->args[0].data.string.data, mc->args[0].data.string.len) == 0) {
                        result = exprtk_val_num((double)i); break;
                    }
                }
            }
            handled = 1;
        } else if (strcmp(m, "substr") == 0 && mc->argc >= 1) {
            int start = (int)val_to_double(mc->args[0]);
            int len = (mc->argc >= 2) ? (int)val_to_double(mc->args[1]) : (int)(mc->obj.data.string.len - start);
            if (start < 0) start = 0;
            if (start >= (int)mc->obj.data.string.len) { start = 0; len = 0; }
            if (start + len > (int)mc->obj.data.string.len) len = (int)mc->obj.data.string.len - start;
            if (len < 0) len = 0;
            char *buf = (char*)mem_alloc(mc->arena, len + 1);
            memcpy(buf, mc->obj.data.string.data + start, len);
            buf[len] = '\0';
            tstr_v sv; sv.data = buf; sv.len = len;
            result = exprtk_val_str(sv);
            handled = 1;
        } else if (strcmp(m, "toUpper") == 0) {
            char *buf = (char*)mem_alloc(mc->arena, mc->obj.data.string.len + 1);
            for (size_t i = 0; i < mc->obj.data.string.len; ++i)
                buf[i] = (char)toupper((unsigned char)mc->obj.data.string.data[i]);
            buf[mc->obj.data.string.len] = '\0';
            tstr_v sv; sv.data = buf; sv.len = mc->obj.data.string.len;
            result = exprtk_val_str(sv);
            handled = 1;
        } else if (strcmp(m, "toLower") == 0) {
            char *buf = (char*)mem_alloc(mc->arena, mc->obj.data.string.len + 1);
            for (size_t i = 0; i < mc->obj.data.string.len; ++i)
                buf[i] = (char)tolower((unsigned char)mc->obj.data.string.data[i]);
            buf[mc->obj.data.string.len] = '\0';
            tstr_v sv; sv.data = buf; sv.len = mc->obj.data.string.len;
            result = exprtk_val_str(sv);
            handled = 1;
        }
    }
    if (call_args != stack_args) free(call_args);
    if (!handled) return unknown_method_error(mc, "string");
    return result;
}

exprtk_value_t eval_bytes_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.bytes.len);
    return unknown_method_error(mc, "bytes");
}

exprtk_value_t eval_uuid_method(mc_ctx_t *mc) {
    char text[TURBO_UUID_STRING_SIZE];
    char *buf;
    size_t len;

    if (strcmp(mc->method, "toString") != 0 && strcmp(mc->method, "to_string") != 0)
        return unknown_method_error(mc, "uuid");
    if (turbo_uuid_format(&mc->obj.data.uuid, text, sizeof(text)) != TURBO_OK)
        return exprtk_val_num(0);
    len = strlen(text);
    buf = (char *)mem_alloc(mc->arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, text, len + 1);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

static exprtk_value_t runtime_string_value(mem_pool_t *arena, const char *text) {
    size_t len;
    char *buf;
    if (!text) text = "";
    len = strlen(text);
    buf = (char *)mem_alloc(arena, len + 1);
    if (!buf) return exprtk_val_num(0);
    memcpy(buf, text, len + 1);
    return exprtk_val_str(tstr_v_from_buf(buf, len));
}

int exprtk_datetime_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    const turbo_datetime_t *dt;
    time_t ts;
    if (!out || value.type != EXPRTK_VAL_DATETIME || !member) return 0;
    dt = &value.data.datetime;
    if (strcmp(member, "year") == 0) *out = exprtk_val_int(dt->year);
    else if (strcmp(member, "month") == 0) *out = exprtk_val_int(dt->month);
    else if (strcmp(member, "day") == 0) *out = exprtk_val_int(dt->day);
    else if (strcmp(member, "hour") == 0) *out = exprtk_val_int(dt->hour);
    else if (strcmp(member, "minute") == 0) *out = exprtk_val_int(dt->minute);
    else if (strcmp(member, "second") == 0) *out = exprtk_val_int(dt->second);
    else if (strcmp(member, "millisecond") == 0) *out = exprtk_val_int(dt->millisecond);
    else if (strcmp(member, "tz_offset") == 0) *out = exprtk_val_int(dt->tz_offset);
    else if (strcmp(member, "has_tz") == 0) *out = exprtk_val_bool(dt->has_tz != 0);
    else if (strcmp(member, "day_of_week") == 0) *out = exprtk_val_int(dt->day_of_week);
    else if (strcmp(member, "timestamp") == 0) {
        ts = turbo_datetime_to_time(dt);
        *out = exprtk_val_num((double)ts);
    } else {
        return 0;
    }
    return 1;
}

exprtk_value_t eval_datetime_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    time_t ts;
    char buf[64];
    if (strcmp(m, "timestamp") == 0 || strcmp(m, "to_time") == 0) {
        ts = turbo_datetime_to_time(&mc->obj.data.datetime);
        return exprtk_val_num((double)ts);
    }
    if (strcmp(m, "toString") == 0 || strcmp(m, "to_string") == 0 ||
        strcmp(m, "format_rfc822") == 0) {
        ts = turbo_datetime_to_time(&mc->obj.data.datetime);
        if (ts == (time_t)-1 || turbo_datetime_format_rfc822(ts, buf, sizeof(buf)) < 0)
            return exprtk_val_str(tstr_v_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "datetime");
}

static int runtime_offset_datetime_text(exprtk_offset_datetime_t value,
                                        char *out, size_t out_size) {
    int offset = value.offset_minutes;
    char sign = '+';
    int hours;
    int minutes;

    if (!out || out_size == 0) return 0;
    if (offset < 0) {
        sign = '-';
        offset = -offset;
    }
    hours = offset / 60;
    minutes = offset % 60;
    return snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d%s%c%02d:%02d",
                    value.datetime.year, value.datetime.month, value.datetime.day,
                    value.datetime.hour, value.datetime.minute, value.datetime.second,
                    value.datetime.millisecond > 0 ? "" : "", sign, hours, minutes) > 0;
}

int exprtk_offset_datetime_member_get(exprtk_value_t value, const char *member,
                                      exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_OFFSET_DATETIME || !member) return 0;
    if (strcmp(member, "offset_minutes") == 0 || strcmp(member, "tz_offset") == 0)
        *out = exprtk_val_int(value.data.offset_datetime.offset_minutes);
    else if (strcmp(member, "datetime") == 0)
        *out = exprtk_val_datetime(value.data.offset_datetime.datetime);
    else {
        exprtk_value_t datetime_value = exprtk_val_datetime(value.data.offset_datetime.datetime);
        return exprtk_datetime_member_get(datetime_value, member, out);
    }
    return 1;
}

exprtk_value_t eval_offset_datetime_method(mc_ctx_t *mc) {
    char buf[64];
    if (strcmp(mc->method, "timestamp") == 0 || strcmp(mc->method, "to_time") == 0)
        return exprtk_val_num((double)turbo_datetime_to_time(&mc->obj.data.offset_datetime.datetime));
    if (strcmp(mc->method, "toString") == 0 || strcmp(mc->method, "to_string") == 0) {
        if (!runtime_offset_datetime_text(mc->obj.data.offset_datetime, buf, sizeof(buf)))
            return exprtk_val_str(tstr_v_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "offset_datetime");
}

static int runtime_date_text(exprtk_date_t date, char *out, size_t out_size) {
    return out && out_size > 0 &&
           snprintf(out, out_size, "%04d-%02d-%02d", date.year, date.month, date.day) > 0;
}

static int runtime_time_text(exprtk_time_t time, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    if (time.millisecond > 0)
        return snprintf(out, out_size, "%02d:%02d:%02d.%03d", time.hour, time.minute,
                        time.second, time.millisecond) > 0;
    return snprintf(out, out_size, "%02d:%02d:%02d", time.hour, time.minute,
                    time.second) > 0;
}

static int runtime_duration_text(int64_t ms, char *out, size_t out_size) {
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

static int runtime_decimal_text(exprtk_decimal_t value, char *out, size_t out_size) {
    char digits[32];
    char *p = digits + sizeof(digits);
    uint64_t mag;
    size_t digit_count;
    size_t pos = 0;
    int negative;

    if (!out || out_size == 0 || value.scale < 0) return 0;
    while (value.scale > 0 && value.mantissa % 10 == 0) {
        value.mantissa /= 10;
        value.scale--;
    }
    if (value.mantissa == 0) value.scale = 0;
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

int exprtk_date_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_DATE || !member) return 0;
    if (strcmp(member, "year") == 0) *out = exprtk_val_int(value.data.date.year);
    else if (strcmp(member, "month") == 0) *out = exprtk_val_int(value.data.date.month);
    else if (strcmp(member, "day") == 0) *out = exprtk_val_int(value.data.date.day);
    else return 0;
    return 1;
}

int exprtk_time_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_TIME || !member) return 0;
    if (strcmp(member, "hour") == 0) *out = exprtk_val_int(value.data.time.hour);
    else if (strcmp(member, "minute") == 0) *out = exprtk_val_int(value.data.time.minute);
    else if (strcmp(member, "second") == 0) *out = exprtk_val_int(value.data.time.second);
    else if (strcmp(member, "millisecond") == 0) *out = exprtk_val_int(value.data.time.millisecond);
    else return 0;
    return 1;
}

int exprtk_duration_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_DURATION || !member) return 0;
    if (strcmp(member, "milliseconds") == 0 || strcmp(member, "ms") == 0)
        *out = exprtk_val_int(value.data.duration_ms);
    else if (strcmp(member, "seconds") == 0)
        *out = exprtk_val_num((double)value.data.duration_ms / 1000.0);
    else return 0;
    return 1;
}

int exprtk_decimal_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_DECIMAL || !member) return 0;
    if (strcmp(member, "mantissa") == 0)
        *out = exprtk_val_int(value.data.decimal.mantissa);
    else if (strcmp(member, "scale") == 0)
        *out = exprtk_val_int(value.data.decimal.scale);
    else return 0;
    return 1;
}

int exprtk_money_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_MONEY || !member) return 0;
    if (strcmp(member, "amount") == 0) *out = exprtk_val_decimal(value.data.money.amount);
    else if (strcmp(member, "currency") == 0) {
        tstr_v sv;
        sv.data = value.data.money.currency;
        sv.len = 3;
        *out = exprtk_val_str(sv);
    } else return 0;
    return 1;
}

int exprtk_enum_member_get(exprtk_value_t value, const char *member, exprtk_value_t *out) {
    if (!out || (value.type != EXPRTK_VAL_ENUM && value.type != EXPRTK_VAL_FLAGS) || !member)
        return 0;
    if (strcmp(member, "value") == 0) *out = exprtk_val_int(value.data.enum_val.value);
    else if (strcmp(member, "name") == 0 || strcmp(member, "symbol") == 0)
        *out = exprtk_val_str(value.data.enum_val.symbol);
    else if (strcmp(member, "type") == 0)
        *out = exprtk_val_str(value.data.enum_val.type_name);
    else if (strcmp(member, "is_flags") == 0)
        *out = exprtk_val_bool(value.type == EXPRTK_VAL_FLAGS);
    else return 0;
    return 1;
}

static const char *runtime_typed_array_kind_name(exprtk_typed_array_kind_t kind) {
    switch (kind) {
        case EXPRTK_TYPED_I32: return "i32";
        case EXPRTK_TYPED_I64: return "i64";
        case EXPRTK_TYPED_F32: return "f32";
        case EXPRTK_TYPED_F64: return "f64";
        default: return "";
    }
}

exprtk_value_t exprtk_typed_array_get_value(exprtk_value_t value, size_t index) {
    if (value.type != EXPRTK_VAL_TYPED_ARRAY || !value.data.typed_array.data ||
        index >= value.data.typed_array.count)
        return exprtk_val_num(0);
    switch (value.data.typed_array.kind) {
        case EXPRTK_TYPED_I32:
            return exprtk_val_int(((const int32_t *)value.data.typed_array.data)[index]);
        case EXPRTK_TYPED_I64:
            return exprtk_val_int(((const int64_t *)value.data.typed_array.data)[index]);
        case EXPRTK_TYPED_F32:
            return exprtk_val_num((double)((const float *)value.data.typed_array.data)[index]);
        case EXPRTK_TYPED_F64:
            return exprtk_val_num(((const double *)value.data.typed_array.data)[index]);
        default:
            return exprtk_val_num(0);
    }
}

int exprtk_typed_array_member_get(exprtk_value_t value, const char *member,
                                  exprtk_value_t *out) {
    if (!out || value.type != EXPRTK_VAL_TYPED_ARRAY || !member) return 0;
    if (strcmp(member, "length") == 0 || strcmp(member, "size") == 0)
        *out = exprtk_val_int((int64_t)value.data.typed_array.count);
    else if (strcmp(member, "kind") == 0)
        *out = exprtk_val_str(tstr_v_from_cstr(runtime_typed_array_kind_name(value.data.typed_array.kind)));
    else return 0;
    return 1;
}

exprtk_value_t eval_typed_array_method(mc_ctx_t *mc) {
    const char *m = mc->method;
    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_int((int64_t)mc->obj.data.typed_array.count);
    if (strcmp(m, "kind") == 0)
        return runtime_string_value(mc->arena,
                                    runtime_typed_array_kind_name(mc->obj.data.typed_array.kind));
    if (strcmp(m, "toList") == 0 || strcmp(m, "to_list") == 0) {
        exprtk_value_t list = exprtk_val_list_empty();
        for (size_t i = 0; i < mc->obj.data.typed_array.count; ++i)
            exprtk_list_push(&list, exprtk_typed_array_get_value(mc->obj, i));
        return list;
    }
    if (strcmp(m, "toVector") == 0 || strcmp(m, "to_vector") == 0) {
        double *data = (double *)mem_alloc(mc->arena,
                                           mc->obj.data.typed_array.count * sizeof(double));
        if (!data && mc->obj.data.typed_array.count > 0) return exprtk_val_vec(NULL, 0);
        for (size_t i = 0; i < mc->obj.data.typed_array.count; ++i)
            data[i] = val_to_double(exprtk_typed_array_get_value(mc->obj, i));
        return exprtk_val_vec(data, mc->obj.data.typed_array.count);
    }
    return unknown_method_error(mc, "typed_array");
}

exprtk_value_t eval_date_method(mc_ctx_t *mc) {
    char buf[32];
    if (strcmp(mc->method, "toString") != 0 && strcmp(mc->method, "to_string") != 0)
        return unknown_method_error(mc, "date");
    if (!runtime_date_text(mc->obj.data.date, buf, sizeof(buf)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return runtime_string_value(mc->arena, buf);
}

exprtk_value_t eval_time_method(mc_ctx_t *mc) {
    char buf[32];
    if (strcmp(mc->method, "toString") != 0 && strcmp(mc->method, "to_string") != 0)
        return unknown_method_error(mc, "time");
    if (!runtime_time_text(mc->obj.data.time, buf, sizeof(buf)))
        return exprtk_val_str(tstr_v_from_cstr(""));
    return runtime_string_value(mc->arena, buf);
}

exprtk_value_t eval_duration_method(mc_ctx_t *mc) {
    char buf[64];
    if (strcmp(mc->method, "milliseconds") == 0 || strcmp(mc->method, "ms") == 0)
        return exprtk_val_int(mc->obj.data.duration_ms);
    if (strcmp(mc->method, "seconds") == 0)
        return exprtk_val_num((double)mc->obj.data.duration_ms / 1000.0);
    if (strcmp(mc->method, "toString") == 0 || strcmp(mc->method, "to_string") == 0) {
        if (!runtime_duration_text(mc->obj.data.duration_ms, buf, sizeof(buf)))
            return exprtk_val_str(tstr_v_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "duration");
}

exprtk_value_t eval_decimal_method(mc_ctx_t *mc) {
    char buf[64];
    if (strcmp(mc->method, "mantissa") == 0)
        return exprtk_val_int(mc->obj.data.decimal.mantissa);
    if (strcmp(mc->method, "scale") == 0)
        return exprtk_val_int(mc->obj.data.decimal.scale);
    if (strcmp(mc->method, "toString") == 0 || strcmp(mc->method, "to_string") == 0) {
        if (!runtime_decimal_text(mc->obj.data.decimal, buf, sizeof(buf)))
            return exprtk_val_str(tstr_v_from_cstr(""));
        return runtime_string_value(mc->arena, buf);
    }
    return unknown_method_error(mc, "decimal");
}

exprtk_value_t eval_vector_method(mc_ctx_t *mc) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    const char *m = mc->method;
    exprtk_value_t stack_args[8];

    if (strcmp(m, "length") == 0 || strcmp(m, "size") == 0)
        return exprtk_val_num((double)mc->obj.data.vector.size);

    if (strcmp(m, "stream") == 0)
        return rt_stream_make(mc->obj);

    if (strcmp(m, "push") == 0 && mc->argc > 0 &&
        (mc->args[0].type == EXPRTK_VAL_NUMBER || mc->args[0].type == EXPRTK_VAL_INTEGER)) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == EXPRTK_VAL_VECTOR) {
            size_t new_sz = var.data.vector.size + 1;
            double *new_data = (double*)mem_alloc(mc->arena, new_sz * sizeof(double));
            if (var.data.vector.data)
                memcpy(new_data, var.data.vector.data, var.data.vector.size * sizeof(double));
            new_data[new_sz - 1] = val_to_double(mc->args[0]);
            var.data.vector.data = new_data;
            var.data.vector.size = new_sz;
            exprtk_env_set(mc->env, vn, var);
            return exprtk_val_num((double)new_sz);
        }
    }

    if (strcmp(m, "pop") == 0 && mc->obj.data.vector.size > 0) {
        exprtk_value_t var; const char *vn;
        if (mc_get_var(mc, &var, &vn) && var.type == EXPRTK_VAL_VECTOR && var.data.vector.size > 0) {
            exprtk_value_t popped = exprtk_val_num(var.data.vector.data[var.data.vector.size - 1]);
            var.data.vector.size--;
            exprtk_env_set(mc->env, vn, var);
            return popped;
        }
    }

    if (strcmp(m, "indexOf") == 0 && mc->argc > 0 &&
        (mc->args[0].type == EXPRTK_VAL_NUMBER || mc->args[0].type == EXPRTK_VAL_INTEGER)) {
        double search_val = val_to_double(mc->args[0]);
        for (size_t i = 0; i < mc->obj.data.vector.size; ++i) {
            if (fabs(mc->obj.data.vector.data[i] - search_val) < 1e-9)
                return exprtk_val_num((double)i);
        }
        return exprtk_val_num(-1);
    }

    if (strcmp(m, "reverse") == 0) {
        double *data = (double*)mem_alloc(mc->arena, mc->obj.data.vector.size * sizeof(double));
        for (size_t i = 0; i < mc->obj.data.vector.size; ++i)
            data[i] = mc->obj.data.vector.data[mc->obj.data.vector.size - 1 - i];
        return exprtk_val_vec(data, mc->obj.data.vector.size);
    }

    /* Registry dispatch: try multiple namespace prefixes */
    size_t call_argc = mc->argc + 1;
    exprtk_value_t *call_args = mc_prepare_call_args(mc, stack_args, 8);
    if (!call_args) return zero;

    static const char *prefixes[] = { "vec_", "ta.", "ts.", "stats.", "math.", NULL };
    exprtk_builtin_fn fn = NULL;
    char full_name[128];

    for (const char **p = prefixes; *p && !fn; ++p) {
        snprintf(full_name, sizeof(full_name), "%s%s", *p, m);
        fn = exprtk_find_builtin(full_name, mc->env);
    }
    if (!fn) fn = exprtk_find_builtin(m, mc->env);

    exprtk_value_t result = zero;
    int handled = 0;
    if (fn) {
        result = fn(call_argc, call_args, mc->env, mc->arena);
        handled = 1;
    } else {
        /* Inline implementations for basic vector ops */
        if (strcmp(m, "sum") == 0) {
            double sum = 0;
            for (size_t i = 0; i < mc->obj.data.vector.size; ++i) sum += mc->obj.data.vector.data[i];
            result = exprtk_val_num(sum);
            handled = 1;
        } else if (strcmp(m, "avg") == 0 || strcmp(m, "mean") == 0) {
            if (mc->obj.data.vector.size > 0) {
                double sum = 0;
                for (size_t i = 0; i < mc->obj.data.vector.size; ++i) sum += mc->obj.data.vector.data[i];
                result = exprtk_val_num(sum / (double)mc->obj.data.vector.size);
            } else {
                result = exprtk_val_num(0.0);
            }
            handled = 1;
        } else if (strcmp(m, "min") == 0 && mc->obj.data.vector.size > 0) {
            double v = mc->obj.data.vector.data[0];
            for (size_t i = 1; i < mc->obj.data.vector.size; ++i)
                if (mc->obj.data.vector.data[i] < v) v = mc->obj.data.vector.data[i];
            result = exprtk_val_num(v);
            handled = 1;
        } else if (strcmp(m, "max") == 0 && mc->obj.data.vector.size > 0) {
            double v = mc->obj.data.vector.data[0];
            for (size_t i = 1; i < mc->obj.data.vector.size; ++i)
                if (mc->obj.data.vector.data[i] > v) v = mc->obj.data.vector.data[i];
            result = exprtk_val_num(v);
            handled = 1;
        }
    }
    if (call_args != stack_args) free(call_args);
    if (!handled) return unknown_method_error(mc, "vector");
    return result;
}

CXX_C_API exprtk_value_t exprtk_member_call_checked_values(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, exprtk_value_t *args, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!object_name || !method_name || !env || (argc > 0 && !args)) return zero;

    mc_ctx_t mc = {
        .method = method_name,
        .obj = exprtk_env_get(env, object_name),
        .args = args,
        .argc = argc,
        .obj_node = (exprtk_node_t *)object_node,
        .env = env,
        .arena = &env->arena,
    };

    switch (mc.obj.type) {
        case EXPRTK_VAL_LIST:
        case EXPRTK_VAL_SET:   return eval_list_method(&mc);
        case EXPRTK_VAL_MAP:
        case EXPRTK_VAL_OBJECT: return eval_map_method(&mc);
        case EXPRTK_VAL_BYTES:  return eval_bytes_method(&mc);
        case EXPRTK_VAL_UUID:   return eval_uuid_method(&mc);
        case EXPRTK_VAL_DATETIME: return eval_datetime_method(&mc);
        case EXPRTK_VAL_DATE: return eval_date_method(&mc);
        case EXPRTK_VAL_TIME: return eval_time_method(&mc);
        case EXPRTK_VAL_DURATION: return eval_duration_method(&mc);
        case EXPRTK_VAL_DECIMAL: return eval_decimal_method(&mc);
        case EXPRTK_VAL_OFFSET_DATETIME: return eval_offset_datetime_method(&mc);
        case EXPRTK_VAL_TYPED_ARRAY: return eval_typed_array_method(&mc);
        case EXPRTK_VAL_STRING: return eval_string_method(&mc);
        case EXPRTK_VAL_VECTOR: return eval_vector_method(&mc);
        case EXPRTK_VAL_INSTANCE:
        case EXPRTK_VAL_CLASS:
            return exprtk_oop_call_method_checked_values(object_name, method_name,
                                                         object_node, argc, args, env);
        default: {
            char full_name[256];
            int written = snprintf(full_name, sizeof(full_name), "%s.%s",
                                   object_name, method_name);
            if (written < 0 || (size_t)written >= sizeof(full_name)) {
                return throw_error(env, object_node, "Qualified function name is too long");
            }
            return exprtk_call_internal(full_name, argc, args, env, &env->arena);
        }
    }
}

CXX_C_API exprtk_value_t exprtk_member_call_checked_numeric(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    size_t argc, const double *argv, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    exprtk_value_t *args = numeric_args_to_values(argc, argv);
    if (argc > 0 && !args) return zero;

    exprtk_value_t result = exprtk_member_call_checked_values(
        object_name, method_name, object_node, argc, args, env);
    free(args);
    return result;
}

CXX_C_API exprtk_value_t exprtk_member_call_checked_value_nodes(
    const char *object_name, const char *method_name, const exprtk_node_t *object_node,
    const exprtk_node_t *call_node, exprtk_env_t *env) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };
    if (!call_node || call_node->type != EXPRTK_NODE_MEMBER_CALL || !env) return zero;

    size_t actual_count = 0;
    exprtk_value_t *args = eval_expand_args(call_node->data.member_call.args,
                                            call_node->data.member_call.arg_count,
                                            env, &actual_count);
    if (!args && actual_count == 0) return zero;

    exprtk_value_t result = exprtk_member_call_checked_values(
        object_name, method_name, object_node, actual_count, args, env);
    free(args);
    return result;
}

int exprtk_env_last_line(const exprtk_env_t *env) {
    return env ? env->last_line : 0;
}

int exprtk_env_last_column(const exprtk_env_t *env) {
    return env ? env->last_column : 0;
}


/* ========================================================================
 * OOP - Class Registration and Lookup
 * ======================================================================== */

void exprtk_env_register_class(exprtk_env_t *env, const char *name, exprtk_class_t *klass) {
    if (!env || !name || !klass) return;
    exprtk_env_set_local(env, name, exprtk_val_class(klass));
}

exprtk_class_t *exprtk_env_lookup_class(exprtk_env_t *env, const char *name) {
    if (!env || !name) return NULL;
    exprtk_value_t value = exprtk_env_get(env, name);
    return value.type == EXPRTK_VAL_CLASS ? value.data.class_val.klass : NULL;
}
