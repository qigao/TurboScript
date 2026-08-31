# Cross-Platform Plugin Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load TurboScript native modules deterministically from a `plugins` directory beside the executable on Windows, Linux, and macOS, with actionable errors and compatible lifecycle behavior.

**Architecture:** Keep `ts_api_create` and the existing `ts_plugin_load()` entry point compatible. Split executable-relative discovery from exact native-library loading inside `ts_plugin_loader.c`; load only absolute candidates, validate the descriptor before initialization, and preserve executable-directory fallback for existing deployments.

**Tech Stack:** C11, Win32 loader APIs, POSIX `dlopen`, CMake Presets, CTest, TinyTest.

**Spec:** Conversation design approved on 2026-08-31.

## Global Constraints

- Preserve the existing `ts_api_create()` ABI and `turbo_script_load_plugin(ctx, name)` API.
- Search `<exe>/plugins` first and `<exe>` second; do not load a bare name through the OS search path.
- Windows uses UTF-16 `LoadLibraryExW`; POSIX uses `RTLD_NOW | RTLD_LOCAL`.
- The TurboScript context owns plugin handles; plugin cleanup precedes native-library close.
- Existing unrelated working-tree changes must remain untouched.
- Execute inline on `main` as explicitly authorized; do not create commits unless requested.

---

### Task 1: Executable-relative discovery regression

**Files:**
- Create: `exprtk/test/plugin_loader_fixture.c`
- Create: `exprtk/test/test_ts_plugin_loader.c`
- Modify: `exprtk/test/CMakeLists.txt`

**Interfaces:**
- Consumes: `ts_plugin_load(const char *)`, `ts_plugin_init(...)`, `ts_plugin_unload(...)`.
- Produces: a real shared-library fixture placed in `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins` and a CTest regression named `test_ts_plugin_loader`.

- [x] Add a fixture exporting `ts_api_create()` with name `loader_fixture`, ABI version `1`, and a load callback returning the supplied environment.
- [x] Add a TinyTest case that calls `ts_plugin_load("loader_fixture<platform suffix>")` while the fixture exists only under `plugins/` beside the test executable.
- [x] Reconfigure and build `test_ts_plugin_loader`; run it and verify failure because the current loader does not search `exe/plugins`.
- [x] Implement the minimum deterministic locator/native-open change in `ts_plugin_loader.c`.
- [x] Rebuild and verify the focused test passes.

### Task 2: Structured validation and error propagation

**Files:**
- Modify: `exprtk/include/ts_plugin.h`
- Modify: `exprtk/include/ts_plugin_loader.h`
- Modify: `exprtk/src/ts_plugin_loader.c`
- Modify: `turbo_script/src/turbo_script.c`
- Extend: `exprtk/test/plugin_loader_fixture.c`
- Extend: `exprtk/test/test_ts_plugin_loader.c`

**Interfaces:**
- Produces `TS_PLUGIN_ABI_VERSION`, `ts_plugin_error_code_t`, `ts_plugin_error_stage_t`, `ts_plugin_error_t`, and:

```c
int ts_plugin_load_ex(const char *path, const char *expected_name,
                      ts_plugin_handle_t **out, ts_plugin_error_t *error);
int ts_plugin_init_ex(ts_plugin_handle_t *handle, void *env, void *scratch,
                      ts_plugin_error_t *error);
```

- [x] Add failing tests for wrong ABI, requested-name mismatch, missing entry point, and initialization failure.
- [x] Run the focused test and confirm each new behavior fails for the intended reason.
- [x] Add structured errors and exact descriptor validation; keep `ts_plugin_load()` and `ts_plugin_init()` as compatible wrappers.
- [x] Route `turbo_script_load_plugin()` through the extended API and include the failure stage/detail in its context error.
- [x] Ensure failed initialization does not invoke `unload(NULL)` and all opened libraries close on failure.
- [x] Rebuild and run the focused loader test plus TurboScript plugin-authorization tests.

### Task 3: Packaging, compatibility, and verification

**Files:**
- Modify: `cmake/CmakeUtils.cmake`
- Modify: `CMakeLists.txt`
- Modify: `turbo_script/app/CMakeLists.txt`
- Modify: `docs/PLUGIN_SYSTEM.md`
- Modify: `docs/zh/plugin-development.md`

**Interfaces:**
- Plugin targets created with `cmake_config_target(... PLUGIN)` emit into the build `bin/plugins` directory.
- Installed plugins use `${CMAKE_INSTALL_BINDIR}/plugins` on every supported platform.

- [x] Update plugin target output and install destinations, removing obsolete executable-directory copies from the CLI target.
- [x] Update documentation to state the actual deterministic order: executable `plugins/`, executable directory compatibility fallback, explicit path.
- [x] Reconfigure with `win-release-user`, build the focused targets, and run the loader/CLI plugin CTest filters.
- [x] Run the complete `win-release-user` build and test preset when focused verification passes.
- [x] Review `git diff` and `git status` to confirm unrelated pre-existing edits were preserved.

## Self-Review

- Spec coverage: discovery, Windows/POSIX backends, ABI validation, error reporting, lifecycle, packaging, compatibility, and tests are each assigned.
- Placeholder scan: no deferred implementation placeholders are present.
- Type consistency: Task 2 defines the extended APIs consumed by TurboScript; legacy wrappers remain unchanged.
