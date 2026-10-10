#include "tinytest.h" /* TurboScript host plugin-loader regression. */
#include "ts_plugin_loader.h"
#include <string.h>

#if defined(_WIN32)
#define LOADER_FIXTURE_FILE "loader_fixture.dll"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.dll"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.dll"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.dll"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.dll"
#define LOADER_LEGACY_FILE "loader_legacy.dll"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.dll"
#define LOADER_PACKAGE_FILE "loader_package.dll"
#elif defined(__APPLE__)
#define LOADER_FIXTURE_FILE "loader_fixture.dylib"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.dylib"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.dylib"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.dylib"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.dylib"
#define LOADER_LEGACY_FILE "loader_legacy.dylib"
#define LOADER_CWD_ONLY_FILE "loader_cwd_only.dylib"
#define LOADER_PACKAGE_FILE "loader_package.dylib"
#else
#define LOADER_FIXTURE_FILE "loader_fixture.so"
#define LOADER_BAD_ABI_FILE "loader_bad_abi.so"
#define LOADER_WRONG_NAME_FILE "loader_wrong_name.so"
#define LOADER_MISSING_ENTRY_FILE "loader_missing_entry.so"
#define LOADER_INIT_FAILURE_FILE "loader_init_failure.so"
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
        check_not_null(handle->open_export);
        check_equal(handle->name, "loader_fixture");
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
        check_not_null(handle->open_export);
        check_equal(handle->name, "loader_fixture");
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
      check_not_null(strstr(error.message, "Salts Plugin"));
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
      check_not_null(strstr(error.message, CMETA_PLUGIN_QUERY_SYMBOL));
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

  describe("Salts admission and lease lifetime") {
    static ts_plugin_handle_t *handle;
    static int attempts;
    before_each() { handle = NULL; attempts = 0; }
    after_each() {
      if (handle) {
        /* Allow cleanup even after a fatal assertion in the retry test. */
        attempts = 1;
        ts_plugin_unload(handle);
      }
    }

    it("rejects a different module contract before initializing") {
      check_equal(ts_plugin_load_ex(LOADER_BAD_CONTRACT_FILE, NULL, &handle, NULL),
                  TS_PLUGIN_ERROR_CONTRACT);
      check_null(handle);
    }

    it("rejects a correct contract ID with a different native signature") {
      check_equal(ts_plugin_load_ex(LOADER_BAD_SIGNATURE_FILE, NULL, &handle, NULL),
                  TS_PLUGIN_ERROR_CONTRACT);
      check_null(handle);
    }

    it("retains the instance and lease after failed close and permits retry") {
      cmeta_plugin_lifecycle_info info;
      cmeta_plugin_lease extra = {0};
      const cmeta_plugin_manifest *manifest = NULL;
      check_equal(ts_plugin_load_ex(LOADER_CLOSE_FAILURE_FILE, NULL, &handle, NULL), 0);
      check_equal(ts_plugin_init(handle, &attempts, &attempts), 0);
      check_equal(cmeta_plugin_registry_get_lifecycle(&handle->registry, handle->ref, &info),
                  CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)1);
      check_equal(cmeta_plugin_registry_unload(&handle->registry, handle->ref), CMETA_PLUGIN_BUSY);
      check_equal(ts_plugin_unload_ex(handle, NULL), TS_PLUGIN_ERROR_CLOSE);
      check_equal(attempts, 1);
      check(handle->instance == &attempts);
      check(cmeta_plugin_lease_valid(handle->lease));
      check_equal(cmeta_plugin_registry_get_lifecycle(&handle->registry, handle->ref, &info),
                  CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)1);
      check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_STOPPING);
      check_equal(cmeta_plugin_registry_acquire(&handle->registry, handle->ref, &extra, &manifest),
                  CMETA_PLUGIN_INVALID_STATE);
      check_equal(ts_plugin_init(handle, &attempts, &attempts), -1);
      check_equal(ts_plugin_unload_ex(handle, NULL), 0);
      handle = NULL;
      check_equal(attempts, 2);
    }
  }
}
