# TurboScript Stable Host Module ABI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 TurboScript 建立同一 immutable module 可创建 MIR interpreter/JIT 固定模式 instance 的稳定纯 C host ABI，并提供双向 host callback、有界 value/result、结构化错误和一致的预算/interrupt 语义。

**Architecture:** `turbo_script_ctx_t` 持有 owner-thread token 与冻结式 host registry；`turbo_script_module_t` 持有一次 parse/validate/lower 得到的 AST、export table、source map 和单一 MIR artifact；每个 `turbo_script_instance_t` 持有独立 runtime context、globals/closures、backend state 与 generation。解释器和 JIT 通过同一 backend bridge 调用同一 export descriptor，不能互相 fallback。

**Tech Stack:** C11、TurboScript exprtk runtime、MIR interpreter/JIT、TurboUtils `tstr`/`turbo_vec_t`/`mem_pool_t`、TinyTest、CMake Presets、MSVC AddressSanitizer。

**Spec:** `docs/superpowers/specs/2026-08-25-turboscript-host-module-abi-design.md`

## Global Constraints

- `TURBO_SCRIPT_HOST_ABI_VERSION` 固定为 `1u`；现有 `TURBO_SCRIPT_NATIVE_ABI_VERSION` 保持 `3`。
- 新 API 完整前仅声明在 `turbo_script/src/host/turbo_script_host_api_internal.h`，不得安装半成品 public declaration。
- module 首版不可跨 context；所有 module/instance/result 操作只能在创建 context 的 owner thread 调用。
- module compile 对 source 与 module name 做拥有型复制；输入 value 与 callback args 都是当前调用栈内 borrowed view。
- result 是 TurboScript allocator 拥有的唯一输出/error owner；reset、下一次写入或 destroy 会使 getter 返回的 view 失效。
- host registry 在第一个 module 创建到最后一个 module 销毁期间冻结；冻结期间 register/unregister 返回 `TURBO_SCRIPT_STATUS_INVALID_STATE`。
- instance mode 在创建时固定为 interpreter 或 JIT；JIT 不支持的语义返回 `TURBO_SCRIPT_STATUS_UNSUPPORTED_BACKEND_SEMANTIC`，不得 fallback。
- export handle `0` 无效；编码为 `(generation << 32) | (slot + 1)`，generation 或 slot 溢出时 fail fast。
- 首版拒绝 context 内任何嵌套 `instance_call`，包括 callback 经 `user_data` 调用另一 instance。
- call options 只能收紧 instance defaults；所有可增长资源必须使用 checked arithmetic 和非零硬上限。
- host module 只接受显式 `export("name")` 导出的顶层具名函数；动态 export name、两参数 value export、重复 export 和超过 16 个参数的 callable 在 compile 阶段返回结构化错误。
- host ABI 默认 profile：source 4 MiB、AST/MIR nodes 100000、imports 64、exports 256、string bytes 4 MiB、instance retained memory 128 MiB、stack 1 MiB、recursion 100、globals 4096、call steps 100000、loop iterations 10000、host callbacks 1024、result bytes 4 MiB、value depth 64、value nodes 65536。
- 本计划不接入 FlexUI Box/EventDispatcher；ABI 安装树验证通过后，在 nanogui/FlexUI 仓库建立独立 adapter 计划。
- 所有 Windows configure/build/test 命令必须在 `VsDevCmd.bat -arch=x64 -host_arch=x64` 环境中运行，并使用版本化 user preset。

## Locked ABI Surface

以下签名先放入 internal API header，Task 9 原样移入 `turbo_script/include/turbo_script.h`：

```c
#define TURBO_SCRIPT_HOST_ABI_VERSION 1u

typedef int32_t turbo_script_status_t;
typedef uint32_t turbo_script_value_kind_t;
typedef uint32_t turbo_script_error_phase_t;
typedef uint32_t turbo_script_execution_mode_t;
typedef uint64_t turbo_script_export_handle_t;

typedef struct turbo_script_module_s turbo_script_module_t;
typedef struct turbo_script_instance_s turbo_script_instance_t;
typedef struct turbo_script_result_s turbo_script_result_t;
typedef struct turbo_script_host_result_builder_s turbo_script_host_result_builder_t;

typedef struct turbo_script_string_view_s {
  const char *data;
  size_t size;
} turbo_script_string_view_t;

typedef struct turbo_script_value_view_s turbo_script_value_view_t;
typedef struct turbo_script_record_entry_view_s turbo_script_record_entry_view_t;

typedef struct turbo_script_array_view_s {
  const turbo_script_value_view_t *items;
  size_t count;
} turbo_script_array_view_t;

typedef struct turbo_script_record_view_s {
  const turbo_script_record_entry_view_t *entries;
  size_t count;
} turbo_script_record_view_t;

struct turbo_script_value_view_s {
  turbo_script_value_kind_t kind;
  uint32_t reserved;
  union {
    uint8_t boolean;
    int64_t integer;
    double number;
    turbo_script_string_view_t string;
    turbo_script_array_view_t array;
    turbo_script_record_view_t record;
  } as;
};

struct turbo_script_record_entry_view_s {
  turbo_script_string_view_t key;
  turbo_script_value_view_t value;
};

typedef struct turbo_script_module_options_s {
  uint32_t struct_size;
  uint32_t reserved0;
  turbo_script_string_view_t module_name;
  size_t max_source_bytes;
  size_t max_ast_nodes;
  size_t max_imports;
  size_t max_exports;
  size_t max_string_bytes;
  uint64_t reserved[4];
} turbo_script_module_options_t;

typedef struct turbo_script_instance_options_s {
  uint32_t struct_size;
  turbo_script_execution_mode_t mode;
  size_t max_retained_bytes;
  size_t max_stack_bytes;
  uint32_t max_recursion;
  uint32_t max_globals;
  uint32_t max_value_depth;
  uint32_t reserved0;
  size_t max_value_nodes;
  size_t max_result_bytes;
  uint64_t reserved[4];
} turbo_script_instance_options_t;

typedef int (*turbo_script_interrupt_fn)(void *user_data);

typedef struct turbo_script_call_options_s {
  uint32_t struct_size;
  uint32_t max_recursion;
  uint32_t max_steps;
  uint32_t max_loop_iterations;
  uint32_t max_host_callbacks;
  uint32_t reserved0;
  size_t max_result_bytes;
  turbo_script_interrupt_fn interrupt;
  void *interrupt_user_data;
  uint64_t reserved[4];
} turbo_script_call_options_t;

typedef struct turbo_script_export_info_s {
  uint32_t struct_size;
  uint32_t min_arity;
  uint32_t max_arity;
  uint32_t reserved0;
  turbo_script_string_view_t name;
  uint64_t reserved[4];
} turbo_script_export_info_t;

typedef struct turbo_script_error_info_s {
  uint32_t struct_size;
  turbo_script_status_t status;
  int32_t error_code;
  turbo_script_error_phase_t phase;
  uint32_t line;
  uint32_t column;
  uint32_t length;
  int32_t cause_code;
  turbo_script_string_view_t module_name;
  turbo_script_string_view_t function_name;
  turbo_script_string_view_t message;
  uint64_t reserved[4];
} turbo_script_error_info_t;

typedef struct turbo_script_host_function_descriptor_s {
  uint32_t struct_size;
  uint32_t min_arity;
  uint32_t max_arity;
  uint32_t reserved0;
  turbo_script_string_view_t name;
  uint64_t reserved[4];
} turbo_script_host_function_descriptor_t;

typedef turbo_script_status_t (*turbo_script_host_function_t)(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder);
```

固定数值常量如下；发布后只能追加，不能重排或复用：

```c
#define TURBO_SCRIPT_STATUS_OK ((turbo_script_status_t)0)
#define TURBO_SCRIPT_STATUS_INVALID_ARGUMENT ((turbo_script_status_t)1)
#define TURBO_SCRIPT_STATUS_OUT_OF_MEMORY ((turbo_script_status_t)2)
#define TURBO_SCRIPT_STATUS_WRONG_THREAD ((turbo_script_status_t)3)
#define TURBO_SCRIPT_STATUS_INVALID_STATE ((turbo_script_status_t)4)
#define TURBO_SCRIPT_STATUS_CONTEXT_MISMATCH ((turbo_script_status_t)5)
#define TURBO_SCRIPT_STATUS_PARSE_ERROR ((turbo_script_status_t)6)
#define TURBO_SCRIPT_STATUS_VALIDATION_ERROR ((turbo_script_status_t)7)
#define TURBO_SCRIPT_STATUS_LIMIT_EXCEEDED ((turbo_script_status_t)8)
#define TURBO_SCRIPT_STATUS_INVALID_UTF8 ((turbo_script_status_t)9)
#define TURBO_SCRIPT_STATUS_DUPLICATE_RECORD_KEY ((turbo_script_status_t)10)
#define TURBO_SCRIPT_STATUS_NOT_FOUND ((turbo_script_status_t)11)
#define TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE ((turbo_script_status_t)12)
#define TURBO_SCRIPT_STATUS_UNSUPPORTED_BACKEND_SEMANTIC ((turbo_script_status_t)13)
#define TURBO_SCRIPT_STATUS_RUNTIME_ERROR ((turbo_script_status_t)14)
#define TURBO_SCRIPT_STATUS_HOST_ERROR ((turbo_script_status_t)15)
#define TURBO_SCRIPT_STATUS_REENTRANT_CALL ((turbo_script_status_t)16)
#define TURBO_SCRIPT_STATUS_INTERRUPTED ((turbo_script_status_t)17)

#define TURBO_SCRIPT_VALUE_NULL ((turbo_script_value_kind_t)0)
#define TURBO_SCRIPT_VALUE_BOOL ((turbo_script_value_kind_t)1)
#define TURBO_SCRIPT_VALUE_INT64 ((turbo_script_value_kind_t)2)
#define TURBO_SCRIPT_VALUE_NUMBER ((turbo_script_value_kind_t)3)
#define TURBO_SCRIPT_VALUE_STRING ((turbo_script_value_kind_t)4)
#define TURBO_SCRIPT_VALUE_ARRAY ((turbo_script_value_kind_t)5)
#define TURBO_SCRIPT_VALUE_RECORD ((turbo_script_value_kind_t)6)

#define TURBO_SCRIPT_ERROR_PHASE_NONE ((turbo_script_error_phase_t)0)
#define TURBO_SCRIPT_ERROR_PHASE_COMPILE ((turbo_script_error_phase_t)1)
#define TURBO_SCRIPT_ERROR_PHASE_INSTANTIATE ((turbo_script_error_phase_t)2)
#define TURBO_SCRIPT_ERROR_PHASE_RESOLVE ((turbo_script_error_phase_t)3)
#define TURBO_SCRIPT_ERROR_PHASE_CALL ((turbo_script_error_phase_t)4)
#define TURBO_SCRIPT_ERROR_PHASE_HOST_CALLBACK ((turbo_script_error_phase_t)5)
#define TURBO_SCRIPT_ERROR_PHASE_INTERRUPT ((turbo_script_error_phase_t)6)

#define TURBO_SCRIPT_EXEC_INTERPRETER ((turbo_script_execution_mode_t)1)
#define TURBO_SCRIPT_EXEC_JIT ((turbo_script_execution_mode_t)2)
```

公开函数名固定如下：

```c
TURBO_SCRIPT_C_API void turbo_script_module_options_init(turbo_script_module_options_t *options);
TURBO_SCRIPT_C_API void turbo_script_instance_options_init(turbo_script_instance_options_t *options);
TURBO_SCRIPT_C_API void turbo_script_call_options_init(turbo_script_call_options_t *options);

TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_result_create(
    turbo_script_ctx_t *ctx, turbo_script_result_t **out_result);
TURBO_SCRIPT_C_API void turbo_script_result_reset(turbo_script_result_t *result);
TURBO_SCRIPT_C_API void turbo_script_result_destroy(turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_result_get_value(
    const turbo_script_result_t *result, turbo_script_value_view_t *out_value);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_result_get_error(
    const turbo_script_result_t *result, turbo_script_error_info_t *out_error);

TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_module_compile(
    turbo_script_ctx_t *ctx, turbo_script_string_view_t source,
    const turbo_script_module_options_t *options, turbo_script_result_t *result,
    turbo_script_module_t **out_module);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_module_destroy(
    turbo_script_module_t *module, turbo_script_result_t *result);

TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_instance_create(
    turbo_script_module_t *module, const turbo_script_instance_options_t *options,
    turbo_script_result_t *result, turbo_script_instance_t **out_instance);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_instance_destroy(
    turbo_script_instance_t *instance, turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_instance_resolve_export(
    turbo_script_instance_t *instance, turbo_script_string_view_t name,
    turbo_script_result_t *result, turbo_script_export_handle_t *out_handle);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_instance_get_export_info(
    turbo_script_instance_t *instance, turbo_script_export_handle_t handle,
    turbo_script_result_t *result, turbo_script_export_info_t *out_info);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_instance_call(
    turbo_script_instance_t *instance, turbo_script_export_handle_t handle,
    const turbo_script_value_view_t *args, size_t arg_count,
    const turbo_script_call_options_t *options, turbo_script_result_t *result);

TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_context_register_host_function(
    turbo_script_ctx_t *ctx, const turbo_script_host_function_descriptor_t *descriptor,
    turbo_script_host_function_t callback, void *user_data,
    turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_context_unregister_host_function(
    turbo_script_ctx_t *ctx, turbo_script_string_view_t name,
    turbo_script_result_t *result);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_host_result_set_value(
    turbo_script_host_result_builder_t *builder,
    const turbo_script_value_view_t *value);
TURBO_SCRIPT_C_API turbo_script_status_t turbo_script_host_result_set_error(
    turbo_script_host_result_builder_t *builder, int32_t cause_code,
    turbo_script_string_view_t message);
```

---

## Preflight Gate: Restore a Reproducible Baseline

**Files:** No TurboScript source changes.

- [ ] **Step 1: Install the matching TurboHttp Debug profile**

Run from `C:\projects\cpp\TurboHTTP` under `VsDevCmd.bat`:

```bat
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
cmake --build --preset install-win-dev-user
```

Expected: `C:\projects\cpp\external\pkgs\turbohttp\debug\lib\cmake\TurboHttp\TurboHttpConfig.cmake` exists. Do not copy `turbohttp-debug` into the new directory and do not add a fallback path.

- [ ] **Step 2: Configure and run the TurboScript baseline**

Run from this worktree under `VsDevCmd.bat`:

```bat
cmake --fresh --preset win-dev-user
cmake --build --preset win-dev-user --target test_turbo_script_basics test_turbo_script_mir_core
ctest --preset win-dev-user -R "^(test_turbo_script_basics|test_turbo_script_mir_core)$" --output-on-failure
```

Expected: configure succeeds and both existing tests pass. Record the exact output in issue #1 before feature edits.

### Task 1: Hidden ABI Types, Result Owner, and Value Validation

**Files:**
- Create: `turbo_script/src/host/turbo_script_host_api_internal.h`
- Create: `turbo_script/src/host/turbo_script_host_internal.h`
- Create: `turbo_script/src/host/turbo_script_host_result.c`
- Create: `turbo_script/test/test_turbo_script_host_result.c`
- Modify: `turbo_script/CMakeLists.txt`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `turbo_script_ctx_t`, `exprtk_value_t`, `tstr`, `turbo_vec_t`, `mem_pool_t`.
- Produces: the locked internal ABI declarations plus `ts_host_validate_value_view()`, `ts_host_value_from_view()`, `ts_host_result_store_view()`, `ts_host_result_set_error()` and result lifecycle used by all later tasks. The internal limit type is fixed as:

```c
typedef struct ts_host_value_limits_s {
  uint32_t max_depth;
  size_t max_nodes;
  size_t max_bytes;
} ts_host_value_limits_t;
```

- [ ] **Step 1: Register the hidden source set and failing TinyTest target**

Add `src/host/*.c` explicitly to `SOURCE_FILES`, add the private include directory, and register:

```cmake
add_ts_test(test_turbo_script_host_result test_turbo_script_host_result.c)
target_include_directories(test_turbo_script_host_result PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/host)
```

- [ ] **Step 2: Write failing result/value tests**

Cover empty string/array/record, every scalar kind, nested array/record, invalid pointer/count, invalid UTF-8, empty/duplicate record key, cyclic input graph, depth 65, node count 65537, byte quota+1, `SIZE_MAX` overflow, result reuse and wrong-context access. One representative case:

```c
it("rejects duplicate record keys without publishing a value") {
  turbo_script_record_entry_view_t entries[2] = {
      {{"id", 2}, {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = 1}},
      {{"id", 2}, {.kind = TURBO_SCRIPT_VALUE_INT64, .as.integer = 2}},
  };
  turbo_script_value_view_t value = {
      .kind = TURBO_SCRIPT_VALUE_RECORD,
      .as.record = {entries, 2},
  };
  check_equal(ts_host_result_store_view(result, &value, &limits),
              TURBO_SCRIPT_STATUS_DUPLICATE_RECORD_KEY);
  check_equal(turbo_script_result_get_value(result, &actual),
              TURBO_SCRIPT_STATUS_INVALID_STATE);
}
```

- [ ] **Step 3: Run the test and verify the missing symbols fail the build**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_result`

Expected: compile/link failure naming the new result/value functions.

- [ ] **Step 4: Implement bounded conversion and result ownership**

Use one `mem_pool_t view_arena` per result. Reset before every write, validate the complete borrowed tree before publishing, deep-copy strings/container data into result-owned storage, and keep `has_value` and `has_error` mutually exclusive. Traversal is O(nodes + UTF-8 bytes), space O(nodes + copied bytes), bounded by the instance/result limits.

- [ ] **Step 5: Run focused tests**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_result
ctest --preset win-dev-user -R "^test_turbo_script_host_result$" --output-on-failure
```

Expected: PASS with no ASan report.

- [ ] **Step 6: Commit**

```bat
git add turbo_script/src/host turbo_script/test/test_turbo_script_host_result.c turbo_script/CMakeLists.txt turbo_script/test/CMakeLists.txt
git commit -m "feat: add bounded TurboScript host results"
```

### Task 2: Context Owner Thread and Frozen Host Registry

**Files:**
- Create: `turbo_script/src/host/turbo_script_host_registry.c`
- Create: `turbo_script/test/test_turbo_script_host_registry.c`
- Modify: `turbo_script/src/turbo_script_internal.h`
- Modify: `turbo_script/src/turbo_script.c`
- Modify: `turbo_script/src/host/turbo_script_host_internal.h`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: result/error functions from Task 1 and legacy `ts_bind_func()` bridge shape.
- Produces: `ts_host_check_owner_thread(ctx)`, immutable registry slots, registration APIs and `ts_host_registry_bind_runtime(ctx, runtime_ctx)`.

- [ ] **Step 1: Write failing registry tests**

Test descriptor `struct_size`, name UTF-8, arity ordering, duplicate name, missing unregister, capacity 256, owner-thread rejection, registry freeze while `active_host_modules > 0`, and unfreeze after the last module. Use a TurboUtils thread to prove wrong-thread status; join it before destroying the context.

```c
static turbo_script_status_t host_echo(
    void *user_data, const turbo_script_value_view_t *args, size_t arg_count,
    turbo_script_host_result_builder_t *builder) {
  (void)user_data;
  if (arg_count != 1) return TURBO_SCRIPT_STATUS_INVALID_ARGUMENT;
  return turbo_script_host_result_set_value(builder, &args[0]);
}
```

- [ ] **Step 2: Run the focused target and verify failure**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_registry`

Expected: missing registration API symbols.

- [ ] **Step 3: Add explicit context state**

Use `TURBO_THREAD_LOCAL` token identity rather than platform thread APIs:

```c
static TURBO_THREAD_LOCAL unsigned char ts_host_thread_token;

ctx->host_owner_thread_token = &ts_host_thread_token;
ctx->active_host_modules = 0;
ctx->host_callback_depth = 0;
ctx->next_instance_generation = 1;
turbo_vec_init(&ctx->host_functions, sizeof(ts_host_function_entry_t));
```

Registry names are owned `tstr`; the vector is capped at 256 and only resolved linearly while mutable. Module lowering stores the stable slot index, so call-time dispatch is O(1). Destroy entries and the vector in the existing final context release path.

- [ ] **Step 4: Implement the exprtk callback adapter and builder terminal rule**

Each registered slot is rebound into an instance runtime context through one internal callback. The builder starts `EMPTY` and permits exactly one `set_value` or `set_error`; callback success with an empty builder becomes `HOST_ERROR`, and a second write becomes `INVALID_STATE`.

- [ ] **Step 5: Run focused and adjacent tests**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_registry test_turbo_script_basics
ctest --preset win-dev-user -R "^(test_turbo_script_host_registry|test_turbo_script_basics)$" --output-on-failure
```

- [ ] **Step 6: Commit**

```bat
git add turbo_script/src turbo_script/test/test_turbo_script_host_registry.c turbo_script/test/CMakeLists.txt
git commit -m "feat: add frozen TurboScript host registry"
```

### Task 3: Immutable Module, Static Export Table, and One MIR Artifact

**Files:**
- Create: `turbo_script/src/host/turbo_script_host_module.c`
- Create: `turbo_script/test/test_turbo_script_host_module.c`
- Modify: `turbo_script/src/host/turbo_script_host_internal.h`
- Modify: `turbo_script/src/mir/turbo_script_mir_internal.h`
- Modify: `turbo_script/src/mir/turbo_script_mir_api.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_functions.c`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: frozen registry slots and structured result errors.
- Produces: `ts_mir_artifact_compile()`, `ts_mir_artifact_destroy()`, immutable `turbo_script_module_t`, ordered export descriptors and parse/lower count of exactly one.

- [ ] **Step 1: Write failing module tests**

Test source/name copying by mutating caller buffers after compile, parse/validation spans, a valid empty export table, missing exported function, duplicate export, dynamic export name, two-argument export, arity 0/1/16/17, source/AST/export/string quotas, host callsite resolution, failed compile leaving registry unfrozen and successful compile freezing it.

Use this valid source in the happy path:

```c
static const char source[] =
    "func add(a, b) { return a + b; };"
    "export(\"add\");";
```

- [ ] **Step 2: Verify the new module test fails**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_module`

- [ ] **Step 3: Extract static export descriptors before lowering**

Walk only the top-level block. Accept literal one-argument `export("name")`, resolve the matching top-level `EXPRTK_NODE_FUNCTION_DEFINITION`, copy name/arity/source span, and reject duplicates before publishing the module. Mark accepted export declaration nodes as compile-time metadata and exclude them from initializer lowering, so runtime never calls the import-only `ts_export()` builtin. Store descriptors in `turbo_vec_t`; complexity is O(AST nodes + exports²) with exports capped at 256, and document the cap beside the scan.

- [ ] **Step 4: Refactor MIR compilation into an owned artifact**

Introduce this private contract:

```c
typedef struct ts_mir_artifact_s ts_mir_artifact_t;
typedef struct ts_host_export_table_s {
  turbo_vec_t entries;
} ts_host_export_table_t;

int ts_mir_artifact_compile(turbo_script_ctx_t *compile_ctx,
                            exprtk_node_t *ast,
                            const ts_host_export_table_t *exports,
                            ts_mir_artifact_t **out_artifact);
void ts_mir_artifact_destroy(ts_mir_artifact_t *artifact);
```

The artifact owns one `MIR_context_t`, one loaded `MIR_module_t`, initializer item, export items and AST-backed immediate lifetimes. Move compile-local arrays out of `turbo_script_ctx_t`; existing legacy compile functions adapt to this internal entry without changing behavior. A module stores `parse_count == 1` and `lower_count == 1` for internal tests/benchmarks.

- [ ] **Step 5: Prove one artifact remains usable by both MIR interfaces**

In the module test, link the same artifact for `MIR_set_interp_interface` and `MIR_set_gen_interface`, retain both the `MIR_item_t` and generated address, then execute a pure numeric export through both. Assert `lower_count == 1`. If MIR invalidates the interpreter item after generator linking, stop this plan and amend the approved spec; do not create a second lowering path.

- [ ] **Step 6: Run module and existing MIR compile tests**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_module test_turbo_script_mir_core
ctest --preset win-dev-user -R "^(test_turbo_script_host_module|test_turbo_script_mir_core)$" --output-on-failure
```

- [ ] **Step 7: Commit**

```bat
git add turbo_script/src/host turbo_script/src/mir turbo_script/test/test_turbo_script_host_module.c turbo_script/test/CMakeLists.txt
git commit -m "feat: compile immutable TurboScript host modules"
```

### Task 4: Mode-Fixed Instances and Generation-Safe Export Handles

**Files:**
- Create: `turbo_script/src/host/turbo_script_host_instance.c`
- Create: `turbo_script/test/test_turbo_script_host_instance.c`
- Modify: `turbo_script/src/host/turbo_script_host_internal.h`
- Modify: `turbo_script/src/host/turbo_script_host_module.c`
- Modify: `turbo_script/src/turbo_script_internal.h`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: immutable module/artifact and registry rebinding.
- Produces: instance create/destroy, independent runtime contexts, fixed mode, resolve/info and generation-safe handles.

- [ ] **Step 1: Write failing lifecycle/handle tests**

Cover interpreter/JIT creation, invalid mode, wrong context result, independent globals, module destroy with live instance, instance destroy while calling, parent context early-close cleanup, found/missing export, handle zero, wrong-instance handle, stale handle after destroy and generation overflow.

```c
check_equal(turbo_script_instance_resolve_export(
                first, (turbo_script_string_view_t){"add", 3}, result, &handle),
            TURBO_SCRIPT_STATUS_OK);
check_not_equal(handle, (turbo_script_export_handle_t)0);
check_equal(turbo_script_instance_get_export_info(second, handle, result, &info),
            TURBO_SCRIPT_STATUS_INVALID_EXPORT_HANDLE);
```

- [ ] **Step 2: Run and observe missing instance symbols**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_instance`

- [ ] **Step 3: Implement instance-owned runtime state**

Create an internal bare runtime context per instance, register the same builtins and frozen host slots, apply instance quotas, and execute the module initializer once. Store globals/closures only in that runtime context. The public parent context owns the registry and generation source; the module owns code/AST. Module creation retains the existing context refcount and module destruction releases it; an early `turbo_script_free(ctx)` marks the context closing, rejects new work, but keeps result/module cleanup valid until their references are released.

- [ ] **Step 4: Implement handle validation**

```c
static uint64_t ts_host_export_handle(uint32_t generation, uint32_t slot) {
  return ((uint64_t)generation << 32) | ((uint64_t)slot + 1u);
}
```

Decode with explicit upper/lower 32-bit checks. `instance_get_export_info()` returns a borrowed name view valid until module destruction and fills the caller-provided `struct_size`-validated info.

- [ ] **Step 5: Run focused and ASan lifecycle tests**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_instance
ctest --preset win-dev-user -R "^test_turbo_script_host_instance$" --output-on-failure
```

- [ ] **Step 6: Commit**

```bat
git add turbo_script/src turbo_script/test/test_turbo_script_host_instance.c turbo_script/test/CMakeLists.txt
git commit -m "feat: add mode-fixed TurboScript instances"
```

### Task 5: Generic Export Calls for Interpreter and JIT

**Files:**
- Create: `turbo_script/test/test_turbo_script_host_call.c`
- Modify: `turbo_script/src/host/turbo_script_host_instance.c`
- Modify: `turbo_script/src/host/turbo_script_host_result.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_internal.h`
- Modify: `turbo_script/src/mir/turbo_script_mir_functions.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_value_emit.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_value_runtime.c`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: resolved export handle, value validation and mode-fixed backend.
- Produces: `ts_mir_artifact_call_interp()` and `ts_mir_artifact_call_jit()` with identical generic `exprtk_value_t` input/output semantics.

- [ ] **Step 1: Write the data-driven call matrix**

Run each case through both modes: null, bool, int64 min/max, number, UTF-8 string, empty/nested list and record, zero/one/16 args, wrong arity, repeated stateful call, closure isolation and non-finite numbers. Compare integers exactly and numbers with absolute/relative tolerance `1e-12`.

```c
typedef struct host_call_case_s {
  const char *name;
  const char *source;
  const char *export_name;
  const turbo_script_value_view_t *args;
  size_t arg_count;
  turbo_script_value_view_t expected;
} host_call_case_t;
```

- [ ] **Step 2: Verify both backends initially fail the new call matrix**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_call`

- [ ] **Step 3: Add a generic MIR export ABI**

Each exported MIR item uses this internal signature:

```c
typedef int (*ts_mir_host_export_fn)(turbo_script_ctx_t *runtime_ctx,
                                     const exprtk_value_t *args,
                                     size_t arg_count,
                                     exprtk_value_t *out_value);
```

Extend function lowering so parameters load tagged values and return writes one owned `exprtk_value_t` through `out_value`. Numeric expressions retain native MIR arithmetic; string/list/record/OOP operations call existing value-preserving runtime helpers from generated control flow. If a node has no JIT lowering or approved runtime bridge, module instantiation in JIT mode returns `UNSUPPORTED_BACKEND_SEMANTIC`; it must not evaluate that function through `exprtk_eval`.

- [ ] **Step 4: Implement call preflight and atomic result publish**

Validate args and quotas before setting state to Calling. Convert borrowed public views into instance-owned temporary exprtk values, invoke the selected backend, convert the complete output into result-owned storage, then destroy temporaries. Any conversion or runtime failure publishes only an error.

- [ ] **Step 5: Run focused, value-runtime and MIR regression tests**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_call test_turbo_script_mir test_turbo_script_mir_advanced
ctest --preset win-dev-user -R "^(test_turbo_script_host_call|test_turbo_script_mir|test_turbo_script_mir_advanced)$" --output-on-failure
```

- [ ] **Step 6: Commit**

```bat
git add turbo_script/src turbo_script/test/test_turbo_script_host_call.c turbo_script/test/CMakeLists.txt
git commit -m "feat: call TurboScript exports through both backends"
```

### Task 6: Structured Errors, State Machine, and Reentrancy

**Files:**
- Create: `turbo_script/test/test_turbo_script_host_state.c`
- Modify: `turbo_script/src/host/turbo_script_host_instance.c`
- Modify: `turbo_script/src/host/turbo_script_host_registry.c`
- Modify: `turbo_script/src/host/turbo_script_host_result.c`
- Modify: `turbo_script/src/turbo_script_internal.h`
- Modify: `turbo_script/src/mir/turbo_script_mir_runtime.c`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: call pipeline and host callback builder.
- Produces: Ready/Calling/CallingHost/Faulted transitions, context-wide reentrancy guard and complete `turbo_script_error_info_t`. Internal state values are fixed as:

```c
typedef uint32_t ts_host_instance_state_t;
#define TS_HOST_INSTANCE_READY ((ts_host_instance_state_t)1)
#define TS_HOST_INSTANCE_CALLING ((ts_host_instance_state_t)2)
#define TS_HOST_INSTANCE_CALLING_HOST ((ts_host_instance_state_t)3)
#define TS_HOST_INSTANCE_FAULTED ((ts_host_instance_state_t)4)
#define TS_HOST_INSTANCE_DESTROYED ((ts_host_instance_state_t)5)
```

- [ ] **Step 1: Write the failure matrix tests**

Assert preflight errors keep Ready; handled script errors return a value and keep Ready; uncaught runtime, host callback failure, quota and interrupt fault the instance; Faulted rejects future calls; nested same-instance and other-instance calls return `REENTRANT_CALL`; errors carry phase, module/function, source span, message and cause.

```c
typedef struct host_failure_case_s {
  const char *name;
  turbo_script_status_t expected_status;
  turbo_script_error_phase_t expected_phase;
  ts_host_instance_state_t expected_state;
} host_failure_case_t;
```

- [ ] **Step 2: Run the state test and verify failure**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_state`

- [ ] **Step 3: Centralize state transitions**

Implement `ts_host_instance_begin_call()`, `ts_host_instance_enter_callback()`, `ts_host_instance_leave_callback()` and `ts_host_instance_finish_call()`. Only these functions mutate state/callback depth. Cleanup always returns temporary ownership exactly once and converts one error at the public boundary.

- [ ] **Step 4: Map legacy parser/runtime diagnostics into result-owned errors**

Copy module/function names and source spans into the result arena. Do not log inside the runtime layers and do not expose `ctx->error_msg` pointers.

- [ ] **Step 5: Run state, callback, task and timer regressions**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_state test_turbo_script_task test_turbo_script_timer
ctest --preset win-dev-user -R "^(test_turbo_script_host_state|test_turbo_script_task|test_turbo_script_timer)$" --output-on-failure
```

- [ ] **Step 6: Commit**

```bat
git add turbo_script/src turbo_script/test/test_turbo_script_host_state.c turbo_script/test/CMakeLists.txt
git commit -m "feat: enforce TurboScript host call state"
```

### Task 7: Shared Limits and Safe-Point Interrupts

**Files:**
- Create: `turbo_script/src/host/turbo_script_host_limits.c`
- Create: `turbo_script/test/test_turbo_script_host_limits.c`
- Modify: `turbo_script/src/host/turbo_script_host_instance.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_internal.h`
- Modify: `turbo_script/src/mir/turbo_script_mir_runtime.c`
- Modify: `turbo_script/src/turbo_script_mir.c`
- Modify: `exprtk/src/exprtk_eval.c`
- Modify: `exprtk/include/exprtk_types.h`
- Modify: `turbo_script/test/CMakeLists.txt`

**Interfaces:**
- Consumes: instance/call defaults and state transitions.
- Produces: checked call budget, shared safe-point helper and equivalent interpreter/JIT quota/interrupt behavior.

- [ ] **Step 1: Write boundary and interruption tests**

Cover 0, 1, exact limit, limit+1, `SIZE_MAX`, call attempts to widen defaults, recursion, stack bytes, steps, loops, callback count, result bytes, retained memory, interrupt before entry, interrupt at loop backedge and long-loop termination for both modes.

```c
typedef struct interrupt_probe_s {
  atomic_int requested;
  size_t checks;
} interrupt_probe_t;

static int test_interrupt(void *user_data) {
  interrupt_probe_t *probe = (interrupt_probe_t *)user_data;
  probe->checks++;
  return atomic_load(&probe->requested) != 0;
}
```

- [ ] **Step 2: Run and verify the new tests fail**

Run: `cmake --build --preset win-dev-user --target test_turbo_script_host_limits`

- [ ] **Step 3: Implement one budget object per call**

```c
typedef struct ts_host_call_budget_s {
  uint32_t steps_left;
  uint32_t loops_left;
  uint32_t callbacks_left;
  uint32_t recursion_left;
  size_t stack_bytes_left;
  size_t result_bytes_left;
  turbo_script_interrupt_fn interrupt;
  void *interrupt_user_data;
} ts_host_call_budget_t;
```

Attach the borrowed budget only for the synchronous call duration. Check at function entry, loop backedge, before/after host callback and every 256 MIR instructions/evaluator nodes. Each function entry charges the artifact-recorded MIR frame bytes plus runtime value temporaries and releases the same charge on exit; checked subtraction failure is a stack quota error. The callback is invoked only on the owner thread and cannot call TurboScript.

- [ ] **Step 4: Insert equivalent MIR guards**

Emit calls to one `ts_host_safe_point(runtime_ctx, kind, cost)` helper from both evaluator and generated MIR. A guard failure sets the same structured status/phase and reaches the same Faulted transition; no backend retry is allowed.

- [ ] **Step 5: Run limits and long-loop regressions under ASan**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_limits test_turbo_script_mir_advanced
ctest --preset win-dev-user -R "^(test_turbo_script_host_limits|test_turbo_script_mir_advanced)$" --output-on-failure
```

- [ ] **Step 6: Commit**

```bat
git add turbo_script/src turbo_script/test/test_turbo_script_host_limits.c turbo_script/test/CMakeLists.txt exprtk/src/exprtk_eval.c exprtk/include/exprtk_types.h
git commit -m "feat: bound TurboScript host calls"
```

### Task 8: Interpreter/JIT Contract Suite and Backend Parity Gate

**Files:**
- Create: `turbo_script/test/test_turbo_script_host_contract.c`
- Modify: `turbo_script/test/CMakeLists.txt`
- Modify: `turbo_script/src/mir/turbo_script_mir_functions.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_value_emit.c`
- Modify: `turbo_script/src/mir/turbo_script_mir_value_runtime.c`
- Modify: `turbo_script/src/host/turbo_script_host_instance.c`
- Modify: `turbo_script/src/host/turbo_script_host_result.c`
- Modify: `turbo_script/src/host/turbo_script_host_limits.c`

**Interfaces:**
- Consumes: complete hidden host ABI.
- Produces: one data-driven suite that runs identical call sequences against both modes and rejects semantic drift.

- [ ] **Step 1: Build a shared scenario runner**

Define each scenario with source, export, args, expected value/error/state and numeric tolerance. The runner creates one module, creates both instances, resolves each handle independently, executes the same ordered calls and compares observable results, callback trace and state.

```c
typedef struct host_contract_scenario_s {
  const char *name;
  const char *source;
  const char *export_name;
  const turbo_script_value_view_t *args;
  size_t arg_count;
  turbo_script_status_t expected_status;
  turbo_script_value_view_t expected_value;
  turbo_script_error_phase_t expected_phase;
} host_contract_scenario_t;

static void run_contract_scenario(const host_contract_scenario_t *scenario) {
  static const turbo_script_execution_mode_t modes[] = {
      TURBO_SCRIPT_EXEC_INTERPRETER,
      TURBO_SCRIPT_EXEC_JIT,
  };
  for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    run_contract_mode(scenario, modes[i]);
  }
}
```

`run_contract_mode(const host_contract_scenario_t *, turbo_script_execution_mode_t)` is implemented in this test file and performs complete result → instance → module → context cleanup on every branch.

- [ ] **Step 2: Add the full contract matrix**

Include export metadata, all value kinds, globals, closures, repeated calls, multiple instances, callback order/args/results, wrong/stale handles, structured errors, quotas, interrupt categories and teardown. Capture scenario name and both backend values with TinyTest `info()` on mismatch.

- [ ] **Step 3: Run the contract suite and fix backend differences at their source**

Run:

```bat
cmake --build --preset win-dev-user --target test_turbo_script_host_contract
ctest --preset win-dev-user -R "^test_turbo_script_host_contract$" --output-on-failure
```

Expected: all scenarios pass for both modes. Do not widen the global `1e-12` tolerance; document a local expression-specific tolerance only when IEEE operation ordering proves it necessary.

- [ ] **Step 4: Run all TurboScript tests**

Run: `ctest --preset win-dev-user -R "^test_turbo_script" --output-on-failure`

- [ ] **Step 5: Commit**

```bat
git add turbo_script/src turbo_script/test
git commit -m "test: enforce TurboScript backend parity"
```

### Task 9: Publish the Complete ABI, Installed Consumers, and Legacy Wrappers

**Files:**
- Modify: `turbo_script/include/turbo_script.h`
- Delete: `turbo_script/src/host/turbo_script_host_api_internal.h`
- Modify: `turbo_script/src/host/turbo_script_host_internal.h`
- Modify: `turbo_script/src/turbo_script.c`
- Create: `turbo_script/test/consumer/CMakeLists.txt`
- Create: `turbo_script/test/consumer/host_consumer.c`
- Create: `turbo_script/test/consumer/host_consumer.cpp`
- Create: `turbo_script/test/verify_host_consumers.cmake`
- Modify: `turbo_script/test/CMakeLists.txt`
- Modify: `docs/api/api-reference.md`
- Modify: `docs/zh/api-reference.md`

**Interfaces:**
- Consumes: hidden ABI with passing contract suite.
- Produces: installed C/C++ ABI v1 and unchanged legacy `run`/`run_jit`/`compile`/`exec`/`ts_bind_func` behavior.

- [ ] **Step 1: Add failing installed-header and layout checks**

The C consumer includes only `<turbo_script.h>`, registers `host_add`, compiles an exported script, calls it in both modes and destroys in result → instance → module → context order. The C++ consumer wraps the same C functions without accessing internal headers. Add `_Static_assert`/`static_assert` for fixed-width fields, `offsetof` ordering and reserved slots.

```c
_Static_assert(sizeof(turbo_script_status_t) == 4, "host status ABI");
_Static_assert(sizeof(turbo_script_export_handle_t) == 8, "export handle ABI");
_Static_assert(offsetof(turbo_script_module_options_t, reserved) >
                   offsetof(turbo_script_module_options_t, max_string_bytes),
               "module options ABI order");
```

- [ ] **Step 2: Move declarations into the installed header atomically**

Move the complete locked ABI block, status/value constants, ownership comments, parameter/error documentation and a minimal runnable example into `turbo_script.h`; update internal includes to consume the public declaration. No symbol may return a not-implemented status.

- [ ] **Step 3: Convert legacy compile/exec to wrappers after parity is green**

`turbo_script_compiled_t` owns an internal zero-export-compatible module artifact but no isolated instance. `turbo_script_exec(ctx, compiled)` invokes that artifact's initializer through MIR interpreter against the caller's original `ctx`, so `ts_bind_num()` before each exec and `ts_get_*()` afterward continue to observe the same environment. It must not execute at compile time, must permit scripts without `export()`, and must preserve the old opaque name and binary symbols.

- [ ] **Step 4: Install and run external consumers**

Run under `VsDevCmd.bat`:

```bat
cmake --build --preset install-win-dev-user
ctest --preset win-dev-user -R "^(verify_turbo_script_host_consumer|test_turbo_script_host_contract|test_turbo_script_basics)$" --output-on-failure
```

Expected: consumer configure uses `find_package(TurboScript CONFIG REQUIRED)` and links only `TurboScript::TurboScript` from the Debug install prefix.

- [ ] **Step 5: Run legacy and full regression tests**

Run: `ctest --preset win-dev-user --output-on-failure`

- [ ] **Step 6: Commit**

```bat
git add turbo_script/include turbo_script/src turbo_script/test docs/api/api-reference.md docs/zh/api-reference.md
git commit -m "feat: publish TurboScript host module ABI"
```

### Task 10: Performance Evidence, Teardown Verification, and FlexUI Handoff

**Files:**
- Create: `turbo_script/test/bench_turbo_script_host.c`
- Modify: `turbo_script/test/CMakeLists.txt`
- Modify: `turbo_script/test/README.md`
- Modify: `docs/jit-guide.md`
- Modify: `docs/INDEX.md`

**Interfaces:**
- Consumes: public host ABI and install-tree consumers.
- Produces: repeatable compile/instance/first-call/steady-call/callback/interrupt measurements and a documented FlexUI adapter contract.

- [ ] **Step 1: Add correctness assertions outside timed blocks**

Compile a module with `func update(x) { return x + 1; }; export("update");`, call it once in each mode, assert result `42` for input `41`, and assert internal parse/lower counts remain one after 10000 calls.

- [ ] **Step 2: Add TinyTest benchmarks with exact work units**

Use `benchmark_batch` for module compile, instance create, first call and interrupt latency. Use `benchmark_ops("steady host call", 1000, 1000)` with exactly 1000 calls inside each timed block. Report interpreter and JIT separately; setup/result validation stays outside the timed block.

```c
benchmark_ops("steady host call", 1000, 1000) {
  for (size_t i = 0; i < 1000; ++i) {
    status = turbo_script_instance_call(instance, handle, &arg, 1, &call_options, result);
  }
}
check_equal(status, TURBO_SCRIPT_STATUS_OK);
```

- [ ] **Step 3: Run Release benchmark and record evidence**

Run under `VsDevCmd.bat`:

```bat
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user --target bench_turbo_script_host
build\Msvc-Release\bin\bench_turbo_script_host.exe
```

Record compiler, preset, CPU, samples, avg/op, ops/s and peak result/instance retained bytes in issue #1. Treat latency +10%, throughput -10% or memory +20% versus the recorded baseline as a review gate, not as an automatic code rewrite trigger.

- [ ] **Step 4: Run final Debug/ASan and Release verification**

Run:

```bat
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
cmake --build --preset win-release-user
ctest --preset win-release-user --output-on-failure
cmake --build --preset install-win-release-user
```

- [ ] **Step 5: Document the FlexUI boundary**

Document that FlexUI owns Box/UiDataContext, compiles one controller module, creates a configured mode-fixed instance, resolves every `EventBinding.handler` at load time, passes an immutable event record, collects mutation/application commands in host callbacks, and commits them only after a successful call. Link GitHub issue #1 and identify a new FlexUI adapter issue as the next change.

- [ ] **Step 6: Commit**

```bat
git add turbo_script/test docs
git commit -m "perf: benchmark TurboScript host calls"
```

## Final Acceptance Matrix

- Functional: result, registry, module, instance, call, state, limits and contract targets pass.
- Compatibility: every pre-existing TurboScript CTest passes without changed stdout/error/context-variable behavior.
- Memory: Debug/ASan reports no leak, UAF, double-free or invalid cross-CRT free for compile failure, callback failure, interrupt, result reuse and teardown.
- Backend: one module records one parse and one lower; interpreter/JIT share export metadata and pass identical scenarios without fallback.
- Packaging: Debug and Release installs compile/run external C11 and C++17 consumers using only installed headers and `TurboScript::TurboScript`.
- Performance: Release evidence reports modes separately and proves steady calls do not parse/lower or allocate unbounded state.
- Tracking: issue #1 contains baseline output, each phase commit, contract results, final benchmark and the follow-up FlexUI adapter issue.
