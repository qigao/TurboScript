#include "ts_plugin.h"

void *net_ctx_create(void);
void net_load(void *ctx, void *env, void *scratch);
void net_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(net, net_ctx_create, net_load, net_ctx_destroy)
