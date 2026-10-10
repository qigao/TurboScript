/** TurboScript native module contract, published through Salts Plugin.
 * Providers must be rebuilt for contract 2; ts_api_create is retired.
 * The passive manifest does not own per-context resources. Each open creates
 * a logical instance released only by successful close on its owner thread.
 */
#ifndef TS_PLUGIN_H
#define TS_PLUGIN_H

#include <salts/plugin_decl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct exprtk_env_s exprtk_env_t;
typedef struct exprtk_module_s exprtk_module_t;
void exprtk_env_add_module(exprtk_env_t *env, const exprtk_module_t *mod);

#define TS_PLUGIN_ABI_VERSION 2U
#define TS_PLUGIN_CONTRACT_ID "qigao.turboscript.module"
#define TS_PLUGIN_OPEN_EXPORT "open"
#define TS_PLUGIN_CLOSE_EXPORT "close"

/* env and scratch are borrowed until close succeeds. A non-null instance
 * carries a close obligation even when a stateless provider aliases env.
 * NULL reports initialization failure without publishing an instance.
 * close returns 0 on success; otherwise the instance stays owned and retryable.
 * No calls or borrowed module values may remain when close begins. */
FunctionInvokeDeclAsAbiResult(fallible, void *, &cmeta_type_void_ptr,
    CMETA_ABI_OBJECT_POINTER, CMETA_RESULT_OWNED | CMETA_RESULT_NULLABLE,
    ts_plugin_open,
    (void *, env, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
        &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (void *, scratch, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
        &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
FunctionInvokeDeclAsAbiResult(fallible, int32_t, &cmeta_type_int32,
    CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, ts_plugin_close,
    (void *, instance, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
        &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));

#define TS_PLUGIN_EXPORTS(X) \
    X(function, ts_plugin_open, TS_PLUGIN_OPEN_EXPORT, TS_PLUGIN_CONTRACT_ID, TS_PLUGIN_ABI_VERSION, 0) \
    X(function, ts_plugin_close, TS_PLUGIN_CLOSE_EXPORT, TS_PLUGIN_CONTRACT_ID, TS_PLUGIN_ABI_VERSION, 0)

/* Custom providers define the exact open/close functions above, then publish. */
#define TS_PLUGIN_PUBLISH(plugin_name) \
    CMETA_PLUGIN_DECLARE(ts_module, plugin_name, (2, 0, 0), \
        TS_PLUGIN_EXPORTS, CMETA_PLUGIN_PASSIVE());

#define TS_PLUGIN_MODULE(plugin_name, module_fn) \
    void *ts_plugin_open(void *env, void *scratch) { \
        const exprtk_module_t *mod = module_fn(); \
        (void)scratch; \
        if (!env || !mod) return NULL; \
        exprtk_env_add_module((exprtk_env_t *)env, mod); \
        return env; \
    } \
    int32_t ts_plugin_close(void *instance) { (void)instance; return 0; } \
    TS_PLUGIN_PUBLISH(#plugin_name)

/* Use CHECKED when cleanup can fail (for example native I/O drain). */
#define TS_PLUGIN_STATEFUL_CHECKED(plugin_name, create_fn, loader_fn, close_fn) \
    void *ts_plugin_open(void *env, void *scratch) { \
        void *ctx; \
        if (!env || !scratch) return NULL; \
        ctx = (void *)create_fn(); \
        if (!ctx) return NULL; \
        loader_fn(ctx, env, scratch); \
        return ctx; \
    } \
    int32_t ts_plugin_close(void *instance) { return instance ? close_fn(instance) : 0; } \
    TS_PLUGIN_PUBLISH(#plugin_name)

#define TS_PLUGIN_STATEFUL(plugin_name, create_fn, loader_fn, destroy_fn) \
    static int ts_module_close(void *instance) { destroy_fn(instance); return 0; } \
    TS_PLUGIN_STATEFUL_CHECKED(plugin_name, create_fn, loader_fn, ts_module_close)

#ifdef __cplusplus
}
#endif
#endif
