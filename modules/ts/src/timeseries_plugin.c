#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_timeseries(void);

TS_PLUGIN_MODULE(timeseries, exprtk_module_timeseries)

