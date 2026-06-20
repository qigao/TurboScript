/**
 * @file exprtk_class.c
 * @brief TurboScript OOP - Class and Instance Runtime Implementation
 */

#include "exprtk_class.h"
#include "exprtk.h"
#include "mir-htab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Hash Table Definitions for Methods and Fields
 * ======================================================================== */

// Hash table entry for method lookup (name -> func)
typedef struct {
    char *name;
    exprtk_func_t *func;
} method_entry_t;

DEF_HTAB(method_entry_t)

// Hash table entry for field storage (name -> value)
typedef struct {
    char *name;
    exprtk_value_t value;
    int access_level;
    exprtk_class_t *owner_class;
} field_entry_t;

DEF_HTAB(field_entry_t)

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

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

static uint64_t next_class_type_id = 1;

/**
 * @brief Hash function for method/field name lookup
 */
static htab_hash_t method_hash(method_entry_t entry, void *arg) {
    (void)arg;
    const char *s = entry.name;
    htab_hash_t h = 0;
    while (*s) h = h * 31 + (unsigned char)*s++;
    return h;
}

static int method_eq(method_entry_t e1, method_entry_t e2, void *arg) {
    (void)arg;
    return strcmp(e1.name, e2.name) == 0;
}

static char *method_arity_key(const char *name, size_t argc) {
    if (!name) return NULL;
    int needed = snprintf(NULL, 0, "%s@%zu", name, argc);
    if (needed < 0) return NULL;

    char *key = (char *)malloc((size_t)needed + 1);
    if (!key) return NULL;
    snprintf(key, (size_t)needed + 1, "%s@%zu", name, argc);
    return key;
}

static const char *value_signature_type(exprtk_value_t value) {
    switch (value.type) {
        case EXPRTK_VAL_NUMBER:
        case EXPRTK_VAL_INTEGER:
            return "number";
        case EXPRTK_VAL_STRING:
            return "string";
        case EXPRTK_VAL_VECTOR:
            return "vector";
        case EXPRTK_VAL_MAP:
            return "map";
        case EXPRTK_VAL_NULL:
            return "null";
        case EXPRTK_VAL_LIST:
            return "list";
        case EXPRTK_VAL_FUNCTION:
        case EXPRTK_VAL_BOUND_METHOD:
            return "function";
        case EXPRTK_VAL_CLASS:
            return "class";
        case EXPRTK_VAL_INSTANCE:
            return "instance";
        default:
            return "unknown";
    }
}

static const char *method_param_type(exprtk_node_t *param) {
    if (!param || param->type != EXPRTK_NODE_MEMBER_ACCESS ||
        !param->data.member_access.object ||
        param->data.member_access.object->type != EXPRTK_NODE_VARIABLE) {
        return NULL;
    }
    return param->data.member_access.member;
}

static int method_has_full_type_signature(exprtk_func_t *method) {
    if (!method || !method->is_script || method->data.script.arg_count == 0) return 0;
    for (size_t i = 0; i < method->data.script.arg_count; ++i) {
        if (!method_param_type(method->data.script.arg_params[i])) return 0;
    }
    return 1;
}

static exprtk_class_t *method_resolve_declared_class(exprtk_func_t *method,
                                                     const char *declared_type);

static char *method_decl_type_key(const char *name, exprtk_func_t *method) {
    if (!name || !method_has_full_type_signature(method)) return NULL;

    size_t len = strlen(name) + 32;
    for (size_t i = 0; i < method->data.script.arg_count; ++i) {
        len += strlen(method_param_type(method->data.script.arg_params[i])) + 1;
    }

    char *key = (char *)malloc(len);
    if (!key) return NULL;

    int written = snprintf(key, len, "%s@%zu", name, method->data.script.arg_count);
    for (size_t i = 0; i < method->data.script.arg_count; ++i) {
        written += snprintf(key + written, len - (size_t)written, "@%s",
                            method_param_type(method->data.script.arg_params[i]));
    }
    return key;
}

static char *method_decl_identity_key(const char *name, exprtk_func_t *method) {
    if (!name || !method_has_full_type_signature(method)) return NULL;

    size_t len = strlen(name) + 32;
    for (size_t i = 0; i < method->data.script.arg_count; ++i) {
        const char *declared_type = method_param_type(method->data.script.arg_params[i]);
        exprtk_class_t *declared_class =
            method_resolve_declared_class(method, declared_type);
        len += declared_class ? 32 : strlen(declared_type) + 1;
    }

    char *key = (char *)malloc(len);
    if (!key) return NULL;

    int written = snprintf(key, len, "%s@%zu", name, method->data.script.arg_count);
    for (size_t i = 0; i < method->data.script.arg_count; ++i) {
        const char *declared_type = method_param_type(method->data.script.arg_params[i]);
        exprtk_class_t *declared_class =
            method_resolve_declared_class(method, declared_type);
        if (declared_class) {
            written += snprintf(key + written, len - (size_t)written, "@#%llu",
                                (unsigned long long)declared_class->type_id);
        } else {
            written += snprintf(key + written, len - (size_t)written, "@%s",
                                declared_type);
        }
    }
    return key;
}

static char *method_call_type_key(const char *name, size_t argc, const exprtk_value_t *args) {
    if (!name || (argc > 0 && !args)) return NULL;

    size_t len = strlen(name) + 32;
    for (size_t i = 0; i < argc; ++i) {
        len += strlen(value_signature_type(args[i])) + 1;
    }

    char *key = (char *)malloc(len);
    if (!key) return NULL;

    int written = snprintf(key, len, "%s@%zu", name, argc);
    for (size_t i = 0; i < argc; ++i) {
        written += snprintf(key + written, len - (size_t)written, "@%s",
                            value_signature_type(args[i]));
    }
    return key;
}

static int class_name_match_depth(exprtk_class_t *klass, const char *name) {
    int depth = 0;
    for (exprtk_class_t *current = klass; current; current = current->prototype) {
        if (current->name && strcmp(current->name, name) == 0) return depth;
        int best_interface_depth = -1;
        for (size_t i = 0; i < current->interface_count; ++i) {
            int interface_depth = class_name_match_depth(current->interfaces[i], name);
            if (interface_depth >= 0 &&
                (best_interface_depth < 0 || interface_depth < best_interface_depth)) {
                best_interface_depth = interface_depth;
            }
        }
        if (best_interface_depth >= 0) return depth + best_interface_depth;
        depth++;
    }
    return -1;
}

static int class_identity_match_depth(exprtk_class_t *klass, exprtk_class_t *target) {
    int depth = 0;
    if (!klass || !target) return -1;

    for (exprtk_class_t *current = klass; current; current = current->prototype) {
        if (current == target ||
            (current->type_id != 0 && current->type_id == target->type_id)) {
            return depth;
        }
        int best_interface_depth = -1;
        for (size_t i = 0; i < current->interface_count; ++i) {
            int interface_depth =
                class_identity_match_depth(current->interfaces[i], target);
            if (interface_depth >= 0 &&
                (best_interface_depth < 0 || interface_depth < best_interface_depth)) {
                best_interface_depth = interface_depth;
            }
        }
        if (best_interface_depth >= 0) return depth + best_interface_depth;
        depth++;
    }

    return -1;
}

static exprtk_class_t *method_resolve_declared_class(exprtk_func_t *method,
                                                     const char *declared_type) {
    if (!method || !declared_type) return NULL;

    if (method->owner_class && method->owner_class->name &&
        strcmp(method->owner_class->name, declared_type) == 0) {
        return method->owner_class;
    }

    if (method->closure_env) {
        exprtk_value_t type_value = exprtk_env_get(method->closure_env, declared_type);
        if (type_value.type == EXPRTK_VAL_CLASS && type_value.data.class_val.klass) {
            return type_value.data.class_val.klass;
        }
    }

    return NULL;
}

static int value_matches_declared_type(exprtk_value_t value, const char *declared_type,
                                       exprtk_func_t *method) {
    if (!declared_type) return 0;

    if (strcmp(declared_type, value_signature_type(value)) == 0) return 10;

    exprtk_class_t *declared_class = method_resolve_declared_class(method, declared_type);
    if (declared_class) {
        exprtk_class_t *value_class = NULL;
        if (value.type == EXPRTK_VAL_INSTANCE && value.data.instance_val.instance) {
            value_class = value.data.instance_val.instance->klass;
        } else if (value.type == EXPRTK_VAL_CLASS && value.data.class_val.klass) {
            value_class = value.data.class_val.klass;
        }

        int depth = class_identity_match_depth(value_class, declared_class);
        return depth >= 0 ? 100 - depth : 0;
    }

    if (value.type == EXPRTK_VAL_INSTANCE && value.data.instance_val.instance) {
        exprtk_instance_t *instance = value.data.instance_val.instance;
        int depth = class_name_match_depth(instance->klass, declared_type);
        if (depth >= 0) return 100 - depth;
    }

    if (value.type == EXPRTK_VAL_CLASS && value.data.class_val.klass) {
        int depth = class_name_match_depth(value.data.class_val.klass, declared_type);
        if (depth >= 0) return 100 - depth;
    }

    return 0;
}

static int method_match_type_score(exprtk_func_t *method, const char *name,
                                   size_t argc, const exprtk_value_t *args) {
    if (!method || !name || !args || !method_has_full_type_signature(method)) return 0;
    if (!method->name || strcmp(method->name, name) != 0) return 0;
    if (method->data.script.arg_count != argc) return 0;

    int score = 0;
    for (size_t i = 0; i < argc; ++i) {
        const char *declared_type = method_param_type(method->data.script.arg_params[i]);
        int param_score = value_matches_declared_type(args[i], declared_type, method);
        if (param_score <= 0) return 0;
        score += param_score;
    }
    return score;
}

static exprtk_func_t *method_table_find_typed_match(void *table, const char *name,
                                                    size_t argc,
                                                    const exprtk_value_t *args) {
    if (!table || !name || (argc > 0 && !args)) return NULL;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)table;
    HTAB_EL(method_entry_t) *els_addr = VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;
    exprtk_func_t *best = NULL;
    int best_score = 0;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash == HTAB_DELETED_HASH) continue;
        exprtk_func_t *method = els_addr[i].el.func;
        int score = method_match_type_score(method, name, argc, args);
        if (score > best_score) {
            best_score = score;
            best = method;
        }
    }

    return best;
}

static int method_entry_is_for_func(method_entry_t entry) {
    if (!entry.name || !entry.func || !entry.func->name) return 0;
    if (strcmp(entry.name, entry.func->name) == 0) return 1;

    char *arity_key =
        method_arity_key(entry.func->name, entry.func->data.script.arg_count);
    if (!arity_key) return 0;
    int matches = strcmp(entry.name, arity_key) == 0;
    free(arity_key);
    if (matches) return 1;

    char *type_key = method_decl_type_key(entry.func->name, entry.func);
    if (!type_key) return 0;
    matches = strcmp(entry.name, type_key) == 0;
    free(type_key);
    return matches;
}

static htab_hash_t field_hash(field_entry_t entry, void *arg) {
    (void)arg;
    const char *s = entry.name;
    htab_hash_t h = 0;
    while (*s) h = h * 31 + (unsigned char)*s++;
    return h;
}

static int field_eq(field_entry_t e1, field_entry_t e2, void *arg) {
    (void)arg;
    return strcmp(e1.name, e2.name) == 0;
}

static int field_table_find(void *table, const char *name, field_entry_t *out_entry) {
    if (!table || !name) return 0;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)table;
    field_entry_t search;
    field_entry_t result;
    search.name = (char *)name;

    if (!HTAB_OP(field_entry_t, do)(htab, search, HTAB_FIND, &result)) return 0;
    if (out_entry) *out_entry = result;
    return 1;
}

static exprtk_value_t *field_table_find_slot(void *table, const char *name) {
    if (!table || !name) return NULL;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)table;
    htab_size_t size = (htab_size_t)VARR_LENGTH(htab_ind_t, htab->entries);
    if (size == 0) return NULL;

    field_entry_t search;
    search.name = (char *)name;
    htab_hash_t hash = field_hash(search, htab->arg);
    if (hash == HTAB_DELETED_HASH) hash += 1;

    htab_size_t mask = size - 1;
    htab_hash_t peterb = hash;
    htab_ind_t *entries = VARR_ADDR(htab_ind_t, htab->entries);
    HTAB_EL(field_entry_t) *els = VARR_ADDR(HTAB_EL(field_entry_t), htab->els);

    for (htab_size_t ind = hash & mask;;) {
        htab_ind_t el_ind = entries[ind];
        if (el_ind == HTAB_EMPTY_IND) return NULL;
        if (el_ind != HTAB_DELETED_IND && els[el_ind].hash == hash &&
            field_eq(els[el_ind].el, search, htab->arg)) {
            return &els[el_ind].el.value;
        }
        peterb >>= 11;
        ind = (5 * ind + peterb + 1) & mask;
    }
}

static int method_table_find(void *table, const char *name, method_entry_t *out_entry) {
    if (!table || !name) return 0;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)table;
    method_entry_t search;
    method_entry_t result;
    search.name = (char *)name;

    if (!HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) return 0;
    if (out_entry) *out_entry = result;
    return 1;
}

static int method_parse_arity_requirement(const char *name, size_t *out_name_len,
                                          size_t *out_argc) {
    if (!name) return 0;

    const char *at = strchr(name, '@');
    if (!at || at == name || !at[1]) return 0;

    size_t argc = 0;
    for (const char *p = at + 1; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        argc = argc * 10 + (size_t)(*p - '0');
    }

    if (out_name_len) *out_name_len = (size_t)(at - name);
    if (out_argc) *out_argc = argc;
    return 1;
}

static int method_table_has_func_with_arity(void *table, const char *name,
                                            size_t name_len, size_t argc) {
    if (!table || !name) return 0;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)table;
    HTAB_EL(method_entry_t) *els_addr =
        VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash == HTAB_DELETED_HASH) continue;

        exprtk_func_t *func = els_addr[i].el.func;
        if (!func || !func->is_script || !func->name) continue;
        if (func->data.script.arg_count != argc) continue;
        if (strlen(func->name) != name_len) continue;
        if (strncmp(func->name, name, name_len) == 0) return 1;
    }

    return 0;
}

static int method_classes_same(exprtk_class_t *left, exprtk_class_t *right) {
    if (!left || !right) return 0;
    return left == right || (left->type_id != 0 && left->type_id == right->type_id);
}

static int method_signatures_exactly_match(exprtk_func_t *requirement,
                                           exprtk_func_t *candidate) {
    if (!requirement || !candidate || !requirement->name || !candidate->name) return 0;
    if (strcmp(requirement->name, candidate->name) != 0) return 0;
    if (requirement->is_static_method != candidate->is_static_method) return 0;
    if (requirement->data.script.arg_count != candidate->data.script.arg_count) return 0;

    if (!method_has_full_type_signature(requirement)) return 1;
    if (!method_has_full_type_signature(candidate)) return 0;

    for (size_t i = 0; i < requirement->data.script.arg_count; ++i) {
        const char *required_type =
            method_param_type(requirement->data.script.arg_params[i]);
        const char *candidate_type =
            method_param_type(candidate->data.script.arg_params[i]);
        if (!required_type || !candidate_type) return 0;

        exprtk_class_t *required_class =
            method_resolve_declared_class(requirement, required_type);
        exprtk_class_t *candidate_class =
            method_resolve_declared_class(candidate, candidate_type);
        if (required_class || candidate_class) {
            if (!method_classes_same(required_class, candidate_class)) return 0;
            continue;
        }

        if (strcmp(required_type, candidate_type) != 0) return 0;
    }

    return 1;
}

static int method_requirement_satisfied_by_func(const char *requirement_name,
                                                exprtk_func_t *requirement,
                                                exprtk_func_t *candidate) {
    if (!requirement_name || !candidate || !candidate->name) return 0;

    if (requirement) return method_signatures_exactly_match(requirement, candidate);

    if (strcmp(requirement_name, candidate->name) == 0) {
        return candidate->data.script.arg_count == 0;
    }

    size_t method_name_len = 0;
    size_t argc = 0;
    if (method_parse_arity_requirement(requirement_name, &method_name_len, &argc)) {
        return strlen(candidate->name) == method_name_len &&
               strncmp(candidate->name, requirement_name, method_name_len) == 0 &&
               candidate->data.script.arg_count == argc;
    }

    char *type_key = method_decl_type_key(candidate->name, candidate);
    int matches = type_key && strcmp(requirement_name, type_key) == 0;
    free(type_key);
    return matches;
}

static int method_table_has_func_satisfying(void *table, const char *requirement_name,
                                            exprtk_func_t *requirement) {
    if (!table || !requirement_name) return 0;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)table;
    HTAB_EL(method_entry_t) *els_addr =
        VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash == HTAB_DELETED_HASH) continue;
        if (!method_entry_is_for_func(els_addr[i].el)) continue;
        if (method_requirement_satisfied_by_func(
                requirement_name, requirement, els_addr[i].el.func)) {
            return 1;
        }
    }

    return 0;
}

static int class_has_own_concrete_method(exprtk_class_t *klass, const char *name) {
    method_entry_t entry;
    if (!klass || !name) return 0;

    if (method_table_find(klass->methods, name, &entry) && entry.func) return 1;

    size_t method_name_len = 0;
    size_t argc = 0;
    if (!method_parse_arity_requirement(name, &method_name_len, &argc)) return 0;

    return method_table_has_func_with_arity(
        klass->methods, name, method_name_len, argc);
}

static int class_has_own_concrete_method_signature(exprtk_class_t *klass,
                                                   const char *name,
                                                   exprtk_func_t *signature) {
    if (!klass || !name) return 0;
    if (!signature) return class_has_own_concrete_method(klass, name);
    return method_table_has_func_satisfying(
        signature->is_static_method ? klass->static_methods : klass->methods,
        name, signature);
}

static int class_has_concrete_method_signature(exprtk_class_t *klass,
                                               const char *name,
                                               exprtk_func_t *signature) {
    for (exprtk_class_t *current = klass; current; current = current->prototype) {
        if (class_has_own_concrete_method_signature(current, name, signature)) return 1;
    }
    return 0;
}

static void class_remove_abstract_method(exprtk_class_t *klass, const char *name) {
    if (!klass || !klass->abstract_methods || !name) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->abstract_methods;
    method_entry_t search;
    method_entry_t result;
    search.name = (char *)name;

    if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
        HTAB_OP(method_entry_t, do)(htab, search, HTAB_DELETE, &result);
    }
}

static void class_remove_satisfied_abstract_methods(exprtk_class_t *klass,
                                                    exprtk_func_t *method_func) {
    if (!klass || !klass->abstract_methods || !method_func) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->abstract_methods;
    HTAB_EL(method_entry_t) *els_addr =
        VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash == HTAB_DELETED_HASH || !els_addr[i].el.name) continue;
        if (!method_requirement_satisfied_by_func(
                els_addr[i].el.name, els_addr[i].el.func, method_func)) {
            continue;
        }

        method_entry_t search;
        method_entry_t result;
        search.name = els_addr[i].el.name;
        HTAB_OP(method_entry_t, do)(htab, search, HTAB_DELETE, &result);
    }
}

static char *class_copy_string(mem_pool_t *arena, const char *value) {
    if (!arena || !value) return NULL;

    size_t len = strlen(value);
    char *copy = (char *)mem_alloc(arena, len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static int class_find_instance_field_index(exprtk_class_t *klass, const char *name,
                                           size_t *out_index) {
    if (!klass || !name) return 0;

    for (size_t i = 0; i < klass->instance_field_count; ++i) {
        if (klass->instance_field_names[i] &&
            strcmp(klass->instance_field_names[i], name) == 0) {
            if (out_index) *out_index = i;
            return 1;
        }
    }
    return 0;
}

static int class_append_instance_field(exprtk_class_t *klass, const char *name,
                                       exprtk_class_t *owner_class,
                                       int access_level,
                                       exprtk_value_t default_value,
                                       int has_default,
                                       int update_existing,
                                       size_t *out_index) {
    size_t index = 0;
    if (!klass || !name) return 0;
    if (class_find_instance_field_index(klass, name, &index)) {
        if (update_existing && klass->instance_field_access_levels)
            klass->instance_field_access_levels[index] = access_level;
        if (update_existing && klass->instance_field_owners)
            klass->instance_field_owners[index] = owner_class ? owner_class : klass;
        if (update_existing && klass->instance_field_defaults && klass->instance_field_has_default) {
            klass->instance_field_defaults[index] = default_value;
            klass->instance_field_has_default[index] = has_default ? 1 : 0;
        }
        if (out_index) *out_index = index;
        return 1;
    }

    if (klass->instance_field_count >= klass->instance_field_capacity) {
        size_t new_capacity =
            klass->instance_field_capacity > 0 ? klass->instance_field_capacity * 2 : 8;
        char **new_names =
            (char **)realloc(klass->instance_field_names, new_capacity * sizeof(*new_names));
        if (!new_names) return 0;
        int *new_access = (int *)realloc(klass->instance_field_access_levels,
                                         new_capacity * sizeof(*new_access));
        if (!new_access) {
            klass->instance_field_names = new_names;
            return 0;
        }
        exprtk_class_t **new_owners =
            (exprtk_class_t **)realloc(klass->instance_field_owners,
                                       new_capacity * sizeof(*new_owners));
        if (!new_owners) {
            klass->instance_field_names = new_names;
            klass->instance_field_access_levels = new_access;
            return 0;
        }
        exprtk_value_t *new_defaults =
            (exprtk_value_t *)realloc(klass->instance_field_defaults,
                                      new_capacity * sizeof(*new_defaults));
        if (!new_defaults) {
            klass->instance_field_names = new_names;
            klass->instance_field_access_levels = new_access;
            klass->instance_field_owners = new_owners;
            return 0;
        }
        unsigned char *new_has_default =
            (unsigned char *)realloc(klass->instance_field_has_default,
                                     new_capacity * sizeof(*new_has_default));
        if (!new_has_default) {
            klass->instance_field_names = new_names;
            klass->instance_field_access_levels = new_access;
            klass->instance_field_owners = new_owners;
            klass->instance_field_defaults = new_defaults;
            return 0;
        }
        for (size_t i = klass->instance_field_capacity; i < new_capacity; ++i) {
            new_access[i] = EXPRTK_ACCESS_PUBLIC;
            new_owners[i] = NULL;
            memset(&new_defaults[i], 0, sizeof(new_defaults[i]));
            new_has_default[i] = 0;
        }
        klass->instance_field_names = new_names;
        klass->instance_field_access_levels = new_access;
        klass->instance_field_owners = new_owners;
        klass->instance_field_defaults = new_defaults;
        klass->instance_field_has_default = new_has_default;
        klass->instance_field_capacity = new_capacity;
    }

    char *field_name = class_copy_string(klass->arena, name);
    if (!field_name) return 0;

    index = klass->instance_field_count++;
    klass->instance_field_names[index] = field_name;
    klass->instance_field_access_levels[index] = access_level;
    klass->instance_field_owners[index] = owner_class ? owner_class : klass;
    klass->instance_field_defaults[index] = default_value;
    klass->instance_field_has_default[index] = has_default ? 1 : 0;
    if (out_index) *out_index = index;
    return 1;
}

static int class_import_instance_fields(exprtk_class_t *klass, exprtk_class_t *source) {
    if (!klass) return 0;
    if (!source) return 1;

    if (!class_import_instance_fields(klass, source->prototype)) return 0;
    for (size_t i = 0; i < source->instance_field_count; ++i) {
        int access_level = source->instance_field_access_levels
            ? source->instance_field_access_levels[i]
            : EXPRTK_ACCESS_PUBLIC;
        exprtk_class_t *owner = source->instance_field_owners
            ? source->instance_field_owners[i]
            : source;
        exprtk_value_t default_value = source->instance_field_defaults
            ? source->instance_field_defaults[i]
            : (exprtk_value_t){0};
        int has_default = source->instance_field_has_default
            ? source->instance_field_has_default[i]
            : 0;
        if (!class_append_instance_field(klass, source->instance_field_names[i], owner,
                                         access_level, default_value, has_default, 0, NULL)) {
            return 0;
        }
    }
    return 1;
}

static int class_sync_prototype_instance_fields(exprtk_class_t *klass) {
    if (!klass) return 0;
    return class_import_instance_fields(klass, klass->prototype);
}

static int class_ensure_instance_field(exprtk_class_t *klass, const char *name,
                                       size_t *out_index) {
    if (!klass || !name) return 0;
    if (!class_sync_prototype_instance_fields(klass)) return 0;
    return class_append_instance_field(klass, name, klass, EXPRTK_ACCESS_PUBLIC,
                                       (exprtk_value_t){0}, 0, 0, out_index);
}

static int instance_ensure_slot_capacity(exprtk_instance_t *instance, size_t needed) {
    if (!instance) return 0;
    if (needed <= instance->field_slot_capacity) return 1;

    size_t new_capacity = instance->field_slot_capacity > 0 ? instance->field_slot_capacity : 8;
    while (new_capacity < needed) new_capacity *= 2;

    exprtk_value_t *new_slots =
        (exprtk_value_t *)realloc(instance->field_slots, new_capacity * sizeof(*new_slots));
    if (!new_slots) return 0;

    unsigned char *new_used =
        (unsigned char *)realloc(instance->field_slot_used, new_capacity * sizeof(*new_used));
    if (!new_used) {
        instance->field_slots = new_slots;
        return 0;
    }

    for (size_t i = instance->field_slot_capacity; i < new_capacity; ++i) {
        memset(&new_slots[i], 0, sizeof(new_slots[i]));
        new_used[i] = 0;
    }

    instance->field_slots = new_slots;
    instance->field_slot_used = new_used;
    instance->field_slot_capacity = new_capacity;
    return 1;
}

static exprtk_func_t *class_clone_func(mem_pool_t *arena,
                                       exprtk_func_t *func,
                                       exprtk_class_t *owner_class) {
    if (!arena || !func) return NULL;

    exprtk_func_t *copy = (exprtk_func_t *)mem_alloc(arena, sizeof(exprtk_func_t));
    if (!copy) return NULL;

    *copy = *func;
    copy->name = class_copy_string(arena, func->name ? func->name : "");
    copy->owner_class = owner_class;
    copy->next = NULL;
    return copy;
}

static void class_add_abstract_method_signature_entry(exprtk_class_t *klass,
                                                      const char *name,
                                                      exprtk_func_t *signature);

static void class_clone_methods(exprtk_class_t *dst, void *src_table, int is_static) {
    if (!dst || !src_table) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)src_table;
    HTAB_EL(method_entry_t) *els_addr = VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH &&
            method_entry_is_for_func(els_addr[i].el)) {
            exprtk_func_t *method =
                class_clone_func(dst->arena, els_addr[i].el.func, dst);
            if (method) {
                exprtk_class_add_method(dst, method->name, method, is_static);
            }
        }
    }
}

static void class_clone_constructors(exprtk_class_t *dst, void *src_table) {
    if (!dst || !src_table) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)src_table;
    HTAB_EL(method_entry_t) *els_addr = VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH &&
            method_entry_is_for_func(els_addr[i].el)) {
            exprtk_func_t *constructor =
                class_clone_func(dst->arena, els_addr[i].el.func, dst);
            if (constructor) {
                exprtk_class_add_constructor(dst, constructor);
            }
        }
    }
}

static void class_clone_abstract_methods(exprtk_class_t *dst, void *src_table) {
    if (!dst || !src_table) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)src_table;
    HTAB_EL(method_entry_t) *els_addr = VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            if (els_addr[i].el.func) {
                exprtk_func_t *signature =
                    class_clone_func(dst->arena, els_addr[i].el.func, dst);
                if (signature) {
                    class_add_abstract_method_signature_entry(
                        dst, els_addr[i].el.name, signature);
                }
            } else {
                exprtk_class_add_abstract_method(dst, els_addr[i].el.name);
            }
        }
    }
}

/* ========================================================================
 * Class Management Implementation
 * ======================================================================== */

exprtk_class_t *exprtk_class_create(
    mem_pool_t *arena,
    const char *name,
    exprtk_node_t *constructor_node,
    exprtk_node_t **method_nodes,
    size_t method_count)
{
    if (!arena || !name) return NULL;

    exprtk_class_t *klass = (exprtk_class_t *)mem_alloc(arena, sizeof(exprtk_class_t));
    if (!klass) return NULL;

    // Copy name
    size_t name_len = strlen(name);
    klass->name = (char *)mem_alloc(arena, name_len + 1);
    if (!klass->name) return NULL;
    memcpy(klass->name, name, name_len + 1);

    // Constructor (will be created during evaluation)
    klass->constructor = NULL;

    // Create method hash tables using MIR HTAB
    HTAB(method_entry_t) *constructors_htab = NULL;
    HTAB(method_entry_t) *methods_htab = NULL;
    HTAB(method_entry_t) *static_methods_htab = NULL;
    HTAB(method_entry_t) *abstract_methods_htab = NULL;
    HTAB(field_entry_t) *static_fields_htab = NULL;
    
    HTAB_OP(method_entry_t, create)(&constructors_htab, mir_std_alloc, 8,
                                    method_hash, method_eq, NULL, NULL);
    HTAB_OP(method_entry_t, create)(&methods_htab, mir_std_alloc, 16,
                                    method_hash, method_eq, NULL, NULL);
    HTAB_OP(method_entry_t, create)(&static_methods_htab, mir_std_alloc, 16,
                                    method_hash, method_eq, NULL, NULL);
    HTAB_OP(method_entry_t, create)(&abstract_methods_htab, mir_std_alloc, 16,
                                    method_hash, method_eq, NULL, NULL);
    HTAB_OP(field_entry_t, create)(&static_fields_htab, mir_std_alloc, 16,
                                   field_hash, field_eq, NULL, NULL);
    
    if (!constructors_htab || !methods_htab || !static_methods_htab || !abstract_methods_htab ||
        !static_fields_htab) {
        if (constructors_htab) HTAB_OP(method_entry_t, destroy)(&constructors_htab);
        if (methods_htab) HTAB_OP(method_entry_t, destroy)(&methods_htab);
        if (static_methods_htab) HTAB_OP(method_entry_t, destroy)(&static_methods_htab);
        if (abstract_methods_htab) HTAB_OP(method_entry_t, destroy)(&abstract_methods_htab);
        if (static_fields_htab) HTAB_OP(field_entry_t, destroy)(&static_fields_htab);
        return NULL;
    }

    klass->constructors = constructors_htab;
    klass->methods = methods_htab;
    klass->static_methods = static_methods_htab;
    klass->abstract_methods = abstract_methods_htab;
    klass->static_fields = static_fields_htab;
    klass->instance_field_names = NULL;
    klass->instance_field_access_levels = NULL;
    klass->instance_field_owners = NULL;
    klass->instance_field_defaults = NULL;
    klass->instance_field_has_default = NULL;
    klass->instance_field_count = 0;
    klass->instance_field_capacity = 0;
    klass->interfaces = NULL;
    klass->interface_count = 0;
    klass->interface_capacity = 0;

    // Initialize prototype chain
    klass->prototype = NULL;
    klass->type_id = next_class_type_id++;
    klass->static_field_version = 1;
    klass->is_abstract = 0;
    klass->is_interface = 0;
    klass->is_final = 0;
    klass->arena = arena;

    return klass;
}

exprtk_class_t *exprtk_class_clone_to_arena(exprtk_class_t *klass, mem_pool_t *arena) {
    if (!klass || !arena) return NULL;
    if (klass->arena == arena) return klass;

    exprtk_class_t *copy = exprtk_class_create(arena, klass->name, NULL, NULL, 0);
    if (!copy) return NULL;

    copy->is_abstract = klass->is_abstract;
    copy->is_interface = klass->is_interface;
    copy->is_final = klass->is_final;
    copy->type_id = klass->type_id;
    copy->prototype = exprtk_class_clone_to_arena(klass->prototype, arena);
    copy->constructor = class_clone_func(arena, klass->constructor, copy);

    class_clone_constructors(copy, klass->constructors);
    class_clone_methods(copy, klass->methods, 0);
    class_clone_methods(copy, klass->static_methods, 1);
    class_clone_abstract_methods(copy, klass->abstract_methods);
    for (size_t i = 0; i < klass->instance_field_count; ++i) {
        int access_level = klass->instance_field_access_levels
            ? klass->instance_field_access_levels[i]
            : EXPRTK_ACCESS_PUBLIC;
        exprtk_class_t *source_owner = klass->instance_field_owners
            ? klass->instance_field_owners[i]
            : klass;
        exprtk_class_t *owner = copy;
        if (source_owner == klass) {
            owner = copy;
        } else if (source_owner == klass->prototype) {
            owner = copy->prototype;
        } else if (source_owner) {
            owner = exprtk_class_clone_to_arena(source_owner, arena);
        }
        exprtk_value_t default_value = klass->instance_field_defaults
            ? klass->instance_field_defaults[i]
            : (exprtk_value_t){0};
        int has_default = klass->instance_field_has_default
            ? klass->instance_field_has_default[i]
            : 0;
        class_append_instance_field(copy, klass->instance_field_names[i], owner,
                                    access_level, default_value, has_default, 1, NULL);
    }
    for (size_t i = 0; i < klass->interface_count; ++i) {
        exprtk_class_add_interface(copy,
            exprtk_class_clone_to_arena(klass->interfaces[i], arena));
    }
    copy->is_abstract = klass->is_abstract;
    copy->is_interface = klass->is_interface;
    copy->is_final = klass->is_final;
    copy->type_id = klass->type_id;

    return copy;
}

void exprtk_class_set_prototype(exprtk_class_t *klass, exprtk_class_t *parent) {
    if (!klass) return;
    klass->prototype = parent;
    class_sync_prototype_instance_fields(klass);
}

void exprtk_class_set_abstract(exprtk_class_t *klass, int is_abstract) {
    if (!klass) return;
    klass->is_abstract = is_abstract ? 1 : 0;
}

void exprtk_class_set_interface(exprtk_class_t *klass, int is_interface) {
    if (!klass) return;
    klass->is_interface = is_interface ? 1 : 0;
    if (klass->is_interface) klass->is_abstract = 1;
}

void exprtk_class_set_final(exprtk_class_t *klass, int is_final) {
    if (!klass) return;
    klass->is_final = is_final ? 1 : 0;
}

int exprtk_class_is_final(exprtk_class_t *klass) {
    return klass && klass->is_final;
}

void exprtk_class_add_interface(exprtk_class_t *klass, exprtk_class_t *interface_class) {
    if (!klass || !interface_class) return;

    for (size_t i = 0; i < klass->interface_count; ++i) {
        if (klass->interfaces[i] == interface_class) return;
    }

    if (klass->interface_count >= klass->interface_capacity) {
        size_t new_capacity = klass->interface_capacity > 0
            ? klass->interface_capacity * 2
            : 4;
        exprtk_class_t **new_interfaces =
            (exprtk_class_t **)mem_alloc(klass->arena,
                                         new_capacity * sizeof(*new_interfaces));
        if (!new_interfaces) return;
        if (klass->interfaces && klass->interface_count > 0) {
            memcpy(new_interfaces, klass->interfaces,
                   klass->interface_count * sizeof(*new_interfaces));
        }
        klass->interfaces = new_interfaces;
        klass->interface_capacity = new_capacity;
    }

    klass->interfaces[klass->interface_count++] = interface_class;
}

int exprtk_class_is_a(exprtk_class_t *klass, exprtk_class_t *target) {
    if (!klass || !target) return 0;

    for (exprtk_class_t *current = klass; current; current = current->prototype) {
        if (current == target ||
            (current->type_id != 0 && current->type_id == target->type_id)) {
            return 1;
        }
        for (size_t i = 0; i < current->interface_count; ++i) {
            if (exprtk_class_is_a(current->interfaces[i], target)) return 1;
        }
    }

    return 0;
}

void exprtk_class_add_method(
    exprtk_class_t *klass,
    const char *name,
    exprtk_func_t *method_func,
    int is_static)
{
    if (!klass || !name || !method_func) return;

    int has_type_signature = method_has_full_type_signature(method_func);
    char *abstract_arity_key = method_arity_key(name, method_func->data.script.arg_count);
    char *arity_key = has_type_signature ? NULL : abstract_arity_key;
    char *type_key = method_decl_type_key(name, method_func);
    class_remove_satisfied_abstract_methods(klass, method_func);

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)(is_static ? klass->static_methods : klass->methods);
    if (!htab) {
        free(abstract_arity_key);
        free(type_key);
        return;
    }

    const char *keys[3] = { name, NULL, NULL };
    keys[1] = arity_key;
    keys[2] = type_key;

    for (size_t i = 0; i < 3; ++i) {
        const char *key = keys[i];
        if (!key) continue;

        method_entry_t search;
        method_entry_t result;
        search.name = (char *)key;
        if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
            result.func = method_func;
            HTAB_OP(method_entry_t, do)(htab, result, HTAB_REPLACE, &result);
            continue;
        }

        size_t key_len = strlen(key);
        char *method_name = (char *)mem_alloc(klass->arena, key_len + 1);
        if (!method_name) continue;
        memcpy(method_name, key, key_len + 1);

        method_entry_t entry;
        entry.name = method_name;
        entry.func = method_func;
        HTAB_OP(method_entry_t, do)(htab, entry, HTAB_INSERT, &result);
    }

    free(abstract_arity_key);
    free(type_key);
}

void exprtk_class_add_constructor(
    exprtk_class_t *klass,
    exprtk_func_t *constructor_func)
{
    if (!klass || !constructor_func || !klass->constructors) return;
    if (!constructor_func->name) constructor_func->name = "constructor";

    if (!klass->constructor) klass->constructor = constructor_func;

    int has_type_signature = method_has_full_type_signature(constructor_func);
    char *arity_key = method_arity_key("constructor", constructor_func->data.script.arg_count);
    char *type_key = method_decl_type_key("constructor", constructor_func);

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->constructors;
    const char *keys[2] = { has_type_signature ? NULL : arity_key, type_key };

    for (size_t i = 0; i < 2; ++i) {
        const char *key = keys[i];
        if (!key) continue;

        method_entry_t search;
        method_entry_t result;
        search.name = (char *)key;
        if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
            result.func = constructor_func;
            HTAB_OP(method_entry_t, do)(htab, result, HTAB_REPLACE, &result);
            continue;
        }

        char *constructor_name = class_copy_string(klass->arena, key);
        if (!constructor_name) continue;

        method_entry_t entry;
        entry.name = constructor_name;
        entry.func = constructor_func;
        HTAB_OP(method_entry_t, do)(htab, entry, HTAB_INSERT, &result);
    }

    free(arity_key);
    free(type_key);
}

exprtk_func_t *exprtk_class_lookup_constructor_typed(
    exprtk_class_t *klass,
    size_t argc,
    const exprtk_value_t *args)
{
    if (!klass) return NULL;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->constructors;
    if (htab) {
        char *key = method_call_type_key("constructor", argc, args);
        if (key) {
            method_entry_t search;
            method_entry_t result;
            search.name = key;
            if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
                free(key);
                return result.func;
            }
            free(key);
        }

        exprtk_func_t *typed_constructor =
            method_table_find_typed_match(htab, "constructor", argc, args);
        if (typed_constructor) return typed_constructor;

        key = method_arity_key("constructor", argc);
        if (key) {
            method_entry_t search;
            method_entry_t result;
            search.name = key;
            if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
                free(key);
                return result.func;
            }
            free(key);
        }
    }

    if (klass->constructor && klass->constructor->data.script.arg_count == argc) {
        return klass->constructor;
    }

    if (klass->prototype) {
        return exprtk_class_lookup_constructor_typed(klass->prototype, argc, args);
    }

    return argc == 0 ? klass->constructor : NULL;
}

void exprtk_class_add_abstract_method(exprtk_class_t *klass, const char *name) {
    if (!klass || !name || !klass->abstract_methods) return;
    if (class_has_own_concrete_method(klass, name)) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->abstract_methods;
    method_entry_t search;
    method_entry_t result;
    search.name = (char *)name;
    if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) return;

    size_t name_len = strlen(name);
    char *method_name = (char *)mem_alloc(klass->arena, name_len + 1);
    if (!method_name) return;
    memcpy(method_name, name, name_len + 1);

    method_entry_t entry;
    entry.name = method_name;
    entry.func = NULL;
    HTAB_OP(method_entry_t, do)(htab, entry, HTAB_INSERT, &result);
}

static void class_add_abstract_method_signature_entry(exprtk_class_t *klass,
                                                      const char *name,
                                                      exprtk_func_t *signature) {
    if (!klass || !name || !signature || !klass->abstract_methods) return;
    if (class_has_own_concrete_method_signature(klass, name, signature)) return;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->abstract_methods;
    method_entry_t search;
    method_entry_t result;
    search.name = (char *)name;
    if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) return;

    char *method_name = class_copy_string(klass->arena, name);
    if (!method_name) return;

    method_entry_t entry;
    entry.name = method_name;
    entry.func = signature;
    HTAB_OP(method_entry_t, do)(htab, entry, HTAB_INSERT, &result);
}

void exprtk_class_add_abstract_method_arity(
    exprtk_class_t *klass,
    const char *name,
    size_t argc)
{
    char *key = method_arity_key(name, argc);
    exprtk_class_add_abstract_method(klass, key ? key : name);
    free(key);
}

void exprtk_class_add_abstract_method_signature(
    exprtk_class_t *klass,
    const char *name,
    exprtk_node_t **arg_params,
    size_t argc,
    exprtk_env_t *closure_env,
    int is_static_method)
{
    exprtk_func_t method;
    memset(&method, 0, sizeof(method));
    method.name = (char *)name;
    method.is_script = 1;
    method.owner_class = klass;
    method.closure_env = closure_env;
    method.is_static_method = is_static_method ? 1 : 0;
    method.data.script.arg_params = arg_params;
    method.data.script.arg_count = argc;

    char *type_key = method_decl_identity_key(name, &method);
    if (!type_key) type_key = method_decl_type_key(name, &method);
    if (!type_key) type_key = method_arity_key(name, argc);
    if (!type_key) return;

    const char *key_value = type_key;
    char *static_key = NULL;
    if (method.is_static_method) {
        size_t key_len = strlen(type_key);
        static_key = (char *)malloc(key_len + 8);
        if (static_key) {
            memcpy(static_key, "static:", 7);
            memcpy(static_key + 7, type_key, key_len + 1);
            key_value = static_key;
        }
    }

    exprtk_func_t *signature =
        (exprtk_func_t *)mem_alloc(klass->arena, sizeof(exprtk_func_t));
    if (signature) {
        *signature = method;
        signature->name = class_copy_string(klass->arena, name);
        signature->next = NULL;
        class_add_abstract_method_signature_entry(klass, key_value, signature);
    }

    free(static_key);
    free(type_key);
}

void exprtk_class_add_abstract_methods_from(
    exprtk_class_t *klass,
    exprtk_class_t *interface_class)
{
    if (!klass || !interface_class || !interface_class->abstract_methods) return;

    HTAB(method_entry_t) *htab =
        (HTAB(method_entry_t) *)interface_class->abstract_methods;
    HTAB_EL(method_entry_t) *els_addr =
        VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH && els_addr[i].el.name) {
            if (class_has_concrete_method_signature(
                    klass, els_addr[i].el.name, els_addr[i].el.func)) {
                continue;
            }
            if (els_addr[i].el.func) {
                class_add_abstract_method_signature_entry(
                    klass, els_addr[i].el.name, els_addr[i].el.func);
            } else {
                exprtk_class_add_abstract_method(klass, els_addr[i].el.name);
            }
        }
    }
}

exprtk_func_t *exprtk_class_lookup_method(
    exprtk_class_t *klass,
    const char *name,
    int is_static)
{
    if (!klass || !name) return NULL;

    HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)(is_static ? klass->static_methods : klass->methods);
    if (!htab) return NULL;

    // Try current class
    method_entry_t search;
    method_entry_t result;
    search.name = (char *)name;
    if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
        return result.func;
    }

    // Try prototype chain
    if (klass->prototype) {
        return exprtk_class_lookup_method(klass->prototype, name, is_static);
    }

    return NULL;
}

exprtk_func_t *exprtk_class_lookup_method_arity(
    exprtk_class_t *klass,
    const char *name,
    int is_static,
    size_t argc)
{
    if (!klass || !name) return NULL;

    HTAB(method_entry_t) *htab =
        (HTAB(method_entry_t) *)(is_static ? klass->static_methods : klass->methods);
    if (!htab) return NULL;

    char *key = method_arity_key(name, argc);
    if (key) {
        method_entry_t search;
        method_entry_t result;
        search.name = key;
        if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
            free(key);
            return result.func;
        }
        free(key);
    }

    if (klass->prototype) {
        exprtk_func_t *parent_method =
            exprtk_class_lookup_method_arity(klass->prototype, name, is_static, argc);
        if (parent_method) return parent_method;
    }

    return NULL;
}

exprtk_func_t *exprtk_class_lookup_method_signature(
    exprtk_class_t *klass,
    exprtk_func_t *signature)
{
    if (!klass || !signature || !signature->name) return NULL;

    HTAB(method_entry_t) *htab =
        (HTAB(method_entry_t) *)(signature->is_static_method ? klass->static_methods : klass->methods);
    if (htab) {
        HTAB_EL(method_entry_t) *els_addr =
            VARR_ADDR(HTAB_EL(method_entry_t), htab->els);
        htab_size_t bound = htab->els_bound;

        for (htab_size_t i = 0; i < bound; ++i) {
            if (els_addr[i].hash == HTAB_DELETED_HASH) continue;
            exprtk_func_t *method = els_addr[i].el.func;
            if (method_signatures_exactly_match(signature, method)) return method;
        }
    }

    if (klass->prototype) {
        return exprtk_class_lookup_method_signature(klass->prototype, signature);
    }

    return NULL;
}

exprtk_func_t *exprtk_class_lookup_method_typed(
    exprtk_class_t *klass,
    const char *name,
    int is_static,
    size_t argc,
    const exprtk_value_t *args)
{
    if (!klass || !name) return NULL;

    HTAB(method_entry_t) *htab =
        (HTAB(method_entry_t) *)(is_static ? klass->static_methods : klass->methods);
    if (!htab) return NULL;

    char *key = method_call_type_key(name, argc, args);
    if (key) {
        method_entry_t search;
        method_entry_t result;
        search.name = key;
        if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
            free(key);
            return result.func;
        }
        free(key);
    }

    exprtk_func_t *typed_method =
        method_table_find_typed_match(htab, name, argc, args);
    if (typed_method) return typed_method;

    key = method_arity_key(name, argc);
    if (key) {
        method_entry_t search;
        method_entry_t result;
        search.name = key;
        if (HTAB_OP(method_entry_t, do)(htab, search, HTAB_FIND, &result)) {
            free(key);
            return result.func;
        }
        free(key);
    }

    if (klass->prototype) {
        exprtk_func_t *parent_method =
            exprtk_class_lookup_method_typed(klass->prototype, name, is_static, argc, args);
        if (parent_method) return parent_method;
    }

    return NULL;
}

int exprtk_class_get_static_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t *out_value)
{
    if (!klass || !name || !out_value) return 0;

    field_entry_t entry;
    if (field_table_find(klass->static_fields, name, &entry)) {
        *out_value = entry.value;
        return 1;
    }

    if (klass->prototype) {
        return exprtk_class_get_static_field(klass->prototype, name, out_value);
    }

    return 0;
}

exprtk_value_t *exprtk_class_get_static_field_slot(
    exprtk_class_t *klass,
    const char *name,
    exprtk_class_t **owner_class)
{
    if (!klass || !name) return NULL;

    exprtk_value_t *slot = field_table_find_slot(klass->static_fields, name);
    if (slot) {
        if (owner_class) *owner_class = klass;
        return slot;
    }

    if (klass->prototype) {
        return exprtk_class_get_static_field_slot(klass->prototype, name, owner_class);
    }

    return NULL;
}

void exprtk_class_set_static_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t value)
{
    if (!klass || !name || !klass->static_fields) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)klass->static_fields;

    field_entry_t search;
    field_entry_t result;
    search.name = (char *)name;
    if (HTAB_OP(field_entry_t, do)(htab, search, HTAB_FIND, &result)) {
        result.value = value;
        HTAB_OP(field_entry_t, do)(htab, result, HTAB_REPLACE, &result);
        return;
    }

    size_t name_len = strlen(name);
    char *field_name = (char *)mem_alloc(klass->arena, name_len + 1);
    if (!field_name) return;
    memcpy(field_name, name, name_len + 1);

    field_entry_t entry;
    entry.name = field_name;
    entry.value = value;
    entry.access_level = EXPRTK_ACCESS_PUBLIC;
    entry.owner_class = klass;

    HTAB_OP(field_entry_t, do)(htab, entry, HTAB_INSERT, &result);
    klass->static_field_version++;
}

void exprtk_class_declare_static_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t value,
    int access_level)
{
    if (!klass || !name || !klass->static_fields) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)klass->static_fields;
    field_entry_t search;
    field_entry_t result;
    search.name = (char *)name;
    if (HTAB_OP(field_entry_t, do)(htab, search, HTAB_FIND, &result)) {
        result.value = value;
        result.access_level = access_level;
        result.owner_class = klass;
        HTAB_OP(field_entry_t, do)(htab, result, HTAB_REPLACE, &result);
        return;
    }

    size_t name_len = strlen(name);
    char *field_name = (char *)mem_alloc(klass->arena, name_len + 1);
    if (!field_name) return;
    memcpy(field_name, name, name_len + 1);

    field_entry_t entry;
    entry.name = field_name;
    entry.value = value;
    entry.access_level = access_level;
    entry.owner_class = klass;

    HTAB_OP(field_entry_t, do)(htab, entry, HTAB_INSERT, &result);
    klass->static_field_version++;
}

int exprtk_class_get_static_field_access(
    exprtk_class_t *klass,
    const char *name,
    exprtk_class_t **owner_class)
{
    if (!klass || !name) return EXPRTK_ACCESS_PUBLIC;

    field_entry_t entry;
    if (field_table_find(klass->static_fields, name, &entry)) {
        if (owner_class) *owner_class = entry.owner_class ? entry.owner_class : klass;
        return entry.access_level;
    }

    if (klass->prototype)
        return exprtk_class_get_static_field_access(klass->prototype, name, owner_class);

    return EXPRTK_ACCESS_PUBLIC;
}

int exprtk_class_declare_instance_field(
    exprtk_class_t *klass,
    const char *name,
    exprtk_value_t default_value,
    int has_default,
    int access_level)
{
    if (!klass || !name) return 0;
    if (!class_sync_prototype_instance_fields(klass)) return 0;
    return class_append_instance_field(klass, name, klass, access_level,
                                       default_value, has_default, 1, NULL);
}

int exprtk_class_get_instance_field_access(
    exprtk_class_t *klass,
    const char *name,
    exprtk_class_t **owner_class)
{
    size_t index = 0;
    if (!klass || !name) return EXPRTK_ACCESS_PUBLIC;
    if (!class_sync_prototype_instance_fields(klass)) return EXPRTK_ACCESS_PUBLIC;
    if (!class_find_instance_field_index(klass, name, &index)) return EXPRTK_ACCESS_PUBLIC;

    if (owner_class) {
        *owner_class = klass->instance_field_owners && klass->instance_field_owners[index]
            ? klass->instance_field_owners[index]
            : klass;
    }
    return klass->instance_field_access_levels
        ? klass->instance_field_access_levels[index]
        : EXPRTK_ACCESS_PUBLIC;
}

void exprtk_class_finalize_abstract_methods(exprtk_class_t *klass) {
    if (!klass) return;

    if (klass->prototype && klass->prototype->abstract_methods) {
        HTAB(method_entry_t) *parent_htab =
            (HTAB(method_entry_t) *)klass->prototype->abstract_methods;
        HTAB_EL(method_entry_t) *els_addr =
            VARR_ADDR(HTAB_EL(method_entry_t), parent_htab->els);
        htab_size_t bound = parent_htab->els_bound;

        for (htab_size_t i = 0; i < bound; ++i) {
            if (els_addr[i].hash != HTAB_DELETED_HASH &&
                !class_has_own_concrete_method_signature(
                    klass, els_addr[i].el.name, els_addr[i].el.func)) {
                if (els_addr[i].el.func) {
                    class_add_abstract_method_signature_entry(
                        klass, els_addr[i].el.name, els_addr[i].el.func);
                } else {
                    exprtk_class_add_abstract_method(klass, els_addr[i].el.name);
                }
            }
        }
    }

    if (klass->abstract_methods) {
        HTAB(method_entry_t) *htab = (HTAB(method_entry_t) *)klass->abstract_methods;
        if (htab->els_num > 0) klass->is_abstract = 1;
    }
}

int exprtk_class_is_abstract(exprtk_class_t *klass) {
    return klass && klass->is_abstract;
}

int exprtk_class_is_interface(exprtk_class_t *klass) {
    return klass && klass->is_interface;
}

void exprtk_class_destroy(exprtk_class_t *klass) {
    if (!klass) return;

    // Free hash tables
    if (klass->constructors) {
        HTAB(method_entry_t) *constructors_htab = (HTAB(method_entry_t) *)klass->constructors;
        HTAB_OP(method_entry_t, destroy)(&constructors_htab);
    }
    if (klass->methods) {
        HTAB(method_entry_t) *methods_htab = (HTAB(method_entry_t) *)klass->methods;
        HTAB_OP(method_entry_t, destroy)(&methods_htab);
    }
    if (klass->static_methods) {
        HTAB(method_entry_t) *static_methods_htab = (HTAB(method_entry_t) *)klass->static_methods;
        HTAB_OP(method_entry_t, destroy)(&static_methods_htab);
    }
    if (klass->static_fields) {
        HTAB(field_entry_t) *static_fields_htab = (HTAB(field_entry_t) *)klass->static_fields;
        HTAB_OP(field_entry_t, destroy)(&static_fields_htab);
    }
    if (klass->abstract_methods) {
        HTAB(method_entry_t) *abstract_methods_htab = (HTAB(method_entry_t) *)klass->abstract_methods;
        HTAB_OP(method_entry_t, destroy)(&abstract_methods_htab);
    }
    free(klass->instance_field_names);
    free(klass->instance_field_access_levels);
    free(klass->instance_field_owners);
    free(klass->instance_field_defaults);
    free(klass->instance_field_has_default);
    klass->instance_field_names = NULL;
    klass->instance_field_access_levels = NULL;
    klass->instance_field_owners = NULL;
    klass->instance_field_defaults = NULL;
    klass->instance_field_has_default = NULL;
    klass->instance_field_count = 0;
    klass->instance_field_capacity = 0;

    // Note: klass itself and all strings are arena-allocated,
    // so they will be freed when the arena is destroyed
}

/* ========================================================================
 * Instance Management Implementation
 * ======================================================================== */

exprtk_instance_t *exprtk_instance_create(
    exprtk_class_t *klass,
    mem_pool_t *arena)
{
    if (!klass || !arena) return NULL;

    exprtk_instance_t *instance = (exprtk_instance_t *)mem_alloc(arena, sizeof(exprtk_instance_t));
    if (!instance) return NULL;

    instance->klass = klass;
    instance->arena = arena;
    instance->field_slots = NULL;
    instance->field_slot_used = NULL;
    instance->field_slot_count = 0;
    instance->field_slot_capacity = 0;
    instance->field_version = 1;

    // Create fields hash table using MIR HTAB
    HTAB(field_entry_t) *fields_htab;
    HTAB_OP(field_entry_t, create)(&fields_htab, mir_std_alloc, 16,
                                   field_hash, field_eq, NULL, NULL);
    
    if (!fields_htab) {
        return NULL;
    }

    instance->fields = fields_htab;

    if (klass->instance_field_count > 0 &&
        instance_ensure_slot_capacity(instance, klass->instance_field_count)) {
        for (size_t i = 0; i < klass->instance_field_count; ++i) {
            if (!klass->instance_field_names[i]) continue;
            exprtk_value_t value;
            if (klass->instance_field_has_default &&
                klass->instance_field_has_default[i]) {
                value = klass->instance_field_defaults[i];
            } else {
                memset(&value, 0, sizeof(value));
                value.type = EXPRTK_VAL_NULL;
            }
            exprtk_instance_set_field(instance, klass->instance_field_names[i], value);
        }
    }

    return instance;
}

int exprtk_instance_get_field(
    exprtk_instance_t *instance,
    const char *name,
    exprtk_value_t *out_value)
{
    if (!instance || !name || !out_value) return 0;

    size_t index = 0;
    if (class_find_instance_field_index(instance->klass, name, &index) &&
        index < instance->field_slot_count &&
        instance->field_slot_used && instance->field_slot_used[index]) {
        *out_value = instance->field_slots[index];
        return 1;
    }

    if (!instance->fields) return 0;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)instance->fields;
    
    field_entry_t search;
    field_entry_t result;
    search.name = (char *)name;
    
    if (HTAB_OP(field_entry_t, do)(htab, search, HTAB_FIND, &result)) {
        *out_value = result.value;
        return 1;
    }

    return 0;
}

exprtk_value_t *exprtk_instance_get_field_slot(
    exprtk_instance_t *instance,
    const char *name)
{
    if (!instance || !name) return NULL;

    size_t index = 0;
    if (class_find_instance_field_index(instance->klass, name, &index) &&
        index < instance->field_slot_count &&
        instance->field_slot_used && instance->field_slot_used[index]) {
        return &instance->field_slots[index];
    }

    if (!instance->fields) return NULL;
    return field_table_find_slot(instance->fields, name);
}

void exprtk_instance_set_field(
    exprtk_instance_t *instance,
    const char *name,
    exprtk_value_t value)
{
    if (!instance || !name) return;
    if (!instance->fields) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)instance->fields;

    size_t slot_index = 0;
    int has_slot = class_ensure_instance_field(instance->klass, name, &slot_index) &&
                   instance_ensure_slot_capacity(instance, slot_index + 1);
    if (has_slot) {
        int was_used = instance->field_slot_used[slot_index] ? 1 : 0;
        instance->field_slots[slot_index] = value;
        instance->field_slot_used[slot_index] = 1;
        if (slot_index + 1 > instance->field_slot_count) {
            instance->field_slot_count = slot_index + 1;
        }
        if (!was_used) {
            instance->field_version++;
        }
    }

    field_entry_t search;
    field_entry_t result;
    search.name = (char *)name;
    if (HTAB_OP(field_entry_t, do)(htab, search, HTAB_FIND, &result)) {
        result.value = value;
        HTAB_OP(field_entry_t, do)(htab, result, HTAB_REPLACE, &result);
        return;
    }

    // Copy field name into arena
    size_t name_len = strlen(name);
    char *field_name = (char *)mem_alloc(instance->arena, name_len + 1);
    if (!field_name) return;
    memcpy(field_name, name, name_len + 1);

    // Create entry
    field_entry_t entry;
    entry.name = field_name;
    entry.value = value;
    entry.access_level = EXPRTK_ACCESS_PUBLIC;
    entry.owner_class = instance->klass;

    // Store in hash table
    HTAB_OP(field_entry_t, do)(htab, entry, HTAB_INSERT, &result);
    if (!has_slot) instance->field_version++;
}

void exprtk_instance_foreach_field(
    exprtk_instance_t *instance,
    exprtk_instance_field_visitor_t visitor,
    void *user_data)
{
    if (!instance || !instance->fields || !visitor) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)instance->fields;
    HTAB_EL(field_entry_t) *els_addr = VARR_ADDR(HTAB_EL(field_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            visitor(&els_addr[i].el.value, user_data);
        }
    }
}

void exprtk_instance_foreach_named_field(
    exprtk_instance_t *instance,
    exprtk_instance_named_field_visitor_t visitor,
    void *user_data)
{
    if (!instance || !instance->fields || !visitor) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)instance->fields;
    HTAB_EL(field_entry_t) *els_addr = VARR_ADDR(HTAB_EL(field_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            visitor(els_addr[i].el.name, &els_addr[i].el.value, user_data);
        }
    }
}

void exprtk_class_foreach_static_field(
    exprtk_class_t *klass,
    exprtk_instance_field_visitor_t visitor,
    void *user_data)
{
    if (!klass || !klass->static_fields || !visitor) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)klass->static_fields;
    HTAB_EL(field_entry_t) *els_addr = VARR_ADDR(HTAB_EL(field_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            visitor(&els_addr[i].el.value, user_data);
        }
    }
}

void exprtk_class_foreach_named_static_field(
    exprtk_class_t *klass,
    exprtk_instance_named_field_visitor_t visitor,
    void *user_data)
{
    if (!klass || !klass->static_fields || !visitor) return;

    HTAB(field_entry_t) *htab = (HTAB(field_entry_t) *)klass->static_fields;
    HTAB_EL(field_entry_t) *els_addr = VARR_ADDR(HTAB_EL(field_entry_t), htab->els);
    htab_size_t bound = htab->els_bound;

    for (htab_size_t i = 0; i < bound; ++i) {
        if (els_addr[i].hash != HTAB_DELETED_HASH) {
            visitor(els_addr[i].el.name, &els_addr[i].el.value, user_data);
        }
    }
}

exprtk_func_t *exprtk_instance_get_method(
    exprtk_instance_t *instance,
    const char *name)
{
    if (!instance || !name) return NULL;
    
    // Lookup in class (will search prototype chain)
    return exprtk_class_lookup_method(instance->klass, name, 0);
}

exprtk_func_t *exprtk_instance_get_method_arity(
    exprtk_instance_t *instance,
    const char *name,
    size_t argc)
{
    if (!instance || !name) return NULL;
    return exprtk_class_lookup_method_arity(instance->klass, name, 0, argc);
}

int exprtk_instance_of(
    exprtk_instance_t *instance,
    exprtk_class_t *klass)
{
    if (!instance || !klass) return 0;
    return exprtk_class_is_a(instance->klass, klass);
}

void exprtk_instance_destroy(exprtk_instance_t *instance) {
    if (!instance) return;

    // Free hash table
    if (instance->fields) {
        HTAB(field_entry_t) *fields_htab = (HTAB(field_entry_t) *)instance->fields;
        HTAB_OP(field_entry_t, destroy)(&fields_htab);
    }
    free(instance->field_slots);
    free(instance->field_slot_used);
    instance->field_slots = NULL;
    instance->field_slot_used = NULL;
    instance->field_slot_count = 0;
    instance->field_slot_capacity = 0;

    // Note: instance itself is arena-allocated
}

