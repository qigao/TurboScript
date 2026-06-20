#include "ts_plugin.h"

void *sqlite_ctx_create(void);
void sqlite_load(void *ctx, void *env, void *scratch);
void sqlite_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(sqlite, sqlite_ctx_create, sqlite_load, sqlite_ctx_destroy)
