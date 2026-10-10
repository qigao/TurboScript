#include "ts_plugin.h"

void *net_ctx_create(void);
void net_load(void *ctx, void *env, void *scratch);
int net_ctx_try_destroy(void *ctx);

TS_PLUGIN_STATEFUL_CHECKED(net, net_ctx_create, net_load, net_ctx_try_destroy)
