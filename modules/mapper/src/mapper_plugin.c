#include "ts_plugin.h"

void *mapper_ctx_create(void);
void mapper_load(void *ctx, void *env, void *scratch);
void mapper_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(mapper, mapper_ctx_create, mapper_load, mapper_ctx_destroy)
