/**
 * @file coro_plugin.c
 * @brief 协程插件入口
 */
#include "ts_plugin.h"

/* Forward declarations from coro module */
void *coro_ctx_create_module(void);
void coro_ctx_destroy_module(void *p);
void coro_load(void *p, void *e, void *s);

TS_PLUGIN_STATEFUL(coro, coro_ctx_create_module, coro_load, coro_ctx_destroy_module)
