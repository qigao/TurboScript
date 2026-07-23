# TurboScript DSL Charter

This document defines what TurboScript is, what it is not, and how the language should evolve.
Its job is to keep the engine useful for embedded scripting without turning it into a general-purpose feature pile.

---

## 1. Purpose

TurboScript is a high-performance embeddable scripting engine with TypeScript-like syntax.
It is built for:

- host application scripting and automation
- data processing
- data binding and format conversion
- lightweight data analysis
- quantitative finance
- vector and time-series analysis

TurboScript is not trying to be a full TypeScript, JavaScript, Python, or Lua replacement. It should feel familiar to users of TypeScript/JavaScript syntax, but it does not implement the TypeScript type system or the browser/server JavaScript ecosystem.

---

## 2. Core Identity

TurboScript should optimize for four things:

1. Clear dataflow
2. Strong vector, matrix, and time-series ergonomics
3. Predictable interpreter/JIT behavior
4. Easy embedding and extension from C/C++

If a feature does not materially improve one of these, it should not be added.

---

## 3. First-Class Data Model

The language should revolve around a small number of primary value shapes:

- `number`: scalar numeric computation
- `string`: text, paths, dates, messages
- `vector`: dense numeric arrays for math and time-series work
- `object`: host/parser plain records and dynamic data
- `map`: explicit script key-value containers and schema map fields
- `class`/`instance`: domain objects, interfaces, and encapsulated behavior
- `null`: explicit absence

`list` is allowed, but it should remain secondary. It is useful for heterogeneous transport, not for the center of the DSL.

Classes and interfaces are supported, but they are a modeling tool, not the center of the language. They should serve embedded APIs, domain models, and host integration rather than encourage large framework-style object hierarchies.

Design rule:

- numeric pipelines should prefer `vector`
- host/parser/schema records should prefer plain `object`
- script-authored key-value containers should prefer `map`
- APIs should not force users to guess between `vector` and `list` unless there is a real semantic reason

---

## 4. Semantic Contracts

The language must be more consistent than clever.

### Error Handling

One error model should dominate:

- programmer misuse and invalid operations should throw
- expected absence should return `null` or an empty value, but only where documented

The current mix of `0`, `-1`, empty values, and thrown errors should be reduced over time.

### Time

Time behavior must be explicit:

- document whether parsing is local time, UTC, or format-dependent
- document whether formatting uses local time by default
- add UTC-aware APIs rather than relying on hidden host timezone behavior

### Containers

The same operation should mean the same thing everywhere:

- indexing rules
- slicing rules
- truthiness rules
- mutation rules
- missing-key behavior

No silent semantic drift between modules.

### Modules

A script import should behave like a language feature, not a side effect lottery:

- deterministic resolution
- caching rules
- explicit exports
- minimal hidden global mutation
- clear namespace ownership; module-prefixed calls such as `ta.sma` and `strategy.var_hist` must not resolve through unrelated aliases

### Execution

Interpreter and MIR JIT must agree on user-visible semantics:

- unsupported JIT forms should report unsupported syntax or unsupported lowering
- unsupported lowering must fail explicitly in the MIR pipeline
- runtime helper lowering is acceptable when it preserves the same semantics and error behavior as the interpreter

---

## 5. Non-Goals

TurboScript should not chase these by default:

- large object-oriented frameworks
- syntax added only to look modern
- multiple competing ways to do the same common task
- full browser-style async ecosystems
- language-level complexity that does not help data pipelines or host embedding
- full Python/Pandas/NumPy compatibility

---

## 6. Priority Roadmap

### Now

1. Freeze semantic rules for errors, time, containers, and imports.
2. Make data pipelines first-class with consistent `map`, `filter`, `reduce`, windowing, and series helpers.
3. Tighten module boundaries with explicit export/import behavior.
4. Audit built-ins so return conventions are predictable.
5. Keep interpreter and MIR JIT behavior aligned with explicit unsupported-feature errors.

### Next

1. Add UTC/timezone-explicit date APIs.
2. Improve `map` and `vector` interoperability for real pipeline work.
3. Add a small, coherent query/dataframe-style layer only if it fits the core model.
4. Improve diagnostics so parse/runtime/type errors point to the exact source construct.

### Later

1. Optimize hot pipeline paths in JIT and interpreter consistently.
2. Add domain-focused standard libraries where the host ecosystem clearly benefits.

---

## 7. Acceptance Test For New Language Features

Before adding syntax or a built-in, ask:

1. Does it improve dataflow, vectors, time-series work, or embedding?
2. Can it be explained in one short paragraph?
3. Does it remove complexity instead of adding a new special case?
4. Does it preserve existing user-visible behavior?
5. Would a library function solve this better than new syntax?

If the answer to `1` is no, the feature should probably not exist.

---

## 8. The Default Direction

TurboScript should become a small, sharp embedded scripting engine for structured numeric and data-oriented scripting.

That means:

- fewer semantic surprises
- stronger pipeline composition
- better time-series primitives
- clearer module boundaries
- less imitation of general-purpose languages
- interpreter/JIT consistency before breadth

The language wins by being opinionated and reliable, not by being universal.
