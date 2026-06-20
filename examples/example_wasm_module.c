/**
 * @file example_wasm_module.c
 * @brief Minimal script-level wasm module demo for TurboScript.
 *
 * Flow:
 *   1) Init bare context
 *   2) Load wasm plugin (wasm_plugin.dll / libwasm_plugin.so)
 *   3) Write temp fib32 wasm file
 *   4) Run script: wasm.open -> wasm.call("fib", 20) -> wasm.close
 *   5) Read result from script vars
 */

#include "turbo_script.h"
#include "fib32.wasm.h"

#include <stdio.h>

static int write_fib32_file(const char *path) {
  FILE *fp = fopen(path, "wb");
  size_t wr;
  if (!fp)
    return -1;
  wr = fwrite(fib32_wasm, 1, fib32_wasm_len, fp);
  fclose(fp);
  return (wr == fib32_wasm_len) ? 0 : -1;
}

int main(void) {
  const char *wasm_file = "example_fib32.wasm";
  turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
  const char *script;
  int rc;

  if (!ctx) {
    fprintf(stderr, "turbo_script_init failed\n");
    return 1;
  }

  if (write_fib32_file(wasm_file) != 0) {
    fprintf(stderr, "failed to write %s\n", wasm_file);
    turbo_script_free(ctx);
    return 1;
  }

  rc = turbo_script_load_plugin(ctx, "wasm");
  if (rc != 0) {
    fprintf(stderr, "turbo_script_load_plugin(\"wasm\") failed: %s\n",
            turbo_script_get_error(ctx));
    remove(wasm_file);
    turbo_script_free(ctx);
    return 1;
  }

  script =
      "h = wasm.open(\"example_fib32.wasm\");"
      "result = wasm.call(h, \"fib\", 20);"
      "wasm.close(h);";

  rc = turbo_script_run(ctx, script);
  if (rc != 0) {
    fprintf(stderr, "turbo_script_run failed: %s\n", turbo_script_get_error(ctx));
    remove(wasm_file);
    turbo_script_free(ctx);
    return 1;
  }

  printf("wasm fib(20) = %.0f\n", ts_get_num(ctx, "result"));

  remove(wasm_file);
  turbo_script_free(ctx);
  return 0;
}

