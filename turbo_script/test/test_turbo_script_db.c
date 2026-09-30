#include "turbo_script.h"
#include "tinytest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH
#define TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH ""
#endif

static int append_quoted_path(char *out, size_t capacity, const char *path) {
  size_t used = 0u;
  if (!out || capacity == 0u || !path) return 0;

  for (const char *p = path; *p; ++p) {
    if (*p == '\\' || *p == '"') {
      if (used + 2u >= capacity) return 0;
      out[used++] = '\\';
      out[used++] = *p;
    } else {
      if (used + 1u >= capacity) return 0;
      out[used++] = *p;
    }
  }
  if (used >= capacity) return 0;
  out[used] = '\0';
  return 1;
}

static int build_success_script(char *out, size_t capacity) {
  char path[2048];
  if (!append_quoted_path(
          path, sizeof(path), TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH))
    return 0;
  return snprintf(
             out, capacity,
             "a=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
             "options:map {filename:\":memory:\"}});"
             "b=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
             "options:map {filename:\":memory:\"}});"
             "ca=db.close(a);"
             "cb=db.close(b);",
             path, path) > 0;
}

static int build_failure_script(char *out, size_t capacity,
                                const char *driver,
                                const char *module_path,
                                const char *options_source) {
  char path[2048];
  if (!append_quoted_path(path, sizeof(path), module_path)) return 0;
  return snprintf(
             out, capacity,
             "x=db.connect(map {driver:\"%s\",module_path:\"%s\"%s});",
             driver, path, options_source ? options_source : "") > 0;
}

static turbo_script_ctx_t *new_db_ctx(void) {
  turbo_script_ctx_t *ctx = turbo_script_init(TURBO_SCRIPT_INIT_BARE);
  if (!ctx) return NULL;
  if (turbo_script_load_plugin(ctx, "db") != 0) {
    turbo_script_free(ctx);
    return NULL;
  }
  return ctx;
}

spec("TurboScript TurboDB ORM configuration") {
  it("uses explicit user config for two connections on one loaded driver") {
    char script[8192];
    turbo_script_ctx_t *interp = new_db_ctx();
    turbo_script_ctx_t *jit = new_db_ctx();

    check_not_null(interp);
    check_not_null(jit);
    check_true(build_success_script(script, sizeof(script)));

    check_equal(turbo_script_run(interp, script), 0);
    check_equal(turbo_script_run_jit(jit, script), 0);

    check(fabs(ts_get_num(interp, "a") - 1.0) <= 1e-9);
    check(fabs(ts_get_num(interp, "b") - 2.0) <= 1e-9);
    check(fabs(ts_get_num(jit, "a") - 1.0) <= 1e-9);
    check(fabs(ts_get_num(jit, "b") - 2.0) <= 1e-9);
    check(fabs(ts_get_num(interp, "ca") - 1.0) <= 1e-9);
    check(fabs(ts_get_num(interp, "cb") - 1.0) <= 1e-9);
    check(fabs(ts_get_num(jit, "ca") - 1.0) <= 1e-9);
    check(fabs(ts_get_num(jit, "cb") - 1.0) <= 1e-9);

    turbo_script_free(jit);
    turbo_script_free(interp);
  }

  it("fails when the explicit driver ID does not match the module manifest") {
    char script[4096];
    turbo_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(build_failure_script(
        script, sizeof(script), "postgresql",
        TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH,
        ",options:map {filename:\":memory:\"}"));

    check(turbo_script_run(ctx, script) != 0);
    check_not_null(strstr(turbo_script_get_error(ctx), "driver"));

    turbo_script_free(ctx);
  }

  it("does not invent a default module path") {
    turbo_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check(turbo_script_run(
              ctx,
              "x=db.connect(map {driver:\"sqlite\","
              "options:map {filename:\":memory:\"}});") != 0);
    check_not_null(strstr(turbo_script_get_error(ctx), "module_path"));

    turbo_script_free(ctx);
  }

  it("passes backend-specific options through to TurboDB and fails fast") {
    char script[4096];
    turbo_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(build_failure_script(
        script, sizeof(script), "sqlite",
        TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH,
        ",options:map {}"));

    check(turbo_script_run(ctx, script) != 0);
    check_not_null(strstr(turbo_script_get_error(ctx), "filename"));

    turbo_script_free(ctx);
  }

  it("rejects changing module_path for an already loaded driver ID") {
    char path[2048];
    char script[8192];
    turbo_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(append_quoted_path(
        path, sizeof(path), TURBOSCRIPT_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "a=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "b=db.connect(map {driver:\"sqlite\","
              "module_path:\"/definitely/different/driver\","
              "options:map {filename:\":memory:\"}});",
              path) > 0);

    check(turbo_script_run(ctx, script) != 0);
    check_not_null(strstr(
        turbo_script_get_error(ctx), "different module_path"));

    turbo_script_free(ctx);
  }
}
