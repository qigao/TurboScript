#ifndef TURBO_SCRIPT_TASK_H
#define TURBO_SCRIPT_TASK_H

#include "turbo_script_internal.h"

enum { TS_TASK_DEFAULT_CAPACITY = 64 };

typedef void (*ts_task_completion_fn)(turbo_script_ctx_t *ctx, int cancelled,
                                      const char *error, void *arg);

int ts_task_scheduler_init(turbo_script_ctx_t *ctx, size_t capacity);
void ts_task_scheduler_shutdown(turbo_script_ctx_t *ctx);
void ts_task_scheduler_destroy(turbo_script_ctx_t *ctx);
void ts_task_register_functions(turbo_script_ctx_t *ctx);
int ts_task_spawn_managed(turbo_script_ctx_t *ctx, exprtk_value_t callback,
                          ts_task_completion_fn completion, void *completion_arg,
                          int64_t *id_out);
int ts_task_cancel_managed(turbo_script_ctx_t *ctx, int64_t id);
size_t ts_task_scheduler_capacity(turbo_script_ctx_t *ctx);

#endif /* TURBO_SCRIPT_TASK_H */
