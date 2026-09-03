# ExprTk Layering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split ExprTk into static Syntax and Core libraries while keeping JIT and plugin ownership in TurboScript.

**Architecture:** `ExprTk::Syntax` owns only lexer, parser and AST lifetime. `ExprTk::Core` owns values, environments and interpreter services and links Syntax. TurboScript owns dynamic plugin loading and the existing MIR/JIT backend, and links Core.

**Tech Stack:** C11, re2c, Lemon, CMake/Ninja, TinyTest, MIR.

**Spec:** `docs/architecture/exprtk-layering.md`

## Global Constraints

- All new libraries are static.
- Preserve existing public `exprtk_*` source API names.
- Syntax must not depend on Core or TurboScript.
- Core must not depend on TurboScript or MIR JIT APIs.
- Configure, build and test only through `win-dev-user` presets under `VsDevCmd.bat`.

---

### Task 1: Establish the static Syntax target

**Files:**
- Modify: `exprtk/CMakeLists.txt`
- Modify: `exprtk/test/CMakeLists.txt`
- Create: `exprtk/test/test_exprtk_syntax.c`

**Interfaces:**
- Produces: `ExprTk::Syntax`, containing `exprtk_parse`, `exprtk_parse_ext`, `exprtk_free`, lexer and generated Lemon parser symbols.
- Consumes: Salts Core allocation support and `Salts::DataBind`, because public AST values expose `turbo_datetime_t` from its installed headers.

- [x] **Step 1: Add a syntax-only parser test target**

```cmake
cmake_add_test(
  TARGET test_exprtk_syntax
  SOURCES test_exprtk_syntax.c
  LIBS exprtk_syntax Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../parser)
```

- [x] **Step 2: Verify the target initially fails to configure**

Run: `cmake --preset win-dev-user`

Expected: CMake reports that `exprtk_syntax` is unknown.

- [x] **Step 3: Move lexer/parser/AST sources into `exprtk_syntax`**

```cmake
add_library(exprtk_syntax STATIC
  src/grammar/exprtk_parser.c
  ${exprtk_LEXER_GEN}
  ${exprtk_GRAMMAR_GEN})
cmake_config_target(exprtk_syntax ALIAS TurboScript::ExprTkSyntax
                    FOLDER exprtk EXPORT_NAME ExprTkSyntax)
```

Keep AST construction and destruction in this target; do not move evaluator or environment code.

- [x] **Step 4: Build the syntax target and run its test**

Run: `cmake --build --preset win-dev-user --target exprtk_syntax test_exprtk_syntax --parallel 4`

Expected: both targets link and `ctest --test-dir build/Msvc -R ^test_exprtk_syntax$ --output-on-failure` passes.

### Task 2: Make ExprTk Core depend on Syntax

**Files:**
- Modify: `exprtk/CMakeLists.txt`
- Modify: `exprtk/test/CMakeLists.txt`
- Modify: `exprtk/src/exprtk_runtime.c`
- Modify: `exprtk/src/exprtk_class.c`
- Modify: `exprtk/src/exprtk_internal.h`

**Interfaces:**
- Consumes: `ExprTk::Syntax` AST and parser API.
- Produces: `ExprTk::Core` through existing `exprtk` target and all current interpreter APIs.

- [x] **Step 1: Link existing core tests to the explicit Core target**

```cmake
set(EXPRTK_TEST_LIBS exprtk Salts::TinyTest)
target_link_libraries(test_exprtk PRIVATE TurboScript::ExprTkSyntax)
```

- [x] **Step 2: Move only interpreter/runtime sources to `exprtk`**

```cmake
target_link_libraries(exprtk PUBLIC TurboScript::ExprTkSyntax Salts::Core Salts::DataBind)
```

Remove generated lexer/parser source files from `exprtk`; retain values, environments, maps, classes, evaluator and built-in modules.

- [x] **Step 3: Remove Core's direct MIR hash-table dependency**

Replace `mir-htab.h` storage in `exprtk_runtime.c` and `exprtk_class.c` with CSTL raw hash maps. Preserve variable ownership; overloaded constructor/method lookup; abstract-method tracking; static-field storage; and instance-field storage. Core must not include `vendor/mir` or link `mir_static`.

- [x] **Step 4: Run Core tests**

Run: `cmake --build --preset win-dev-user --target exprtk test_exprtk test_exprtk_checked_registration test_mod_math --parallel 4`

Expected: all targets link without `mir_static` on the Core link interface; the matching CTest filter passes.

### Task 3: Move plugin loading to TurboScript

**Files:**
- Move: `exprtk/src/ts_plugin_loader.c` to `turbo_script/src/host/ts_plugin_loader.c`
- Move: `exprtk/include/ts_plugin_loader.h` to `turbo_script/include/ts_plugin_loader.h`
- Modify: `exprtk/CMakeLists.txt`
- Modify: `turbo_script/CMakeLists.txt`
- Modify: `exprtk/test/CMakeLists.txt`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Produces: TurboScript-owned `ts_plugin_load`, `ts_plugin_init` and `ts_plugin_unload` implementation.
- Consumes: `ts_plugin.h` plugin ABI and TurboScript context ownership.

- [x] **Step 1: Move the plugin loader test target under TurboScript**

```cmake
cmake_add_test(
  TARGET test_ts_plugin_loader
  SOURCES test_ts_plugin_loader.c
  LIBS turbo_script Salts::TinyTest
  DEPENDS test_plugin_loader_fixture test_plugin_loader_bad_abi)
```

- [x] **Step 2: Verify the test does not link before the implementation move**

Run: `cmake --build --preset win-dev-user --target test_ts_plugin_loader --parallel 4`

Expected: unresolved plugin-loader symbols.

- [x] **Step 3: Move implementation and update ownership dependencies**

Remove the loader source from `exprtk`; add it to TurboScript host sources. Preserve loader error codes and use TurboScript's path/instance lifecycle; do not add a Core-to-TurboScript dependency.

- [x] **Step 4: Build and run the loader tests**

Run: `cmake --build --preset win-dev-user --target test_ts_plugin_loader --parallel 4`

Expected: target links through TurboScript and `ctest --test-dir build/Msvc -R ^test_ts_plugin_loader$ --output-on-failure` passes.

### Task 4: Preserve TurboScript MIR/JIT ownership

**Files:**
- Modify: `turbo_script/CMakeLists.txt`
- Modify: `docs/architecture/exprtk-layering.md`
- Test: `turbo_script/test/test_turbo_script_basics.c`

**Interfaces:**
- Consumes: `ExprTk::Core` AST/evaluator API.
- Produces: unchanged `turbo_script_run`, `turbo_script_run_jit` and MIR cache APIs.

- [x] **Step 1: Add a regression that runs equivalent interpreter and JIT expressions**

```c
check_equal(turbo_script_run(ctx, "answer = 6 * 7;"), 0);
check_equal(turbo_script_run_jit(ctx, "answer = 6 * 7;"), 0);
check_equal(ts_get_num(ctx, "answer"), 42.0);
```

- [ ] **Step 2: Build TurboScript and its JIT regression target**

Run: `cmake --build --preset win-dev-user --target turbo_script test_turbo_script_basics --parallel 4`

Expected: TurboScript links `exprtk`/Core and MIR only from the TurboScript target.

- [ ] **Step 3: Run the regression test**

Run: `ctest --test-dir build/Msvc -R ^test_turbo_script_basics$ --output-on-failure`

Expected: interpreter and JIT execute the same source with the same result.

### Task 5: Verify static export and package boundaries

**Files:**
- Modify: `cmake/TurboScriptConfig.cmake.in`
- Modify: `README.md`

**Interfaces:**
- Produces: exported `TurboScript::ExprTk` and `TurboScript::ExprTkSyntax` static targets.

- [x] **Step 1: Add package exports for both static targets**

```cmake
install(TARGETS exprtk_syntax exprtk EXPORT TurboScriptTargets)
```

- [ ] **Step 2: Configure and build the smallest package set**

Run: `cmake --preset win-dev-user && cmake --build --preset win-dev-user --target exprtk_syntax exprtk turbo_script --parallel 4`

Expected: all three static dependency layers build with no target cycle.

- [ ] **Step 3: Run final focused tests and whitespace verification**

Run: `ctest --test-dir build/Msvc -R "^(test_exprtk_syntax|test_exprtk|test_ts_plugin_loader|test_turbo_script_basics)$" --output-on-failure && git diff --check`

Expected: all focused tests pass and `git diff --check` has no output.

## Self-Review

- Spec coverage: Tasks 1–2 establish the Syntax/Core boundary, Task 3 moves host-only plugin loading, Task 4 preserves TurboScript-only JIT, and Task 5 verifies exports and static CMake boundaries.
- Placeholder scan: no unspecified interfaces or test commands remain.
- Type consistency: all tasks use the existing `exprtk` Core target and introduce only `exprtk_syntax` / `TurboScript::ExprTkSyntax`.

## Execution Handoff

Execute inline in this session with `superpowers:executing-plans`, preserving review checkpoints after each task.
