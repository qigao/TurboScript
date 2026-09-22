#include <turbo_script.h>
#include <stdio.h>

static int verify_backend(int jit) {
  turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_DEFAULT);
  if (!ctx) return 1;
  ts_bind_num(ctx, "input", 6.0);
  const char *source = "answer = input * 7;";
  int status = jit ? turbo_script_run_jit(ctx, source) : turbo_script_run(ctx, source);
  if (status != 0 || ts_get_num(ctx, "answer") != 42.0) {
    fprintf(stderr, "backend=%d status=%d error=%s\n", jit, status,
            turbo_script_get_error(ctx));
    turbo_script_free(ctx);
    return 2;
  }
  turbo_script_free(ctx);
  return 0;
}

int main(void) {
  if (verify_backend(0) || verify_backend(1)) return 1;
  puts("TURBOSCRIPT_SDK_INTERPRETER_JIT_OK");
  return 0;
}
