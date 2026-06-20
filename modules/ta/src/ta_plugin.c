#include "ts_plugin.h"
const exprtk_module_t *exprtk_module_ta(void);
TS_PLUGIN_MODULE(ta, exprtk_module_ta)
