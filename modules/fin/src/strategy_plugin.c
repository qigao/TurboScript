#include "ts_plugin.h"
#include "fin_plugin_functions.h"

#include <salts/thread.h>

const exprtk_module_t *exprtk_module_strategy(void);

static void *fin_module_load(void *self, void *env, void *scratch) {
  const exprtk_module_t *module;
  (void)self;
  (void)scratch;
  module = exprtk_module_strategy();
  if (!env || !module) return NULL;
  exprtk_env_add_module((exprtk_env_t *)env, module);
  return env;
}

static void fin_module_unload(void *self, void *instance) {
  (void)self;
  (void)instance;
}

static char fin_module_token;

static const ts_plugin_module_vtable fin_module_vtable = {
    .implementation = "fin",
    .capabilities = 0u,
    .load = fin_module_load,
    .unload = fin_module_unload,
};

static ts_plugin_module fin_module = {
    .self = &fin_module_token,
    .vtable = &fin_module_vtable,
};

static bool SALTS_PLUGIN_CALL fin_kelly_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  double win_rate;
  double avg_win;
  double avg_loss;

  if (context != NULL || !return_storage || !params ||
      param_count != 3u || !params[0] || !params[1] || !params[2])
    return false;

  win_rate = *(const double *)params[0];
  avg_win = *(const double *)params[1];
  avg_loss = *(const double *)params[2];
  *(double *)return_storage =
      ts_fin_kelly(win_rate, avg_win, avg_loss);
  return true;
}

/*
 * Migration state under one Salts Plugin ABI:
 * - turboscript.module keeps the remaining legacy language facade mounted;
 * - strategy.kelly is already a canonical Function export.
 * Both are protected by the same manifest lease; this is not a dual loader or
 * a parallel Plugin ABI.
 */
static salts_plugin_export fin_exports[2];
static salts_once_t fin_exports_once = SALTS_ONCE_INIT;

static void fin_exports_init(void) {
  fin_exports[0] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
      .contract_version = TS_PLUGIN_MODULE_CONTRACT_VERSION,
      .capabilities = 0u,
      .export_id = TS_PLUGIN_MODULE_EXPORT_ID,
      .contract_id = TS_PLUGIN_MODULE_CONTRACT_ID,
      .value.interface = {
          .desc = &ts_plugin_module_interface_meta,
          .value = &fin_module,
      },
  };

  fin_exports[1] = (salts_plugin_export){
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_FUNCTION,
      .contract_version = 1u,
      .capabilities = 0u,
      .export_id = "strategy.kelly",
      .contract_id = "turboscript.fin.strategy.kelly",
      .value.function = {
          .desc = FunctionMeta(ts_fin_kelly),
          .abi = FunctionAbi(ts_fin_kelly),
          .context = NULL,
          .invoke = fin_kelly_invoke,
      },
  };
}

static const salts_plugin_manifest fin_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "fin",
    .version = {1u, 0u, 0u},
    .exports = fin_exports,
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
  salts_once(&fin_exports_once, fin_exports_init);
  return &fin_manifest;
}
