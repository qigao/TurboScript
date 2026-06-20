#include "ts_plugin.h"
const exprtk_module_t *exprtk_module_strategy(void);
TS_PLUGIN_MODULE(strategy, exprtk_module_strategy)
