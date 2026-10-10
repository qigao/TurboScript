/* An old-style entry alone must never be admitted by the new loader. */
#include <salts/plugin.h>
CMETA_PLUGIN_QUERY_EXPORT const void *ts_api_create(void) { return NULL; }
