/**
 * @file ts_plugin_loader.h
 * @brief Cross-platform plugin loader (dlopen / LoadLibrary).
 */
#ifndef TS_PLUGIN_LOADER_H
#define TS_PLUGIN_LOADER_H

#include "ts_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_plugin_handle_s {
    void              *dl_handle;   /* OS DLL handle */
    const ts_plugin_t *plugin;      /* plugin descriptor */
    void              *instance;    /* returned by load() */
} ts_plugin_handle_t;

/**
 * dlopen the plugin at @p path and resolve ts_api_create.
 * @return handle on success, NULL on failure.
 */
ts_plugin_handle_t *ts_plugin_load(const char *path);

/**
 * Call plugin->load(env, scratch, coro) and store the instance.
 * @return 0 on success, -1 on failure.
 */
int ts_plugin_init(ts_plugin_handle_t *h, void *env, void *scratch );

/**
 * Call plugin->unload(instance), dlclose, free handle.
 */
void ts_plugin_unload(ts_plugin_handle_t *h);

#ifdef __cplusplus
}
#endif

#endif /* TS_PLUGIN_LOADER_H */
