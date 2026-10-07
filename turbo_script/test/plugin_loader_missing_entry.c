#include <salts/plugin.h>

SALTS_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *SALTS_PLUGIN_CALL
not_salts_plugin_query(uint32_t host_abi) {
  (void)host_abi;
  return NULL;
}
