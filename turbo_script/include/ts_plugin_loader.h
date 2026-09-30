/**
 * @file ts_plugin_loader.h
 * @brief TurboScript adapter over the canonical Salts plugin registry.
 */
#ifndef TS_PLUGIN_LOADER_H
#define TS_PLUGIN_LOADER_H

#include "ts_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_plugin_function_view_s {
    const salts_plugin_export *entry;         /* borrowed under the plugin lease */
    salts_plugin_function_invoke_fn invoke;   /* pre-bound exact adapter */
    void *context;                            /* provider-owned, lease-bound */
} ts_plugin_function_view_t;

typedef struct ts_plugin_handle_s {
    salts_plugin_registry registry;          /* owns the loaded DSO */
    salts_plugin_ref plugin_ref;             /* registry identity */
    salts_plugin_lease lease;                /* keeps manifest/code alive */
    const salts_plugin_manifest *manifest;   /* borrowed under lease */
    const salts_plugin_export *module_export;/* canonical module capability */
    ts_plugin_module *module;                /* optional borrowed interface handle */
    void *instance;                          /* optional per-script-context instance */
    void *function_bindings;                 /* private canonical Function binding cache */
    size_t function_binding_count;
    void *service_bindings;                  /* private DataBind Service BindingPlan cache */
    size_t service_binding_count;
    int initialized;
} ts_plugin_handle_t;

#define TS_PLUGIN_ERROR_PATH_CAPACITY 1024U
#define TS_PLUGIN_ERROR_MESSAGE_CAPACITY 512U

typedef enum ts_plugin_error_code_e {
    TS_PLUGIN_ERROR_NONE = 0,
    TS_PLUGIN_ERROR_INVALID_ARGUMENT = -1,
    TS_PLUGIN_ERROR_OPEN = -2,
    TS_PLUGIN_ERROR_SYMBOL = -3,
    TS_PLUGIN_ERROR_DESCRIPTOR = -4,
    TS_PLUGIN_ERROR_ABI = -5,
    TS_PLUGIN_ERROR_NAME = -6,
    TS_PLUGIN_ERROR_OUT_OF_MEMORY = -7,
    TS_PLUGIN_ERROR_INITIALIZE = -8
} ts_plugin_error_code_t;

typedef enum ts_plugin_error_stage_e {
    TS_PLUGIN_STAGE_NONE = 0,
    TS_PLUGIN_STAGE_ARGUMENT,
    TS_PLUGIN_STAGE_OPEN,
    TS_PLUGIN_STAGE_SYMBOL,
    TS_PLUGIN_STAGE_ABI,
    TS_PLUGIN_STAGE_INITIALIZE
} ts_plugin_error_stage_t;

typedef struct ts_plugin_error_s {
    ts_plugin_error_code_t code;
    ts_plugin_error_stage_t stage;
    uint32_t native_code;
    char attempted_path[TS_PLUGIN_ERROR_PATH_CAPACITY];
    char message[TS_PLUGIN_ERROR_MESSAGE_CAPACITY];
} ts_plugin_error_t;

/**
 * Load and validate a plugin, optionally requiring its descriptor name to
 * equal @p expected_name. The loader searches executable-relative locations
 * only when @p path is a filename without directory separators.
 *
 * @return TS_PLUGIN_ERROR_NONE on success, otherwise a negative error code.
 */
int ts_plugin_load_ex(const char *path, const char *expected_name,
                      ts_plugin_handle_t **out, ts_plugin_error_t *error);

/**
 * Resolve the configured TurboScript plugin path and load it through
 * Salts::Plugin. The DSO must export the canonical salts_plugin_query entry.
 * @return handle on success, NULL on failure.
 */
ts_plugin_handle_t *ts_plugin_load(const char *path);

/*
 * Control-plane lookup over the already-bound canonical Function cache.
 * Performs no registry/symbol/manifest discovery. Returned descriptor/code
 * pointers are valid only while the handle's Salts Plugin lease remains held.
 */
int ts_plugin_find_bound_function(
    const ts_plugin_handle_t *handle, const char *export_id,
    ts_plugin_function_view_t *out);

/**
 * Call the canonical TurboScript module interface load(env, scratch) method
 * while the Salts plugin lease is held, and store the per-context instance.
 * @return 0 on success, -1 on failure.
 */
int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch );

/** Initialize a validated plugin and report a structured failure. */
int ts_plugin_init_ex(ts_plugin_handle_t *h, void *env, void *scratch,
                      ts_plugin_error_t *error);

/**
 * Release the per-context module instance, then release the Salts plugin lease,
 * quiesce and unload the DSO.
 */
void ts_plugin_unload(ts_plugin_handle_t *h);

#ifdef __cplusplus
}
#endif

#endif /* TS_PLUGIN_LOADER_H */
