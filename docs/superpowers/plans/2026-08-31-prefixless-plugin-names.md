# Prefixless Plugin Names Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and discover TurboScript native plugins as `<name>.dll`, `<name>.so`, or `<name>.dylib` without the `tbs_` filename prefix.

**Architecture:** Keep logical plugin names, the `ts_api_create` C ABI, descriptor validation, lifecycle ownership, and executable-relative directories unchanged. Make the prefixless filename canonical while retaining `tbs_<name>` as a load-only compatibility fallback for existing deployments.

**Tech Stack:** C11, CMake, TinyTest, CTest, Win32 `LoadLibraryExW`, POSIX `dlopen`.

**Spec:** User request in this conversation on 2026-08-31: remove the `tbs_` prefix from `tbs_parser.dll` and the common plugin naming convention.

## Global Constraints

- Apply the naming rule uniformly to every `cmake_config_target(... PLUGIN)` module.
- Preserve plugin ABI version 1, descriptor names, entry point `ts_api_create`, and host APIs.
- Prefer `<name><suffix>`; try `tbs_<name><suffix>` only after an open failure.
- Use prefixless collision-safe stems for `crypto` and `rules_forge`, whose
  dependency libraries already own the logical-name filenames.
- Keep search directories `<exe>/plugins`, then `<exe>`; never search CWD or `PATH`.
- Work inline on the existing `main` worktree and preserve unrelated dirty changes.

---

### Task 1: Prefixless discovery contract

**Files:**
- Modify: `exprtk/test/CMakeLists.txt`
- Modify: `exprtk/test/test_ts_plugin_loader.c`
- Modify: `turbo_script/test/CMakeLists.txt`
- Modify: `turbo_script/test/test_turbo_script_basics.c`

**Interfaces:**
- Produces a valid `plugins/loader_fixture<suffix>` and a conflicting legacy `plugins/tbs_loader_fixture<suffix>` whose descriptor name is deliberately wrong.
- Verifies `turbo_script_load_plugin(ctx, "loader_fixture")` selects the prefixless library.

- [x] Change the primary fixture filename to `loader_fixture<suffix>` and add the conflicting legacy fixture.
- [x] Add a TinyTest case that loads logical name `loader_fixture` through the public TurboScript API.
- [x] Build and run the focused test; confirm it fails at descriptor-name validation because the current resolver tries `tbs_` first.

### Task 2: Runtime naming order and module outputs

**Files:**
- Modify: `turbo_script/src/turbo_script.c`
- Modify: plugin `CMakeLists.txt` files under `modules/`

**Interfaces:**
- `ts_plugin_file_name(..., use_legacy_prefix)` emits the canonical bare name when false and the compatibility name when true.
- Every plugin target emits a prefixless filename under `bin/plugins`; `crypto`
  and `rules_forge` use `_plugin` stems to avoid dependency collisions.

- [x] Reverse the resolver order so the prefixless name is attempted first and only open failures reach the legacy-prefixed path.
- [x] Remove `tbs_` from every plugin target `OUTPUT_NAME`, including optional WASM.
- [x] Reconfigure, build focused targets, and confirm loader and TurboScript tests pass.

### Task 3: Documentation, installation, and regression verification

**Files:**
- Modify: `docs/PLUGIN_SYSTEM.md`
- Modify: `docs/zh/plugin-development.md`
- Modify: module-specific documentation and filename-based integration tests.

**Interfaces:**
- User-facing examples use `parser.dll`, `mapper.dll`, and `<name>.<suffix>`.
- Installed libraries remain under `${CMAKE_INSTALL_BINDIR}/plugins`.

- [x] Update filename constants in DLL integration tests and plugin documentation.
- [x] Scan tracked source and documentation for obsolete canonical `tbs_` names; retain only explicit compatibility text.
- [x] Run the complete Windows release build and CTest preset.
- [x] Stage an install and verify `bin/plugins/parser.dll` exists while no installed plugin uses `tbs_`.
- [x] Review `git diff --check` and preserve pre-existing worktree changes.

## Self-Review

- Spec coverage: build names, lookup priority, legacy compatibility, tests, documentation, and install layout are covered.
- Placeholder scan: the plan contains no deferred implementation placeholders.
- Type consistency: no public C type or function signature changes.
