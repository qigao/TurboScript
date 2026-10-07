/**
 * @file ts_plugin.h
 * @brief TurboScript module adapter published through the canonical Salts Plugin ABI.
 *
 * Salts owns DSO publication, loading, lifecycle and leases. TurboScript owns
 * only the per-script-context module adapter semantics: load(env, scratch)
 * mounts one module/context and returns its opaque context instance; unload()
 * releases that context instance.
 */
#ifndef TS_PLUGIN_H
#define TS_PLUGIN_H

#include <salts/plugin.h>
#include <stddef.h>
#include <stdint.h>

#ifndef EXPRTK_C_API
#define EXPRTK_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct exprtk_env_s exprtk_env_t;
typedef struct mem_pool_s mem_pool_t;
typedef struct exprtk_module_s exprtk_module_t;
EXPRTK_C_API void exprtk_env_add_module(exprtk_env_t *env,
                                        const exprtk_module_t *mod);

#define TS_PLUGIN_MODULE_EXPORT_ID "turboscript.module"
#define TS_PLUGIN_MODULE_CONTRACT_ID "turboscript.module"
#define TS_PLUGIN_MODULE_CONTRACT_VERSION 1u

/*
 * This interface is intentionally per-context. It is not the Salts DSO
 * lifecycle: the plugin manifest remains passive and Salts::Plugin owns the
 * module start/lease/stop/unload state machine.
 */
#define TS_PLUGIN_MODULE_METHODS(X, I) \
    X(I, R2, void *, load, void *, env, void *, scratch) \
    X(I, V1, void, unload, void *, instance)

CMETA_INTERFACE(ts_plugin_module, TS_PLUGIN_MODULE_METHODS);

#define TS_PLUGIN_DETAIL_EXPORT(plugin_name, load_fn, unload_fn)                 \
    static char ts__##plugin_name##_adapter_token;                              \
    static const ts_plugin_module_vtable ts__##plugin_name##_adapter_vtable = { \
        .implementation = #plugin_name,                                         \
        .capabilities = 0u,                                                     \
        .load = (load_fn),                                                      \
        .unload = (unload_fn),                                                  \
    };                                                                          \
    static ts_plugin_module ts__##plugin_name##_adapter = {                     \
        &ts__##plugin_name##_adapter_token,                                     \
        &ts__##plugin_name##_adapter_vtable                                     \
    };                                                                          \
    static const cmeta_plugin_export ts__##plugin_name##_export = {             \
        .struct_size = CMETA_PLUGIN_EXPORT_SIZE,                                \
        .kind = CMETA_PLUGIN_EXPORT_INTERFACE,                                  \
        .contract_version = TS_PLUGIN_MODULE_CONTRACT_VERSION,                  \
        .capabilities = 0u,                                                     \
        .export_id = TS_PLUGIN_MODULE_EXPORT_ID,                                \
        .contract_id = TS_PLUGIN_MODULE_CONTRACT_ID,                            \
        .value.interface = {                                                    \
            .desc = &ts_plugin_module_interface_meta,                           \
            .value = &ts__##plugin_name##_adapter,                             \
        },                                                                      \
    };                                                                          \
    static const cmeta_plugin_manifest ts__##plugin_name##_manifest = {         \
        .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,                              \
        .abi_version = CMETA_PLUGIN_ABI_VERSION,                                \
        .plugin_id = #plugin_name,                                              \
        .version = {1u, 0u, 0u},                                               \
        .exports = &ts__##plugin_name##_export,                                \
        .export_count = 1u,                                                     \
        .self = NULL,                                                           \
        .start = NULL,                                                          \
        .request_stop = NULL,                                                   \
        .is_quiescent = NULL,                                                   \
        .destroy = NULL,                                                        \
    };                                                                          \
    CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL    \
    cmeta_plugin_query(uint32_t host_abi) {                                     \
        return host_abi == CMETA_PLUGIN_ABI_VERSION                            \
                   ? &ts__##plugin_name##_manifest                             \
                   : NULL;                                                      \
    }

/* Stateless module: mount one exprtk_module_t into the target environment. */
#define TS_PLUGIN_MODULE(plugin_name, module_fn)                                \
    static void *ts__##plugin_name##_load(void *self, void *env,               \
                                           void *scratch) {                     \
        const exprtk_module_t *module;                                          \
        (void)self;                                                             \
        (void)scratch;                                                          \
        module = module_fn();                                                   \
        if (env == NULL || module == NULL) return NULL;                         \
        exprtk_env_add_module((exprtk_env_t *)env, module);                     \
        return env;                                                             \
    }                                                                           \
    static void ts__##plugin_name##_unload(void *self, void *instance) {        \
        (void)self;                                                             \
        (void)instance;                                                         \
    }                                                                           \
    TS_PLUGIN_DETAIL_EXPORT(plugin_name, ts__##plugin_name##_load,              \
                            ts__##plugin_name##_unload)

/* Stateless module with context cleanup attached to the borrowed environment. */
#define TS_PLUGIN_MODULE_WITH_UNLOAD(plugin_name, module_fn, unload_fn)         \
    static void *ts__##plugin_name##_load(void *self, void *env,               \
                                           void *scratch) {                     \
        const exprtk_module_t *module;                                          \
        (void)self;                                                             \
        (void)scratch;                                                          \
        module = module_fn();                                                   \
        if (env == NULL || module == NULL) return NULL;                         \
        exprtk_env_add_module((exprtk_env_t *)env, module);                     \
        return env;                                                             \
    }                                                                           \
    static void ts__##plugin_name##_unload(void *self, void *instance) {        \
        (void)self;                                                             \
        if (instance != NULL) unload_fn((exprtk_env_t *)instance);              \
    }                                                                           \
    TS_PLUGIN_DETAIL_EXPORT(plugin_name, ts__##plugin_name##_load,              \
                            ts__##plugin_name##_unload)

/* Stateful module: create one context per TurboScript script context. */
#define TS_PLUGIN_STATEFUL(plugin_name, create_fn, loader_fn, destroy_fn)       \
    static void *ts__##plugin_name##_load(void *self, void *env,               \
                                           void *scratch) {                     \
        void *ctx;                                                              \
        (void)self;                                                             \
        ctx = (void *)create_fn();                                              \
        if (ctx == NULL) return NULL;                                           \
        loader_fn(ctx, env, scratch);                                           \
        return ctx;                                                             \
    }                                                                           \
    static void ts__##plugin_name##_unload(void *self, void *instance) {        \
        (void)self;                                                             \
        if (instance != NULL) destroy_fn(instance);                             \
    }                                                                           \
    TS_PLUGIN_DETAIL_EXPORT(plugin_name, ts__##plugin_name##_load,              \
                            ts__##plugin_name##_unload)

#ifdef __cplusplus
}
#endif

#endif /* TS_PLUGIN_H */
