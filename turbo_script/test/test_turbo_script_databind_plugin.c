#include "../src/turbo_script_internal.h"
#include "tinytest.h"

#include <string.h>

static ts_plugin_handle_t *find_plugin(
    turbo_script_ctx_t *ctx, const char *name) {
  if (!ctx || !name) return NULL;
  for (size_t i = 0u; i < ctx->plugin_count; ++i) {
    if (ctx->loaded_names[i] &&
        strcmp(ctx->loaded_names[i], name) == 0)
      return ctx->plugins[i];
  }
  return NULL;
}

static void check_decode_result(
    turbo_script_ctx_t *ctx,
    const char *name,
    int64_t expected) {
  exprtk_value_t result = exprtk_env_get(&ctx->env, name);
  exprtk_value_t pixels;

  check_true(exprtk_value_is_object_like(&result));
  check_true(exprtk_map_has(&result, "pixels"));
  pixels = exprtk_map_get(&result, "pixels");
  check_equal(pixels.type, EXPRTK_VAL_INTEGER);
  check_equal(pixels.data.integer, expected);
}

static void check_encode_result(
    turbo_script_ctx_t *ctx,
    const char *name,
    int64_t expected) {
  exprtk_value_t result = exprtk_env_get(&ctx->env, name);
  exprtk_value_t bytes;

  check_true(exprtk_value_is_object_like(&result));
  check_true(exprtk_map_has(&result, "bytes"));
  bytes = exprtk_map_get(&result, "bytes");
  check_equal(bytes.type, EXPRTK_VAL_INTEGER);
  check_equal(bytes.data.integer, expected);
}

spec("TurboScript generated DataBind Service plugin") {
  it("imports a canonical generated catalog and calls it in interpreter and JIT") {
    const char *script =
        "import(\"Image.ImageProcessor\");"
        "decoded=Image.Codec.Decode(map {width:12});"
        "encoded=Image.Codec.Encode(map {pixels:48});";
    turbo_script_ctx_t *interp =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    turbo_script_ctx_t *jit =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);
    ts_plugin_handle_t *interp_plugin;
    ts_plugin_handle_t *jit_plugin;

    check_not_null(interp);
    check_not_null(jit);
    check_equal(turbo_script_run(interp, script), 0);
    check_equal(turbo_script_run_jit(jit, script), 0);

    check_decode_result(interp, "decoded", INT64_C(48));
    check_decode_result(jit, "decoded", INT64_C(48));
    check_encode_result(interp, "encoded", INT64_C(12));
    check_encode_result(jit, "encoded", INT64_C(12));

    interp_plugin = find_plugin(interp, "Image.ImageProcessor");
    jit_plugin = find_plugin(jit, "Image.ImageProcessor");
    check_not_null(interp_plugin);
    check_not_null(jit_plugin);
    if (interp_plugin) {
      check_equal(interp_plugin->databind_binding_count, (size_t)2u);
      check_equal(interp_plugin->function_binding_count, (size_t)0u);
    }
    if (jit_plugin) {
      check_equal(jit_plugin->databind_binding_count, (size_t)2u);
      check_equal(jit_plugin->function_binding_count, (size_t)0u);
    }

    turbo_script_free(jit);
    turbo_script_free(interp);
  }

  it("surfaces DataBind required-field diagnostics") {
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(turbo_script_run(
              ctx,
              "import(\"Image.ImageProcessor\");"
              "bad=Image.Codec.Decode(map {});") != 0);
    check_not_null(strstr(turbo_script_get_error(ctx), "width"));

    turbo_script_free(ctx);
  }

  it("rejects unsafe or empty dotted plugin-name segments") {
    turbo_script_ctx_t *ctx =
        turbo_script_init(TURBO_SCRIPT_INIT_BARE);

    check_not_null(ctx);
    check(turbo_script_load_plugin(ctx, "Image..ImageProcessor") != 0);
    check(turbo_script_load_plugin(ctx, ".ImageProcessor") != 0);
    check(turbo_script_load_plugin(ctx, "Image.ImageProcessor.") != 0);
    check(turbo_script_load_plugin(ctx, "../Image.ImageProcessor") != 0);

    turbo_script_free(ctx);
  }
}
