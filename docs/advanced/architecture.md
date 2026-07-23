# TurboScript Engine Design

## Overview

TurboScript is a high-performance embeddable scripting engine with TypeScript-like syntax.
Its public execution model is a pure MIR backend: source is parsed into an AST, validated, lowered to MIR, and then executed by either MIR's interpreter or MIR-generated native code.

TurboScript is meant for host-driven scripting, data transformation, lightweight analysis, quantitative helpers, and extension through C/C++ modules. It is not a full TypeScript, JavaScript, Python, or Lua runtime.

## Execution Pipeline

```text
[ Script Source ]
      |
      v
[ Lexer ] -> [ Parser ] -> [ AST ] -> [ Validation ]
                                      |
                                      v
                                [ MIR Lowering ]
                                      |
                                      v
                                [ MIR Module ]
                               /             \
                              v               v
                    [ MIR Interpreter ]  [ MIR JIT ]
```

`turbo_script_run()` and `turbo_script_run_jit()` share parsing, validation, lowering, runtime helpers, and error behavior. They differ only at the final MIR execution stage.

Unsupported lowering forms must return explicit errors. The host may explicitly choose MIR interpreter mode, but the engine does not hide unsupported JIT forms by switching execution backends.

## Core Components

- `exprtk/parser/exprtk_lexer.re`: lexer source.
- `exprtk/parser/exprtk_grammar.y`: grammar and AST construction.
- `exprtk/include/exprtk_types.h`: AST and runtime value definitions.
- `exprtk/src/exprtk_validate.c`: semantic validation.
- `turbo_script/src/turbo_script.c`: host API, context lifetime, imports, plugins, bindings.
- `turbo_script/src/turbo_script_mir.c`: AST-to-MIR backend and MIR execution APIs.
- `exprtk/src/mod_*.c`: core built-in modules.
- `modules/*`: optional plugins and domain modules.

## Runtime State

`turbo_script_ctx_t` is the top-level state owner:

- `exprtk_env_t env` stores variables, constants, functions, classes, loaded modules, and runtime error state.
- `mir_interp_ctx` stores MIR interpreter modules.
- `mir_ctx` stores MIR JIT modules and generated code.
- JIT cache maps script hashes to compiled native function pointers.
- Plugin state records dynamically loaded modules and import names.

The environment is the single source of script-visible state. MIR-generated code must synchronize through the environment for variables and runtime values that outlive one execution.

Runtime ownership classes, scenario profiles, quotas, and the callback-scoped
WebSocket receive contract are specified in [Runtime Memory Policy](memory-policy.md).

## MIR Lowering

The MIR backend handles:

- numeric expressions and comparisons
- assignment and constants
- `if`, loops, `break`, `continue`, `return`, and `switch`
- function declarations and direct script calls
- closures and dynamic calls through runtime helpers
- vectors, maps, lists, strings, and templates
- destructuring, indexing, slicing, and member access
- class/interface declarations, constructors, methods, fields, `super`, `instanceof`, overload dispatch, and access checks
- parser/mapper and plugin function calls through helper dispatch

Hot paths should stay in MIR instructions where possible. Dynamic or host-owned behavior should be represented as explicit runtime helper calls.

## Runtime Helpers

Runtime helpers are part of the MIR backend contract. They provide operations that cannot be expressed as simple scalar MIR instructions:

- environment load/store
- string/list/map/vector operations
- built-in and plugin dispatch
- OOP class/member/method operations
- parser/data binding
- file, datetime, math, matrix, time-series, TA, and finance helpers

Helpers must report errors through `exprtk_env_t` and `turbo_script_ctx_t` so both MIR interpreter and MIR JIT observe the same behavior.

## Module Resolution

Function resolution order:

1. Script-local functions and closures.
2. Environment-specific modules.
3. Global registry modules.
4. Built-in short-name compatibility lookup for known core namespaces only.

Plugin namespaces must resolve by registered module name. They must not fall through to unrelated short names.

## Data Binding

`modules/mapper` owns class-first JSON, YAML, and XML mapping:

- `mapper.read_json`, `mapper.read_yaml`, `mapper.read_xml`
- `mapper.write_json`, `mapper.write_yaml`, `mapper.write_xml`

TurboScript class field declarations are the type source. The mapper uses
TurboUtils `turbo_parser.h` for document syntax and never loads a separate
schema or codec. `modules/parser` remains limited to configuration text.

`ts`, `ta`, and `fin` should consume canonical runtime values from parser/data binding instead of owning independent parsing semantics.

## Error Policy

TurboScript uses explicit errors:

- parse failure: `TURBO_SCRIPT_ERROR_PARSE`
- validation failure: `TURBO_SCRIPT_ERROR_VALIDATE`
- MIR lowering/JIT failure: `TURBO_SCRIPT_ERROR_JIT`
- runtime helper failure: `TURBO_SCRIPT_ERROR_RUNTIME`
- plugin loading failure: `TURBO_SCRIPT_ERROR_PLUGIN`

Returning a default numeric value to hide an unsupported form is not acceptable. Unsupported features should be covered by tests that assert an error.

## Verification

Preferred validation order:

1. Closest unit test for the changed lowering/helper path.
2. `test_turbo_script_mir` for MIR interpreter and MIR JIT parity.
3. `test_turbo_script` for host API regression.
4. Module-specific tests when parser, data binding, math, string, file, datetime, `ts`, `ta`, or `fin` behavior changes.
5. `bench_turbo_script_mir` for performance-sensitive changes.
