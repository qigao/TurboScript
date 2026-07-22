#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "turbo_coro_context.h"
#include "CoroNet/turbo_coro_object_pool.h"
#include "turbo_parser.h"
#include "turbo_script.h"
#include "turbo_thread.h"

enum { TURBO_SCRIPT_CORO_STACK_SIZE = 512 * 1024 };

static coro_context_t *create_script_coro_context(void) {
    const coro_object_pool_config_t pool_config = {
        16,
        1024,
        TURBO_SCRIPT_CORO_STACK_SIZE,
    };
    return coro_context_create_ex(NULL, &pool_config);
}

static int run_scheduled_callbacks(turbo_script_ctx_t *ctx, coro_context_t *coro_ctx,
                                   int script_result) {
    int loop_result;
    if (script_result < 0 ||
        (turbo_script_timer_active_count(ctx) == 0 &&
         turbo_script_task_active_count(ctx) == 0)) return script_result;
    loop_result = coro_context_run(coro_ctx, TURBO_RUN_DEFAULT);
    if (loop_result < 0) return -1;
    if (turbo_script_timer_failed_count(ctx) > 0) {
        fprintf(stderr, "A timer callback failed; use timer.error(job_id) for details.\n");
        return -1;
    }
    if (turbo_script_task_failed_count(ctx) > 0) {
        fprintf(stderr, "A managed task failed; use task.error(task_id) for details.\n");
        return -1;
    }
    return script_result;
}

typedef enum {
    REPL_REQUEST_EVAL = 0,
    REPL_REQUEST_PRINT_STATS,
    REPL_REQUEST_SHUTDOWN
} repl_request_kind_t;

typedef struct repl_runtime_s repl_runtime_t;

typedef struct {
    repl_runtime_t *runtime;
    repl_request_kind_t kind;
    const char *script;
    int result;
    int done;
} repl_request_t;

struct repl_runtime_s {
    turbo_script_ctx_t *ctx;
    coro_context_t *coro_ctx;
    turbo_thread_t thread;
    turbo_mutex_t mutex;
    turbo_cond_t cond;
    atomic_int stopping;
    int loop_result;
};

static void repl_runtime_thread(void *arg) {
    repl_runtime_t *runtime = (repl_runtime_t *)arg;
    do {
        coro_context_set_persistent(runtime->coro_ctx, 1);
        runtime->loop_result = coro_context_run(runtime->coro_ctx, TURBO_RUN_DEFAULT);
    } while (runtime->loop_result >= 0 &&
             !atomic_load_explicit(&runtime->stopping, memory_order_acquire));
}

static void repl_runtime_request(void *arg1, void *arg2) {
    repl_request_t *request = (repl_request_t *)arg1;
    repl_runtime_t *runtime = request->runtime;
    (void)arg2;

    switch (request->kind) {
    case REPL_REQUEST_EVAL:
        request->result = turbo_script_run_and_print(runtime->ctx, request->script);
        if (request->result < 0)
            fprintf(stderr, "Error: %s\n", turbo_script_get_error(runtime->ctx));
        break;
    case REPL_REQUEST_PRINT_STATS:
        turbo_script_print_jit_stats(runtime->ctx, stdout);
        request->result = 0;
        break;
    case REPL_REQUEST_SHUTDOWN:
        atomic_store_explicit(&runtime->stopping, 1, memory_order_release);
        turbo_script_free(runtime->ctx);
        runtime->ctx = NULL;
        coro_context_set_persistent(runtime->coro_ctx, 0);
        request->result = 0;
        break;
    }

    turbo_mutex_lock(&runtime->mutex);
    request->done = 1;
    turbo_cond_signal(&runtime->cond);
    turbo_mutex_unlock(&runtime->mutex);
}

static int repl_runtime_submit(repl_runtime_t *runtime, repl_request_kind_t kind,
                               const char *script) {
    repl_request_t request = {runtime, kind, script, -1, 0};
    int rc;
    rc = coro_post(runtime->coro_ctx, repl_runtime_request, &request, NULL);
    if (rc != 0) return -1;

    turbo_mutex_lock(&runtime->mutex);
    while (!request.done) turbo_cond_wait(&runtime->cond, &runtime->mutex);
    turbo_mutex_unlock(&runtime->mutex);
    return request.result;
}

static int repl_runtime_start(repl_runtime_t *runtime, turbo_script_ctx_t *ctx,
                              coro_context_t *coro_ctx) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->ctx = ctx;
    runtime->coro_ctx = coro_ctx;
    atomic_init(&runtime->stopping, 0);
    turbo_mutex_init(&runtime->mutex);
    turbo_cond_init(&runtime->cond);
    coro_context_set_persistent(coro_ctx, 1);
    if (turbo_thread_create(&runtime->thread, repl_runtime_thread, runtime) != 0) {
        coro_context_set_persistent(coro_ctx, 0);
        turbo_cond_destroy(&runtime->cond);
        turbo_mutex_destroy(&runtime->mutex);
        return -1;
    }
    return 0;
}

static void repl_runtime_destroy(repl_runtime_t *runtime) {
    (void)turbo_thread_join(&runtime->thread);
    turbo_thread_destroy(&runtime->thread);
    turbo_cond_destroy(&runtime->cond);
    turbo_mutex_destroy(&runtime->mutex);
}

int main(int argc, char **argv) {
    char *eval_str = NULL;
    char *file_path = NULL;
    bool enable_jit_stats = false;

    turbo_cmd_parser_t *parser = turbo_cmd_create("TurboScript REPL", "1.0");
    turbo_cmd_add_string(parser, &eval_str, "eval", "e", "Evaluate a string of code and exit");
    turbo_cmd_add_string(parser, &file_path, "file", "f", "Run the given script file and exit");
    turbo_cmd_add_flag(parser, &enable_jit_stats, "jit-stats", "", "Enable JIT statistics collection and print at exit");
    
    turbo_cmd_parse(parser, argc, argv, true);
    turbo_cmd_destroy(parser);

    turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
    if (!ctx) {
        fprintf(stderr, "Failed to initialize TurboScript context.\n");
        return 1;
    }

    // 启用 JIT 统计（如果请求）
    if (enable_jit_stats) {
        turbo_script_enable_jit_stats(ctx, 1);
        printf("JIT statistics enabled.\n");
    }

    if (eval_str || file_path) {
        coro_context_t *coro_ctx = create_script_coro_context();
        if (!coro_ctx) {
            fprintf(stderr, "Failed to initialize the timer event loop.\n");
            turbo_script_free(ctx);
            return 1;
        }
        turbo_script_set_coro_context(ctx, coro_ctx);
        int res = eval_str ? turbo_script_run(ctx, eval_str)
                           : turbo_script_run_file(ctx, file_path);
        res = run_scheduled_callbacks(ctx, coro_ctx, res);
        if (enable_jit_stats) {
            turbo_script_print_jit_stats(ctx, stdout);
        }
        turbo_script_free(ctx);
        coro_context_destroy(coro_ctx);
        return (res < 0) ? 1 : 0;
    }

#ifdef DEBUG
    printf("TurboScript REPL (v1.0) [DEBUG MODE - Interpreter]\n");
#else
    printf("TurboScript REPL (v1.0) [JIT Mode]\n");
#endif
    printf("Type 'exit' or 'quit' to exit.\n");

    coro_context_t *repl_coro_ctx = create_script_coro_context();
    repl_runtime_t repl_runtime;
    if (!repl_coro_ctx) {
        fprintf(stderr, "Failed to initialize the REPL event loop.\n");
        turbo_script_free(ctx);
        return 1;
    }
    turbo_script_set_coro_context(ctx, repl_coro_ctx);
    if (repl_runtime_start(&repl_runtime, ctx, repl_coro_ctx) != 0) {
        fprintf(stderr, "Failed to start the REPL runtime thread.\n");
        turbo_script_free(ctx);
        coro_context_destroy(repl_coro_ctx);
        return 1;
    }

    char buffer[4096];
    while (1) {
        printf("ts> ");
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            break;
        }

        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) {
            buffer[--len] = '\0';
        }

        if (strcmp(buffer, "exit") == 0 || strcmp(buffer, "quit") == 0) {
            break;
        }

        if (len == 0) {
            continue;
        }

        (void)repl_runtime_submit(&repl_runtime, REPL_REQUEST_EVAL, buffer);
    }

    if (enable_jit_stats) {
        (void)repl_runtime_submit(&repl_runtime, REPL_REQUEST_PRINT_STATS, NULL);
    }

    if (repl_runtime_submit(&repl_runtime, REPL_REQUEST_SHUTDOWN, NULL) != 0) {
        fprintf(stderr, "Failed to stop the REPL runtime cleanly.\n");
        coro_context_stop(repl_coro_ctx);
    }
    repl_runtime_destroy(&repl_runtime);
    coro_context_destroy(repl_coro_ctx);
    return repl_runtime.loop_result < 0 ? 1 : 0;
}
