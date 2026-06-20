#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "turbo_parser.h"
#include "turbo_script.h"

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

    if (eval_str) {
        int res = turbo_script_run(ctx, eval_str);
        if (enable_jit_stats) {
            turbo_script_print_jit_stats(ctx, stdout);
        }
        turbo_script_free(ctx);
        return (res < 0) ? 1 : 0;
    }

    if (file_path) {
        int res = turbo_script_run_file(ctx, file_path);
        if (enable_jit_stats) {
            turbo_script_print_jit_stats(ctx, stdout);
        }
        turbo_script_free(ctx);
        return (res < 0) ? 1 : 0;
    }

#ifdef DEBUG
    printf("TurboScript REPL (v1.0) [DEBUG MODE - Interpreter]\n");
#else
    printf("TurboScript REPL (v1.0) [JIT Mode]\n");
#endif
    printf("Type 'exit' or 'quit' to exit.\n");

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

        if (turbo_script_run_and_print(ctx, buffer) < 0) {
            fprintf(stderr, "Error: %s\n", turbo_script_get_error(ctx));
        }
    }

    if (enable_jit_stats) {
        turbo_script_print_jit_stats(ctx, stdout);
    }
    
    turbo_script_free(ctx);
    return 0;
}
