#include "ts_plugin.h"
#include "sqlite_ctx.h"
#include "sqlite_provider.h"

#include <salts/thread.h>
#include <stdlib.h>

typedef struct sqlite_module_instance_s {
  sqlite_provider *provider;
  void *session;
} sqlite_module_instance;

static void *sqlite_module_load(void *self, void *env, void *scratch) {
  sqlite_module_instance *instance;
  sqlite_provider *provider = sqlite_provider_export();
  void *session;
  (void)self;

  if (!provider || !sqlite_provider_valid(provider) || !env || !scratch)
    return NULL;

  session = sqlite_provider_create(provider);
  if (!session) return NULL;

  instance = (sqlite_module_instance *)calloc(1, sizeof(*instance));
  if (!instance) {
    sqlite_provider_destroy(provider, session);
    return NULL;
  }

  if (!sqlite_load_provider(provider, session, env, scratch)) {
    sqlite_provider_destroy(provider, session);
    free(instance);
    return NULL;
  }

  instance->provider = provider;
  instance->session = session;
  return instance;
}

static void sqlite_module_unload(void *self, void *opaque) {
  sqlite_module_instance *instance = (sqlite_module_instance *)opaque;
  (void)self;
  if (!instance) return;
  if (instance->provider && sqlite_provider_valid(instance->provider) &&
      instance->session) {
    sqlite_provider_destroy(instance->provider, instance->session);
  }
  instance->session = NULL;
  instance->provider = NULL;
  free(instance);
}

static char sqlite_module_token;
static const ts_plugin_module_vtable sqlite_module_vtable = {
    .implementation = "sqlite",
    .capabilities = 0u,
    .load = sqlite_module_load,
    .unload = sqlite_module_unload,
};
static ts_plugin_module sqlite_module = {
    &sqlite_module_token,
    &sqlite_module_vtable
};

static salts_plugin_export sqlite_exports[2];
static salts_once_t sqlite_exports_once = SALTS_ONCE_INIT;

static void sqlite_exports_init(void) {
  sqlite_provider *provider = sqlite_provider_export();

  sqlite_exports[0] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
      .contract_version = TS_PLUGIN_MODULE_CONTRACT_VERSION,
      .capabilities = 0u,
      .export_id = TS_PLUGIN_MODULE_EXPORT_ID,
      .contract_id = TS_PLUGIN_MODULE_CONTRACT_ID,
      .value.interface = {
          .desc = &ts_plugin_module_interface_meta,
          .value = &sqlite_module,
      },
  };

  sqlite_exports[1] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
      .contract_version = SQLITE_PROVIDER_CONTRACT_VERSION,
      .capabilities = 0u,
      .export_id = SQLITE_PROVIDER_EXPORT_ID,
      .contract_id = SQLITE_PROVIDER_CONTRACT_ID,
      .value.interface = {
          .desc = sqlite_provider_interface(),
          .value = provider,
      },
  };
}

static const salts_plugin_manifest sqlite_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "sqlite",
    .version = {1u, 0u, 0u},
    .exports = sqlite_exports,
    .export_count = 2u,
    .self = NULL,
    .start = NULL,
    .request_stop = NULL,
    .is_quiescent = NULL,
    .destroy = NULL,
};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  if (host_abi != SALTS_PLUGIN_ABI_VERSION) return NULL;
  salts_once(&sqlite_exports_once, sqlite_exports_init);
  if (!sqlite_provider_valid(sqlite_provider_export())) return NULL;
  return &sqlite_manifest;
}
