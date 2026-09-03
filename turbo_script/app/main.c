#include "turbo_script.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  const char *eval = NULL, *file = NULL;
  turbo_script_ctx_t *ctx;
  int rc;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--eval") && i + 1 < argc) eval = argv[++i];
    else if ((!strcmp(argv[i], "--file") || !strcmp(argv[i], "-f")) && i + 1 < argc) file = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { puts("Usage: turbo_script_repl [--eval SCRIPT | --file PATH]"); return 0; }
    else { fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]); return 2; }
  }
  if ((eval && file) || (!eval && !file)) { fputs("Specify exactly one of --eval or --file.\n", stderr); return 2; }
  ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  if (!ctx) { fputs("Failed to initialize TurboScript.\n", stderr); return 1; }
  rc = eval ? turbo_script_run(ctx, eval) : turbo_script_run_file(ctx, file);
  if (rc < 0) fprintf(stderr, "TurboScript: %s\n", turbo_script_get_error(ctx));
  turbo_script_free(ctx);
  return rc < 0 ? 1 : 0;
}
