#include "ts_plugin.h"

void *db_ctx_create(void);
void  db_plugin_load(void *ctx, void *env, void *scratch);
void  db_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(data_bind, db_ctx_create, db_plugin_load, db_ctx_destroy)
