#ifndef TS_PLUGIN_SERVICE_BINDING_H
#define TS_PLUGIN_SERVICE_BINDING_H

#include "ts_plugin_loader.h"
#include "exprtk.h"

#include <stddef.h>

int ts_plugin_service_bindings_prepare(
    ts_plugin_handle_t *handle, exprtk_env_t *env,
    char *error, size_t error_size);

int ts_plugin_service_binding_get(
    const ts_plugin_handle_t *handle, const char *export_id,
    exprtk_native_fn *out_fn, void **out_user_data);

void ts_plugin_service_bindings_destroy(ts_plugin_handle_t *handle);

#endif /* TS_PLUGIN_SERVICE_BINDING_H */
