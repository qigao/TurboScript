#include "ts_plugin.h"

void *wasm_ctx_create(void);
void wasm_load(void *ctx, void *env, void *scratch);
void wasm_ctx_destroy(void *ctx);

TS_PLUGIN_STATEFUL(wasm, wasm_ctx_create, wasm_load, wasm_ctx_destroy)

