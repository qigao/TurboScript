#include "ts_plugin.h" /* TurboScript canonical plugin-loader fixture. */

#ifndef FIXTURE_PLUGIN_NAME
#define FIXTURE_PLUGIN_NAME "loader_fixture"
#endif

#ifndef FIXTURE_ABI_VERSION
#define FIXTURE_ABI_VERSION SALTS_PLUGIN_ABI_VERSION
#endif

#if defined(FIXTURE_INIT_FAILURE)
static int *loader_fixture_failed_init_unload_count;
#endif

static void *loader_fixture_load(void *self, void *env, void *scratch) {
  (void)self;
#if defined(FIXTURE_INIT_FAILURE)
  loader_fixture_failed_init_unload_count = (int *)scratch;
  (void)env;
  return NULL;
#else
  (void)scratch;
  return env;
#endif
}

static void loader_fixture_unload(void *self, void *instance) {
  (void)self;
#if defined(FIXTURE_INIT_FAILURE)
  if (!instance && loader_fixture_failed_init_unload_count)
    ++*loader_fixture_failed_init_unload_count;
#else
  (void)instance;
#endif
}

static char loader_fixture_token;

static const ts_plugin_module_vtable loader_fixture_vtable = {
    .implementation = FIXTURE_PLUGIN_NAME,
    .capabilities = 0u,
    .load = loader_fixture_load,
    .unload = loader_fixture_unload,
};

static ts_plugin_module loader_fixture_module = {
    .self = &loader_fixture_token,
    .vtable = &loader_fixture_vtable,
};

static const salts_plugin_export loader_fixture_export = {
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = TS_PLUGIN_MODULE_CONTRACT_VERSION,
    .capabilities = 0u,
    .export_id = TS_PLUGIN_MODULE_EXPORT_ID,
    .contract_id = TS_PLUGIN_MODULE_CONTRACT_ID,
    .value.interface = {
        .desc = &ts_plugin_module_interface_meta,
        .value = &loader_fixture_module,
    },
};

static const salts_plugin_manifest loader_fixture_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = FIXTURE_ABI_VERSION,
    .plugin_id = FIXTURE_PLUGIN_NAME,
    .version = {1u, 0u, 0u},
    .exports = &loader_fixture_export,
    .export_count = 1u,
    .self = NULL,
    .start = NULL,
    .request_stop = NULL,
    .is_quiescent = NULL,
    .destroy = NULL,
};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  (void)host_abi;
  return &loader_fixture_manifest;
}
