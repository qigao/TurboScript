# TurboScript DSL Execution Plan

This document turns the DSL charter into concrete engineering work.
It is intentionally opinionated.

---

## 1. What We Are Fixing

TurboScript already has many modern language features:

- pipe operator in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:954)
- arrow functions in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:529)
- destructuring in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:591)
- spread in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:774)
- `try/catch/throw` in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:441)
- map literals in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:974)

The problem is not feature count.

The problem is that the language is growing in three directions at once:

- numeric/vector DSL
- structured data DSL
- general-purpose scripting language

That split must be controlled.

---

## 2. Immediate Product Decision

TurboScript should bias toward:

- dataflow
- vectors
- time-series
- maps as records
- host embedding

TurboScript should not bias toward:

- full general-purpose app scripting
- feature parity with JavaScript
- multiple styles for the same common operation

This single decision should drive all future language work.

---

## 3. Syntax Triage

### Keep And Strengthen

These match the DSL direction and should become core:

- `|>` pipe
- `map{}` record literals
- vector literals and slicing
- destructuring
- method-style dispatch on vectors/maps where it improves pipelines
- `try/catch/throw`, but only with one clear error contract

### Keep But Tighten

These are useful, but currently risk semantic drift:

- arrow functions
- spread
- member access and member calls
- `for-in`
- list values

Rule:

- if a feature cannot be explained as “better dataflow” or “better embedding”, narrow it

### Freeze For Now

Do not expand these until the core semantics are stable:

- new control-flow syntax
- new object-model syntax
- new function-definition variants
- more implicit dispatch magic

### Avoid

These are likely wrong for TurboScript unless a real host use case proves otherwise:

- classes
- inheritance
- decorators
- async runtime model
- macro systems
- operator overloading beyond the current data model

---

## 4. Issue Backlog

### P0: Semantic Consistency

#### TS-DLS-001: Unify built-in failure behavior

Problem:

- some built-ins return `0`
- some return `-1`
- some return empty values
- some throw

Done when:

- a single rules table exists in docs
- all core built-ins follow it
- tests assert failure mode, not just success path

Progress:

- global IO built-ins now document the three current failure shapes
- failure-contract tests cover status vs data-returning IO functions
- path/date helpers were aligned with the same compatibility contract

#### TS-DLS-002: Freeze time semantics

Problem:

- `date()` and `format_date()` can silently depend on host timezone

Done when:

- docs explicitly define parse and format timezone behavior
- UTC-specific APIs exist
- tests separate “format shape” from “timezone policy”

Progress:

- legacy `date()` / `format_date()` behavior is now documented as compatibility behavior
- `date_utc()` and `format_date_utc()` provide deterministic UTC-specific APIs
- tests now separate host-local formatting shape from exact UTC formatting

#### TS-DLS-003: Define container contract

Problem:

- `vector`, `map`, and `list` overlap in user mental model

Done when:

- indexing, slicing, missing-key, and truthiness behavior are documented in one place
- APIs use `vector` for numeric pipelines by default
- `list` usage is intentionally limited

Progress:

- the language guide now documents vector/map/list roles, truthiness, and missing-key behavior
- regression tests pin the current map/list truthiness and missing-key contract

### P1: Module And Import Discipline

#### TS-DLS-004: Add explicit export model

Problem:

- current import behavior is execution-oriented, not module-oriented

Done when:

- script modules can explicitly export names
- import side effects are documented and minimized
- duplicate import behavior is deterministic

Progress:

- script imports now support `export(name)` and `export(name, value)`
- `import("./file.ts")` returns an export `map` when explicit exports exist
- repeated imports of the same resolved script path reuse the cached module result
- compatibility side effects still exist, but are now documented as legacy behavior

#### TS-DLS-005: Separate “module namespace” from “global mutation”

Problem:

- too much behavior can leak through globals

Done when:

- plugin/module docs define what becomes global and what stays namespaced

Progress:

- legacy `import()` remains side-effect compatible for existing scripts
- `import_module()` now provides an isolated script-module path that returns exports without leaking globals
- docs now distinguish compatibility imports from isolated module imports

### P1: Pipeline-First Standard Layer

#### TS-DLS-006: Add coherent collection pipeline primitives

Problem:

- the language has pipe syntax, but its standard pipeline vocabulary is still fragmented

Done when:

- there is one standard set of collection helpers for transform, filter, aggregation, and windowing

Suggested surface:

- `map`
- `filter`
- `reduce`
- `take`
- `drop`
- `group_by`
- `window`
- `lag`
- `diff`

Progress:

- vector-first `map`, `filter`, `reduce`, `take`, `drop`, and `lag` now exist as core pipeline primitives
- pipe syntax can now target `map(...)` directly even though `map` is also the map-literal keyword
- this first cut is intentionally vector-centric; generic list/map pipeline semantics remain deferred

### P2: Diagnostics

#### TS-DLS-007: Improve parser and runtime diagnostics

Problem:

- parse and runtime failures can still be technically correct but not pedagogically clear

Done when:

- common errors point to source construct and expected shape
- data-shape mismatches mention actual and expected types in human language

---

## 5. Parser Convergence Plan

### Goal

Reduce grammar growth by making one construct do one job.

### Work Items

#### Parser-1: Restrict future pipe expansion

Current:

- pipe rewrites RHS call by prepending the LHS in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:954)

Action:

- keep `expr |> f(args)` only
- do not add special-case pipe forms until pipeline vocabulary stabilizes

#### Parser-2: Standardize destructuring patterns

Current:

- destructuring enters through several grammar paths in [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:591)

Action:

- define one canonical pattern grammar for vector and map destructuring
- reject exotic forms early with a single parse error family

#### Parser-3: Narrow arrow-function parameter patterns

Current:

- arrow params accept variable, vector, map literal, spread, assignment-shaped nodes via [exprtk_grammar.y](C:/projects/cpp/TurboScript/exprtk/parser/exprtk_grammar.y:103)

Action:

- decide which parameter patterns are truly first-class
- reject the rest instead of preserving every clever form

#### Parser-4: Make module syntax a language feature, not a side effect

Action:

- reserve grammar space for `export`
- avoid piling more behavior into ad hoc import rules

---

## 6. Evaluator Convergence Plan

### Goal

Split the evaluator by semantic domain instead of growing one giant switch forever.

Current hotspot:

- MIR lowering and runtime helper calls

### Work Items

#### Eval-1: Extract value-domain handlers

Split evaluation into dedicated helpers for:

- scalars and arithmetic
- vectors and slicing
- maps and member access
- flow control
- functions and calls
- error propagation

Reason:

- current evaluator is correct in many places, but structurally too central

#### Eval-2: Centralize method dispatch policy

Current:

- member call behavior should live in MIR lowering plus shared runtime helper dispatch

Action:

- define one dispatch order
- document it
- test it

No more “magic, but only for this type” growth.

#### Eval-3: Centralize thrown error values

Current:

- throw path is represented by MIR lowering and runtime helper error state
- helper-thrown runtime errors are surfaced through `exprtk_env_t` and `turbo_script_ctx_t`

Action:

- unify language-thrown and runtime-thrown value shapes
- ensure catch behavior is deterministic and documented

#### Eval-4: Normalize missing-member and invalid-index behavior

Current:

- invalid index throws are reported by MIR lowering/runtime helpers
- member access throws are reported by MIR lowering/runtime helpers
- optional chaining is grammar sugar, so contract must still be documented clearly

Action:

- define the exact rule for:
- invalid access
- missing map key
- null-safe access
- list index out of bounds
- vector index out of bounds

---

## 7. Suggested Milestone Order

### Milestone A: Freeze Semantics

- TS-DLS-001
- TS-DLS-002
- TS-DLS-003

### Milestone B: Reduce Language Surprise

- Parser-2
- Parser-3
- Eval-2
- Eval-4

### Milestone C: Make Pipelines First-Class

- TS-DLS-006
- parser and docs cleanup for pipe-centric usage

### Milestone D: Real Modules

- TS-DLS-004
- TS-DLS-005

### Milestone E: Internal Cleanup

- Eval-1
- Eval-3
- diagnostic polish

---

## 8. Definition Of Success

TurboScript is on the right track when:

- a new user can explain the language in under one minute
- error behavior is predictable without reading source code
- vector and map workflows feel native
- imports behave like modules, not lucky script execution
- new features are rejected more often than they are added

That last point matters.

A healthy DSL says “no” a lot.
