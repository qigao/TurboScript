#ifndef TS_PLUGIN_DATABIND_H
#define TS_PLUGIN_DATABIND_H

#include "exprtk_module.h"

#include <stddef.h>

struct ts_plugin_handle_s;

/*
 * Compile the generated DataBind Service catalog under the handle's retained
 * Salts Plugin lease. No script function is registered here; registration is
 * performed atomically together with raw scalar Function exports by the host
 * loader.
 */
int ts_plugin_databind_prepare(
    struct ts_plugin_handle_s *handle,
    char *error, size_t error_size);

size_t ts_plugin_databind_count(
    const struct ts_plugin_handle_s *handle);

/* Return the private immutable binding cache row for one catalog export. */
void *ts_plugin_databind_find(
    const struct ts_plugin_handle_s *handle,
    const char *export_id);

/* Generic ExprTk callback for one prepared DataBind Service operation. */
exprtk_value_t ts_plugin_databind_call(
    size_t argc, exprtk_value_t *args,
    exprtk_env_t *env, void *user_data);

/* Destroy BindingPlans/codec while the Plugin lease is still live. */
void ts_plugin_databind_clear(
    struct ts_plugin_handle_s *handle);

#endif /* TS_PLUGIN_DATABIND_H */
