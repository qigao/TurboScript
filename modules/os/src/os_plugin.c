/**
 * @file os_plugin.c
 * @brief Stateful plugin entry point for the os module.
 */
#include "ts_plugin.h"

void *os_module_create(void);
void os_module_load(void *module, void *env, void *scratch);
void os_module_destroy(void *module);

TS_PLUGIN_STATEFUL(os, os_module_create, os_module_load, os_module_destroy)
