# TurboScript `stream.*` → CFlow Migration Matrix

This document is the implementation truth for issue #18. It separates the
TurboScript language facade from the execution owner underneath it.

## Ownership rule

```text
TurboScript syntax / source adapters / diagnostics
                 |
                 v
        CMeta graphability contract
                 |
                 v
          CFlow Graph / Plan
                 |
                 v
         MIR scalar kernels
```

- TurboScript owns syntax, source adaptation, script-visible errors, and the
  decision that a region is graphable.
- CMeta owns callable types/effects used for admission.
- CFlow owns graph topology, validation, optimization, Plan execution, and
  result lifetime.
- MIR owns executable scalar kernels inside admitted CFlow nodes.
- Once a pipeline is admitted to the CFlow runtime path, execution failure is
  an error; it does not silently retry through the legacy ExprTk stream loop.

## Status labels

- **CFlow** — execution is routed through the TurboScript → CMeta → CFlow →
  MIR-kernel path.
- **Legacy adapter** — the language surface exists, but execution remains in
  the existing ExprTk/TurboScript eager stream implementation.
- **Provider hook** — dispatch is delegated to
  `stream.<source_kind>.<method>` when such a function is registered.
- **Documented only** — present in language documentation, but this audit found
  no current core factory/test registration. Do not treat it as an implemented
  core source.

## Source matrix

| Surface | Current implementation truth | Execution owner | Migration target |
| --- | --- | --- | --- |
| `stream.of([1,2,3])` | Numeric vector literal | **CFlow** | Complete Phase 1 source |
| `[1,2,3].stream()` | Numeric vector literal facade | **CFlow** | Complete Phase 1 source |
| `stream.of(vectorVar)` / `vectorVar.stream()` | Runtime binding must be a vector | **CFlow** | Complete Phase 1 source |
| `stream.of(listVar)` / numeric `listVar.stream()` | Runtime list is admitted only when every element is number/int64; empty list is allowed | **CFlow** | Complete Phase 1 numeric-list source |
| heterogeneous list | No coercion to `double` | **Legacy adapter** | Intentionally outside numeric CFlow slice |
| map/object `.stream()` | Existing runtime projects map values into a list-like stream | **Legacy adapter** | Requires typed record/container design before CFlow admission |
| string `.stream()` | Existing runtime materializes string lines | **Legacy adapter** | Fold into text/file adapter work in Phase 2 |
| `stream.text(text)` | Core factory; preserves text metadata and supports `.lines()` / `.split()` | **Legacy adapter** | Phase 2: typed text/range adapter, preserving eager behavior first |
| `stream.text(io.read_file(path)).lines()` | Verified by IO tests; eager file read then text-line materialization | **Legacy adapter** | First concrete Phase 2 file path |
| `stream.lines(path)` | Listed in language guide, but no current core factory/test registration found | **Documented only** | Either implement as a canonical adapter or remove the stale surface |
| `stream.csv(...)` | Listed in language guide; no current core factory/test registration found in this audit | **Documented only** | Establish provider/factory contract before CFlow migration |
| `stream.json(...)` | Listed in language guide; no current core factory/test registration found in this audit | **Documented only** | Establish provider/factory contract before CFlow migration |
| `stream.xml(...)` | Listed in language guide; no current core factory/test registration found in this audit | **Documented only** | Establish provider/factory contract before CFlow migration |

The core runtime currently registers `stream.of` and `stream.text`. A stream
with `source_kind` may also dispatch member calls through the provider hook
`stream.<source_kind>.<method>`.

## Operator / terminal matrix

| Surface | CFlow status | Notes |
| --- | --- | --- |
| `.filter(fn)` | **CFlow** for admitted numeric sources | Callable must pass the current pure/capture-free CMeta admission rule |
| `.map(fn)` | **CFlow** for admitted numeric sources | MIR-backed scalar kernel |
| `.reduce(seed, fn)` | **CFlow** when seed is a numeric literal | Uses CFlow seeded reduce; dynamic seed remains legacy |
| `.count()` | **CFlow** | Scalar terminal |
| `.collect()` | **CFlow** | Preserves legacy result shape: map-free vector pipeline → vector; pipeline containing map → list |
| `.toList()` | **CFlow** | CFlow result copied into TurboScript-owned list before result destruction |
| `.toVector()` | **CFlow** | CFlow result copied into TurboScript-owned vector before result destruction |
| `.forEach(fn)` | **Legacy adapter** | Effectful terminal; keep as an explicit execution barrier until semantics are specified |
| `.filterExpr(expr)` / `.where(expr)` | **Provider hook / legacy adapter** | Generic core fallback does not define a CFlow expression compiler |
| text `.lines()` | **Legacy adapter** | Eager string → list materialization |
| text `.split(sep)` | **Legacy adapter** | Eager string → list materialization |

## Phase 1 parity boundary

Phase 1 is the synchronous numeric in-memory slice:

- vector literals;
- bound vectors;
- homogeneous number/int64 lists;
- pure capture-free `filter` / `map`;
- numeric-literal seeded `reduce`;
- `count`, `collect`, `toList`, and `toVector`;
- interpreter/JIT parity;
- ASan/UBSan/LSan coverage;
- Linux/macOS/Android SDK qualification.

Shapes outside that boundary remain explicitly legacy rather than being
silently coerced into the numeric CFlow model.

## Phase 2 file/data rule

Phase 2 must preserve current eager behavior before introducing incremental
execution:

```text
file / text / format decode
        |
        v
TurboScript or SaltsUtils adapter
        |
        v
typed boundary value / range
        |
        v
CFlow Graph / Plan
```

CFlow stays format-neutral. CSV/JSON/XML parsing, JSONPath/XPath, DataBind
BindingPlan/ValidationPlan, and file I/O remain outside CFlow.

The first verified Phase 2 candidate is
`stream.text(io.read_file(path)).lines()`. Before implementing
`stream.lines/csv/json/xml`, reconcile the language guide with actual
factory/provider registration so the migration does not preserve a
documentation-only API by accident.

## Phase 3 Reactive rule

Reactive/backpressure execution is not implied by the current `stream.*`
syntax. It must be explicitly specified and qualified. Existing file/text
streams are eager today; migration to Publisher/Subscription semantics must not
silently change ordering, ownership, error, or materialization behavior.
