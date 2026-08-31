#include "ts_plugin.h"
const exprtk_module_t *exprtk_module_strategy(void);
TS_PLUGIN_MODULE(fin, exprtk_module_strategy)
