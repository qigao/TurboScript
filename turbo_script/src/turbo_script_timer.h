#ifndef TURBO_SCRIPT_TIMER_H
#define TURBO_SCRIPT_TIMER_H

#include "turbo_script_internal.h"

enum { TS_TIMER_DEFAULT_CAPACITY = 64 };

int ts_timer_scheduler_init(turbo_script_ctx_t *ctx, size_t capacity);
void ts_timer_scheduler_shutdown(turbo_script_ctx_t *ctx);
void ts_timer_scheduler_destroy(turbo_script_ctx_t *ctx);
void ts_timer_register_functions(turbo_script_ctx_t *ctx);
size_t ts_timer_scheduler_capacity(turbo_script_ctx_t *ctx);

#endif /* TURBO_SCRIPT_TIMER_H */
