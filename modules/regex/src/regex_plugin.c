/**
 * @file regex_plugin.c
 * @brief Regex plugin entry point
 */
#include "regex_ctx.h"
#include "ts_plugin.h"

TS_PLUGIN_STATEFUL(regex, regex_ctx_create, regex_load, regex_ctx_destroy)
