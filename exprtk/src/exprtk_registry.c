/**
 * @file exprtk_registry.c
 * @brief Unified registry for module-based built-in function dispatch.
 */

#include "exprtk_internal.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define REGISTRY_INITIAL_CAP 256

static exprtk_func_entry_t *g_registry = NULL;
static size_t g_registry_count = 0;
static size_t g_registry_cap = 0;
static int g_registry_ready = 0;

static exprtk_value_t exprtk_call_undefined(exprtk_env_t *env, const char *name) {
    exprtk_value_t zero = { EXPRTK_VAL_NUMBER, {0.0} };

    if (!env) return zero;

    snprintf(env->error_msg, sizeof(env->error_msg), "Undefined function '%s'",
             name ? name : "<null>");
    env->error_line = env->last_line;
    env->error_column = env->last_column;
    env->flow = exprtk_FLOW_THROW;
    env->error_value = exprtk_val_str(tstr_v_from_cstr(env->error_msg));
    return zero;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const exprtk_func_entry_t *)a)->name,
                  ((const exprtk_func_entry_t *)b)->name);
}

static int search_cmp(const void *key, const void *entry) {
    return strcmp((const char *)key, ((const exprtk_func_entry_t *)entry)->name);
}

static double exprtk_numeric_value(exprtk_value_t value) {
    if (value.type == EXPRTK_VAL_INTEGER) return (double)value.data.integer;
    if (value.type == EXPRTK_VAL_NUMBER) return value.data.number;
    return 0.0;
}

static exprtk_value_t *normalize_numeric_args(exprtk_value_t *args, size_t argc, exprtk_value_t *stack_args,
                                              size_t stack_cap, int *needs_free) {
    if (needs_free) *needs_free = 0;
    if (!args || argc == 0) return args;

    exprtk_value_t *normalized = stack_args;
    if (argc > stack_cap) {
        normalized = (exprtk_value_t *)malloc(argc * sizeof(exprtk_value_t));
        if (!normalized) return args;
        if (needs_free) *needs_free = 1;
    }

    for (size_t i = 0; i < argc; ++i) {
        normalized[i] = args[i];
        if (normalized[i].type == EXPRTK_VAL_INTEGER) {
            normalized[i] = exprtk_val_num((double)normalized[i].data.integer);
        }
    }

    return normalized;
}

static int builtin_needs_raw_args(const char *name) {
    const char *entry = name ? strrchr(name, '.') : NULL;
    if (!name) return 0;
    entry = entry ? entry + 1 : name;
    return strcmp(entry, "typeof") == 0 ||
           strcmp(entry, "is_int64") == 0 ||
           strcmp(entry, "is_bool") == 0 ||
           strcmp(entry, "is_bytes") == 0 ||
           strcmp(entry, "is_decimal") == 0;
}

/* =========================================================================
 * Module cache: sorted flat array of all per-env module entries
 * ========================================================================= */

/** Build sorted cache of all module entries for O(log n) lookup. */
static void build_mod_cache(exprtk_env_t *env) {
    if (!env || !env->modules || env->module_count == 0) return;

    /* Count total entries across all modules */
    size_t total = 0;
    for (size_t m = 0; m < env->module_count; m++)
        total += env->modules[m]->count;
    if (total == 0) return;

    /* Allocate and flatten */
    exprtk_func_entry_t *cache = (exprtk_func_entry_t *)malloc(total * sizeof(exprtk_func_entry_t));
    if (!cache) return;

    size_t idx = 0;
    for (size_t m = 0; m < env->module_count; m++) {
        const exprtk_module_t *mod = env->modules[m];
        for (size_t i = 0; i < mod->count; i++)
            cache[idx++] = mod->entries[i];
    }

    /* Sort by name for binary search */
    qsort(cache, total, sizeof(exprtk_func_entry_t), entry_cmp);

    env->mod_cache = cache;
    env->mod_cache_count = total;
}

/** O(log n) lookup in the sorted module cache. */
static exprtk_builtin_fn mod_cache_find(exprtk_env_t *env, const char *name) {
    if (!env->mod_cache) build_mod_cache(env);
    if (!env->mod_cache || env->mod_cache_count == 0) return NULL;

    const exprtk_func_entry_t *hit = (const exprtk_func_entry_t *)bsearch(
        name, (exprtk_func_entry_t *)env->mod_cache, env->mod_cache_count,
        sizeof(exprtk_func_entry_t), search_cmp);
    return hit ? hit->fn : NULL;
}

static exprtk_builtin_fn mod_find_in_named_module(exprtk_env_t *env, const char *module_name,
                                                  size_t module_name_len,
                                                  const char *entry_name) {
    if (!env || !module_name || !entry_name) return NULL;

    for (exprtk_env_t *e = env; e; e = e->parent) {
        for (size_t m = 0; m < e->module_count; ++m) {
            const exprtk_module_t *mod = e->modules[m];
            if (!mod || !mod->module_name) continue;
            if (strlen(mod->module_name) != module_name_len ||
                strncmp(mod->module_name, module_name, module_name_len) != 0)
                continue;

            for (size_t i = 0; i < mod->count; ++i) {
                if (strcmp(mod->entries[i].name, entry_name) == 0)
                    return mod->entries[i].fn;
            }
        }
    }
    return NULL;
}

static int is_global_compat_namespace(const char *module_name, size_t module_name_len) {
    static const char *const names[] = {"math", "stats", "string", "io", "core", "regex", NULL};
    for (size_t i = 0; names[i]; ++i) {
        if (strlen(names[i]) == module_name_len &&
            strncmp(names[i], module_name, module_name_len) == 0)
            return 1;
    }
    return 0;
}

void exprtk_registry_add_module(const exprtk_module_t *mod) {
    if (!mod) return;
    for (size_t i = 0; i < mod->count; ++i) {
        if (g_registry_count >= g_registry_cap) {
            size_t new_cap = g_registry_cap == 0 ? REGISTRY_INITIAL_CAP : g_registry_cap * 2;
            exprtk_func_entry_t *grown = (exprtk_func_entry_t *)realloc(g_registry, new_cap * sizeof(exprtk_func_entry_t));
            if (!grown) return;
            g_registry = grown;
            g_registry_cap = new_cap;
        }
        g_registry[g_registry_count++] = mod->entries[i];
    }
    g_registry_ready = 0; // Need re-sort
}

void exprtk_registry_init(void) {
    if (g_registry_ready) return;

    /* Register core module by default */
    exprtk_registry_add_module(exprtk_module_core());

    qsort(g_registry, g_registry_count, sizeof(exprtk_func_entry_t), entry_cmp);
    g_registry_ready = 1;
}

exprtk_builtin_fn exprtk_registry_find(const char *name) {
    if (!g_registry_ready) exprtk_registry_init();
    if (g_registry_count == 0) return NULL;

    /* 1. Try exact match (could be "sum" or "math.sum") */
    const exprtk_func_entry_t *hit = (const exprtk_func_entry_t *)bsearch(
        name, g_registry, g_registry_count,
        sizeof(exprtk_func_entry_t), search_cmp);
    if (hit) return hit->fn;

    /* 2. If name doesn't have a dot, it might be registered with a prefix we don't know here.
     * But our current registry flattens everything.
     * If a module "math" has "sum", it's currently registered as "sum".
     * If we want to support "math.sum", we need to register it as both or handle dots.
     */
    return NULL;
}

exprtk_builtin_fn exprtk_find_builtin(const char *name, exprtk_env_t *env) {
    if (env) {
        exprtk_builtin_fn fn = mod_cache_find(env, name);
        if (fn) return fn;

        const char *dot = strchr(name, '.');
        if (dot) {
            fn = mod_find_in_named_module(env, name, (size_t)(dot - name), dot + 1);
            if (fn) return fn;
        }
    }
    return exprtk_registry_find(name);
}

exprtk_value_t exprtk_call_internal(const char *name, size_t argc,
                                    exprtk_value_t *args, exprtk_env_t *env,
                                    mem_pool_t *arena) {
    exprtk_value_t stack_args[8];
    int normalized_needs_free = 0;
    exprtk_value_t *normalized_args = normalize_numeric_args(args, argc, stack_args, 8, &normalized_needs_free);
    exprtk_value_t *builtin_args = builtin_needs_raw_args(name) ? args : normalized_args;

    /* 1. Check native/script functions in environment */
    if (env) {
        exprtk_env_t *curr_env_iter = env;
        while (curr_env_iter) {
            exprtk_func_t *f = curr_env_iter->funcs;
            while (f) {
                if (strcmp(f->name, name) == 0) {
                    if (f->is_script) {
                        exprtk_value_t result = eval_script_function(f, argc, args, env, env);
                        if (normalized_needs_free) free(normalized_args);
                        return result;
                    } else {
                        exprtk_value_t result = f->data.native.fn(argc, normalized_args, f->data.native.user_data);
                        if (normalized_needs_free) free(normalized_args);
                        return result;
                    }
                }
                f = f->next;
            }
            curr_env_iter = curr_env_iter->parent;
        }
    }

    /* 1.25 Check if name is a variable holding a function value */
    if (env) {
        exprtk_value_t callee = exprtk_env_get(env, name);
        if (callee.type == EXPRTK_VAL_CLASS) {
            exprtk_value_t result =
                exprtk_oop_instantiate_class_value(callee, name, argc, args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
        if (callee.type == EXPRTK_VAL_BOUND_METHOD) {
            exprtk_value_t result = exprtk_oop_call_bound_method(callee, argc, args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
        if (callee.type == EXPRTK_VAL_FUNCTION && callee.data.function.body) {
            exprtk_value_t result = exprtk_call_function_value(callee, argc, args, env);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
    }

    /* 1.5 Per-env module cache — O(log n) sorted-array lookup */
    if (env) {
        /* Try non-namespaced first */
        exprtk_builtin_fn mod_fn = mod_cache_find(env, name);
        if (mod_fn) {
            exprtk_value_t result = mod_fn(argc, builtin_args, env, arena);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }

        /* Try namespaced: "module.func" only inside the named module. */
        const char *dot = strchr(name, '.');
        if (dot) {
            mod_fn = mod_find_in_named_module(env, name, (size_t)(dot - name), dot + 1);
            if (mod_fn) {
                exprtk_value_t result = mod_fn(argc, builtin_args, env, arena);
                if (normalized_needs_free) free(normalized_args);
                return result;
            }
        }
    }

    /* 2. Module registry — O(log n) sorted-array lookup */
    /* Try full name first (e.g., "vec.reverse") */
    exprtk_builtin_fn mod_fn = exprtk_registry_find(name);
    if (mod_fn) {
        exprtk_value_t result = mod_fn(argc, builtin_args, env, arena);
        if (normalized_needs_free) free(normalized_args);
        return result;
    }

    /* Check known global built-in namespaces too.  Plugin namespaces are handled
     * by env modules above and must not fall back to unrelated short names. */
    const char *final_dot = strchr(name, '.');
    if (final_dot && is_global_compat_namespace(name, (size_t)(final_dot - name))) {
        mod_fn = exprtk_registry_find(final_dot + 1);
        if (mod_fn) {
            exprtk_value_t result = mod_fn(argc, builtin_args, env, arena);
            if (normalized_needs_free) free(normalized_args);
            return result;
        }
    }

    if (normalized_needs_free) free(normalized_args);
    return exprtk_call_undefined(env, name);
}
