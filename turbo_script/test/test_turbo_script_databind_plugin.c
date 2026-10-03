#include "../src/turbo_script_internal.h"
#include "exprtk.h"
#include "tinytest.h"

#include <string.h>

static int map_integer(
    const exprtk_value_t *map, const char *name, int64_t expected) {
  exprtk_value_t value;
  if (!exprtk_value_is_object_like(map) ||
      !exprtk_map_has(map, name))
    return 0;
  value = exprtk_map_get(map, name);
  return value.type == EXPRTK_VAL_INTEGER &&
         value.data.integer == expected;
}

static int map_string(
    const exprtk_value_t *map, const char *name, const char *expected) {
  exprtk_value_t value;
  size_t length;
  if (!expected || !exprtk_value_is_object_like(map) ||
      !exprtk_map_has(map, name))
    return 0;
  value = exprtk_map_get(map, name);
  length = strlen(expected);
  return value.type == EXPRTK_VAL_STRING &&
         value.data.string.len == length &&
         memcmp(value.data.string.data, expected, length) == 0;
}

spec("TurboScript generated DataBind Service Plugin binding") {
  it("binds a generated Service with defaults in interpreter and JIT") {
    const char *source =
        "import(\"Image.ImageProcessor\");"
        "result=Image.Codec.Decode(map {width:12});";
    turbo_script_ctx_t *interp =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    turbo_script_ctx_t *jit =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_value_t interp_result;
    exprtk_value_t jit_result;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(turbo_script_run(interp, source), 0);
    check_equal(turbo_script_run_jit(jit, source), 0);

    interp_result = exprtk_env_get(&interp->env, "result");
    jit_result = exprtk_env_get(&jit->env, "result");
    check_true(map_integer(&interp_result, "pixels", 48));
    check_true(map_integer(&jit_result, "pixels", 48));

    turbo_script_free(jit);
    turbo_script_free(interp);
  }

  it("runs DataBind validation before generated Service invocation") {
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(
        turbo_script_run(
            ctx,
            "import(\"Image.ImageProcessor\");"
            "result=Image.Codec.Decode(map {width:0});") != 0);
    check_not_null(strstr(
        turbo_script_get_error(ctx),
        "DataBind Service input binding failed"));

    turbo_script_free(ctx);
  }

  it("returns generated typed Service errors as structured data") {
    const char *source =
        "import(\"ErrorPlugin.StorePlugin\");"
        "result=ErrorPlugin.Store.Read(map {id:7});";
    turbo_script_ctx_t *interp =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    turbo_script_ctx_t *jit =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_value_t interp_result;
    exprtk_value_t jit_result;
    exprtk_value_t interp_error;
    exprtk_value_t jit_error;
    exprtk_value_t interp_payload;
    exprtk_value_t jit_payload;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(turbo_script_run(interp, source), 0);
    check_equal(turbo_script_run_jit(jit, source), 0);

    interp_result = exprtk_env_get(&interp->env, "result");
    jit_result = exprtk_env_get(&jit->env, "result");
    check_true(exprtk_map_has(&interp_result, "error"));
    check_true(exprtk_map_has(&jit_result, "error"));

    interp_error = exprtk_map_get(&interp_result, "error");
    jit_error = exprtk_map_get(&jit_result, "error");
    check_true(map_string(&interp_error, "type", "NotFound"));
    check_true(map_string(&jit_error, "type", "NotFound"));

    interp_payload = exprtk_map_get(&interp_error, "payload");
    jit_payload = exprtk_map_get(&jit_error, "payload");
    check_true(map_integer(&interp_payload, "id", 7));
    check_true(map_integer(&jit_payload, "id", 7));

    turbo_script_free(jit);
    turbo_script_free(interp);
  }

  it("keeps native business status distinct from typed Service errors") {
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(
        turbo_script_run(
            ctx,
            "import(\"ErrorPlugin.StorePlugin\");"
            "result=ErrorPlugin.Store.Read(map {id:99});") != 0);
    check_not_null(strstr(
        turbo_script_get_error(ctx),
        "returned native status -9"));

    turbo_script_free(ctx);
  }

  it("returns generated Service success through the same BindingPlan") {
    const char *source =
        "import(\"ErrorPlugin.StorePlugin\");"
        "result=ErrorPlugin.Store.Read(map {id:3});";
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    exprtk_value_t result;

    check_not_null(ctx);
    check_equal(turbo_script_run(ctx, source), 0);
    result = exprtk_env_get(&ctx->env, "result");
    check_true(map_integer(&result, "value", 30));
    check_false(exprtk_map_has(&result, "error"));

    turbo_script_free(ctx);
  }
}
