/**
 * @file ts_plugin_loader.h
 * @brief TurboScript host module admission and Salts Plugin lease owner.
 */
#ifndef TS_PLUGIN_LOADER_H
#define TS_PLUGIN_LOADER_H

#include "ts_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exclusive owner-thread handle: do not copy or mutate its registry/lease.
 * Exports and name are borrowed and become invalid when the lease is released. */
typedef struct ts_plugin_handle_s {
    cmeta_plugin_registry registry;
    cmeta_plugin_ref ref;
    cmeta_plugin_lease lease;
    const char *name; /* borrowed under lease */
    const cmeta_plugin_export *open_export;
    const cmeta_plugin_export *close_export;
    void *instance;
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
    TS_PLUGIN_ERROR_INITIALIZE = -8,
    TS_PLUGIN_ERROR_CLOSE = -9,
    TS_PLUGIN_ERROR_CONTRACT = -10
} ts_plugin_error_code_t;

typedef enum ts_plugin_error_stage_e {
    TS_PLUGIN_STAGE_NONE = 0,
    TS_PLUGIN_STAGE_ARGUMENT,
    TS_PLUGIN_STAGE_OPEN,
    TS_PLUGIN_STAGE_SYMBOL,
    TS_PLUGIN_STAGE_ABI,
    TS_PLUGIN_STAGE_INITIALIZE,
    TS_PLUGIN_STAGE_CLOSE
} ts_plugin_error_stage_t;

typedef struct ts_plugin_error_s {
    ts_plugin_error_code_t code;
    ts_plugin_error_stage_t stage;
    uint32_t native_code;
    char attempted_path[TS_PLUGIN_ERROR_PATH_CAPACITY];
    char message[TS_PLUGIN_ERROR_MESSAGE_CAPACITY];
} ts_plugin_error_t;

/**
 * Load and validate a plugin, optionally requiring its manifest plugin_id to
 * equal @p expected_name. The loader searches executable-relative locations
 * only when @p path is a filename without directory separators.
 *
 * @return TS_PLUGIN_ERROR_NONE on success, otherwise a negative error code.
 */
int ts_plugin_load_ex(const char *path, const char *expected_name,
                      ts_plugin_handle_t **out, ts_plugin_error_t *error);

/**
 * Load and admit the current Salts manifest and TurboScript contract.
 * @return handle on success, NULL on failure.
 */
ts_plugin_handle_t *ts_plugin_load(const char *path);

/**
 * Invoke the admitted open(env, scratch) and retain its instance under lease.
 * @return 0 on success, -1 on failure.
 */
int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch);

/** Initialize a validated plugin and report a structured failure. */
int ts_plugin_init_ex(ts_plugin_handle_t *h, void *env, void *scratch,
                      ts_plugin_error_t *error);

/**
 * Close the instance and release/unload the registry. Requires no remaining
 * script calls or borrowed values. NULL succeeds. Failure preserves the handle
 * for a retry and closes new admission; success consumes the handle.
 */
int ts_plugin_unload_ex(ts_plugin_handle_t *h, ts_plugin_error_t *error);

/** Convenience wrapper: aborts on close failure; use unload_ex to retry. */
void ts_plugin_unload(ts_plugin_handle_t *h);

#ifdef __cplusplus
}
#endif

#endif /* TS_PLUGIN_LOADER_H */
