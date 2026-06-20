/**
 * @file parser_plugin.c
 * @brief Parser 插件入口
 */
#include "ts_plugin.h"

/* Forward declarations from parser module */
void *parser_ctx_create(void);
void parser_ctx_destroy(void *p);
void parser_load(void *p, void *e, void *s);

TS_PLUGIN_STATEFUL(parser, parser_ctx_create, parser_load, parser_ctx_destroy)
