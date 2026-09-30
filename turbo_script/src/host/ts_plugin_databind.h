#ifndef TURBO_SCRIPT_PLUGIN_DATABIND_H
#define TURBO_SCRIPT_PLUGIN_DATABIND_H

#include "ts_plugin_loader.h"
#include "exprtk_module.h"

int ts_plugin_databind_bind(
    ts_plugin_handle_t *handle, exprtk_env_t *env,
    ts_plugin_error_t *error);

int ts_plugin_databind_covers(
    const ts_plugin_handle_t *handle, const char *export_id);

void ts_plugin_databind_clear(ts_plugin_handle_t *handle);

#endif /* TURBO_SCRIPT_PLUGIN_DATABIND_H */
