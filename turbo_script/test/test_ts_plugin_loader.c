#include "tinytest.h" /* TurboScript host plugin-loader regression. */
#include "ts_plugin_loader.h"
#include "exprtk.h"
#include <math.h>
#include <string.h>

#if defined(_WIN32)
#define LOADER_FIXTURE_FILE "loader_fixture.dll"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.dll"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.dll"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.dll"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.dll"
#define LOADER_FUNCTION_FILE "loader_function.dll"
#define LOADER_LEGACY_FILE "loader_legacy.dll"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.dll"
#define LOADER_PACKAGE_FILE "loader_package.dll"
#elif defined(__APPLE__)
#define LOADER_FIXTURE_FILE "loader_fixture.dylib"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.dylib"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.dylib"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.dylib"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.dylib"
#define LOADER_FUNCTION_FILE "loader_function.dylib"
#define LOADER_LEGACY_FILE "loader_legacy.dylib"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.dylib"
#define LOADER_PACKAGE_FILE "loader_package.dylib"
#else
#define LOADER_FIXTURE_FILE "loader_fixture.so"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.so"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.so"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.so"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.so"
#define LOADER_FUNCTION_FILE "loader_function.so"
#define LOADER_LEGACY_FILE "loader_legacy.so"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.so"
#define LOADER_PACKAGE_FILE "loader_package.so"
#endif

spec("plugin loader") {
  describe("executable-relative discovery") {
    it("loads a plugin from the plugins directory beside the executable") {
      ts_plugin_handle_t *handle = ts_plugin_load(LOADER_FIXTURE_FILE);

      check_not_null(handle);
      if (handle) {
        check_not_null(handle->manifest);
        check_equal(handle->manifest->plugin_id, "loader_fixture");
        ts_plugin_unload(handle);
      }
    }

    it("keeps the executable directory as a compatibility fallback") {
      ts_plugin_handle_t *handle = ts_plugin_load(LOADER_LEGACY_FILE);

      check_not_null(handle);
      if (handle) ts_plugin_unload(handle);
    }

    it("loads a plugin from the configured TurboScript package") {
      ts_plugin_handle_t *handle = ts_plugin_load(LOADER_PACKAGE_FILE);

      check_not_null(handle);
      if (handle) {
        check_not_null(handle->manifest);
        check_equal(handle->manifest->plugin_id, "loader_fixture");
        ts_plugin_unload(handle);
      }
    }

    it("does not search the current working directory") {
      ts_plugin_error_t error = {0};
      ts_plugin_handle_t *handle = NULL;

      check_equal(ts_plugin_load_ex(LOADER_CWD_ONLY_FILE, NULL, &handle, &error),
                  TS_PLUGIN_ERROR_OPEN);
      check_null(handle);
      check_equal(error.stage, TS_PLUGIN_STAGE_OPEN);
    }
  }

  describe("descriptor validation") {
    it("rejects a plugin built for a different ABI") {
      ts_plugin_error_t error = {0};
      ts_plugin_handle_t *handle = NULL;

      check_equal(ts_plugin_load_ex(LOADER_BAD_ABI_FILE, "loader_bad_abi", &handle, &error),
                  TS_PLUGIN_ERROR_ABI);
      check_null(handle);
      check_equal(error.stage, TS_PLUGIN_STAGE_ABI);
      check_not_null(strstr(error.message, "ABI"));
    }

    it("rejects a descriptor whose name differs from the requested plugin") {
      ts_plugin_error_t error = {0};
      ts_plugin_handle_t *handle = NULL;

      check_equal(ts_plugin_load_ex(LOADER_WRONG_NAME_FILE, "loader_wrong_name", &handle, &error),
                  TS_PLUGIN_ERROR_NAME);
      check_null(handle);
      check_equal(error.stage, TS_PLUGIN_STAGE_ABI);
      check_not_null(strstr(error.message, "name"));
    }

    it("reports a missing plugin entry point separately from open failures") {
      ts_plugin_error_t error = {0};
      ts_plugin_handle_t *handle = NULL;

      check_equal(ts_plugin_load_ex(LOADER_MISSING_ENTRY_FILE, NULL, &handle, &error),
                  TS_PLUGIN_ERROR_SYMBOL);
      check_null(handle);
      check_equal(error.stage, TS_PLUGIN_STAGE_SYMBOL);
      check_not_null(strstr(error.message, "salts_plugin_query"));
    }
  }

  describe("canonical Function exports") {
    it("loads and binds a Function-only plugin without a module adapter") {
      ts_plugin_error_t error = {0};
      ts_plugin_handle_t *handle = NULL;
      exprtk_env_t env;
      exprtk_value_t arg;
      exprtk_value_t result;

      exprtk_env_init(&env);
      check_equal(
          ts_plugin_load_ex(
              LOADER_FUNCTION_FILE, "loader_function", &handle, &error),
          TS_PLUGIN_ERROR_NONE);
      check_not_null(handle);
      if (handle) {
        check_null(handle->module);
        check_equal(handle->manifest->export_count, (size_t)6u);
        check_equal(handle->manifest->exports[0].kind,
                    SALTS_PLUGIN_EXPORT_FUNCTION);
        check_equal(
            ts_plugin_init_ex(handle, &env, NULL, &error),
            TS_PLUGIN_ERROR_NONE);
        check_equal(handle->function_binding_count, (size_t)6u);
        check_true(exprtk_env_has_func(&env, "loader_function.double"));
        check_true(exprtk_env_has_func(&env, "loader_function.increment"));
        check_true(exprtk_env_has_func(&env, "loader_function.positive"));
        check_true(exprtk_env_has_func(&env, "loader_function.long_to_double"));
        check_true(exprtk_env_has_func(&env, "loader_function.float_to_double"));
        check_true(exprtk_env_has_func(&env, "loader_function.add"));

        arg = exprtk_val_num(3.5);
        result = exprtk_call_internal(
            "loader_function.double", 1u, &arg, &env);
        check_equal(result.type, EXPRTK_VAL_NUMBER);
        check(fabs(result.data.number - 7.0) <= 1e-9);
        exprtk_value_destroy(&result);

        arg = exprtk_val_num(4.0);
        result = exprtk_call_internal(
            "loader_function.increment", 1u, &arg, &env);
        check_equal(result.type, EXPRTK_VAL_INTEGER);
        check_equal(result.data.integer, (int64_t)5);
        exprtk_value_destroy(&result);

        arg = exprtk_val_num(-2.0);
        result = exprtk_call_internal(
            "loader_function.positive", 1u, &arg, &env);
        check_equal(result.type, EXPRTK_VAL_BOOL);
        check_false(result.data.boolean);
        exprtk_value_destroy(&result);

        arg = exprtk_val_num(7.0);
        result = exprtk_call_internal(
            "loader_function.long_to_double", 1u, &arg, &env);
        check_equal(result.type, EXPRTK_VAL_NUMBER);
        check(fabs(result.data.number - 7.5) <= 1e-9);
        exprtk_value_destroy(&result);

        arg = exprtk_val_num(1.5);
        result = exprtk_call_internal(
            "loader_function.float_to_double", 1u, &arg, &env);
        check_equal(result.type, EXPRTK_VAL_NUMBER);
        check(fabs(result.data.number - 1.75) <= 1e-6);
        exprtk_value_destroy(&result);

        {
          exprtk_value_t args[2] = {
              exprtk_val_num(1.25), exprtk_val_num(2.5)};
          result = exprtk_call_internal(
              "loader_function.add", 2u, args, &env);
          check_equal(result.type, EXPRTK_VAL_NUMBER);
          check(fabs(result.data.number - 3.75) <= 1e-9);
          exprtk_value_destroy(&result);
        }

        check_false(env.aborted);

        arg = exprtk_val_num(1.5);
        result = exprtk_call_internal(
            "loader_function.increment", 1u, &arg, &env);
        check_true(env.aborted);
        check_not_null(strstr(env.error_msg, "argument 0"));
        exprtk_value_destroy(&result);
      }

      exprtk_env_free(&env);
      if (handle) ts_plugin_unload(handle);
    }
  }

  describe("initialization cleanup") {
    it("does not call plugin unload when initialization returned no instance") {
      int failed_init_unload_count = 0;
      ts_plugin_error_t error = {0};
      ts_plugin_handle_t *handle = NULL;

      check_equal(ts_plugin_load_ex(LOADER_INIT_FAILURE_FILE, "loader_init_failure", &handle,
                                    &error), TS_PLUGIN_ERROR_NONE);
      check_not_null(handle);
      if (handle) {
        check_equal(ts_plugin_init_ex(handle, (void *)1, &failed_init_unload_count, &error),
                    TS_PLUGIN_ERROR_INITIALIZE);
        check_equal(error.stage, TS_PLUGIN_STAGE_INITIALIZE);
        ts_plugin_unload(handle);
      }
      check_equal(failed_init_unload_count, 0);
    }
  }
}
