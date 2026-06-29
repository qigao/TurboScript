#include "ts_plugin.h"

void *rfg_ctx_create(void);
void rfg_plugin_load(void *ctx, void *env, void *scratch);
void rfg_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(rules_forge, rfg_ctx_create, rfg_plugin_load, rfg_ctx_destroy)
