# TurboScript Architecture

## Overview

TurboScript is a high-performance embeddable scripting engine with TypeScript-like syntax.
The public execution backend is MIR only:

```text
source
  -> lexer/parser
  -> AST
  -> validation
  -> MIR lowering
  -> MIR module
       |-> MIR interpreter   (`turbo_script_run`)
       |-> MIR native code   (`turbo_script_run_jit`)
```

The AST is the frontend representation. It is not a public execution backend.
Unsupported language forms must fail during validation, MIR lowering, or runtime helper execution.

## Frontend

The frontend is implemented by `exprtk`:

- `parser/exprtk_lexer.re`: re2c lexer.
- `parser/exprtk_grammar.y`: Lemon grammar.
- `include/exprtk_types.h`: AST nodes and runtime value shapes.
- `src/exprtk_validate.c`: validation and semantic checks.

The parser owns syntax and AST construction. Constant folding and simple syntax normalization can happen here, but execution policy belongs to the MIR backend.

## Runtime State

`turbo_script_ctx_t` owns one script environment and the MIR state:

- `exprtk_env_t env`: variables, constants, functions, classes, modules, and runtime error state.
- `mir_interp_ctx`: MIR context used by `turbo_script_run` and `turbo_script_run_mir_interp`.
- `mir_ctx`: MIR context used by `turbo_script_run_jit`.
- JIT cache: script hash to compiled native function pointer.
- Plugin handles and script import state.

Variables persist across runs on the same context because the environment is the single source of runtime state.

## MIR Backend

`turbo_script/src/turbo_script_mir.c` is the backend:

- Parses and validates source.
- Lowers AST nodes to MIR instructions.
- Emits runtime helper calls for dynamic values, OOP, maps, strings, parser/data binding, and other non-scalar operations.
- Emits optimized paths for numeric expressions, loops, direct math calls, pre-bound vectors, map numeric pointers, OOP method call caches, and public numeric instance field slots.
- Executes the same MIR through either MIR interpreter or MIR JIT.

`turbo_script_run()` and `turbo_script_run_jit()` share this pipeline. They differ only in the final MIR execution mode.

## Runtime Helpers

Runtime helpers are C functions callable from MIR. They are used when an operation is dynamic or host-owned:

- environment load/store
- string/list/map/vector operations
- module dispatch
- OOP class definition, instance creation, member access, method dispatch, `super`, and `instanceof`
- parser/data binding, file, datetime, math/statistics, and plugin-backed functionality

Helpers must preserve the same semantics in MIR interpreter and MIR JIT mode. They should report errors through the context/environment state instead of manufacturing default values that hide failures.

## Modules

Core built-ins live under `exprtk/src` and optional extensions live under `modules/*`.

Important module groups:

- `core`, `math`, `stats`, `string`, `regex`, `io`
- `mapper`: class-first JSON/YAML/XML mapping through Salts DataBind parser APIs
- `ts`, `ta`, `fin`: time-series, technical analysis, and finance helpers
- `net`, `sqlite`, and other plugin modules

The mapper is the canonical path for structured JSON/YAML/XML values. TurboScript
class field declarations are the type metadata and Salts DataBind parser APIs
owns document syntax. Data modules should consume typed class instances rather
than introducing a second schema format.

## Error Policy

TurboScript should fail explicitly when a script cannot be represented by MIR:

- parse errors use `TURBO_SCRIPT_ERROR_PARSE`
- validation errors use `TURBO_SCRIPT_ERROR_VALIDATE`
- MIR lowering or native compilation errors use `TURBO_SCRIPT_ERROR_JIT`
- runtime helper failures use `TURBO_SCRIPT_ERROR_RUNTIME`

No execution mode should silently switch to another backend. The host may choose MIR interpreter or MIR JIT explicitly, but both use the same MIR lowering contract.

## Testing

Use focused tests first:

- `test_turbo_script_mir`: MIR lowering, MIR interpreter, MIR JIT, and parity between both modes.
- `test_turbo_script`: public host API behavior.
- module-specific tests under `modules/*/test`.

For performance-sensitive changes, run `bench_turbo_script_mir` and compare warm/JIT exec-only results.
