#include "ts_plugin.h"

const exprtk_module_t *exprtk_module_timeseries(void);

TS_PLUGIN_MODULE(ts, exprtk_module_timeseries)

