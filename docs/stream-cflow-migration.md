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
- The ExprTk `filter/map/reduce` loops are barrier-only fallback code for
  shapes rejected before admission. Admitted numeric/text terminals must never
  reach those loops.

## Status labels

- **CFlow** — execution is routed through the TurboScript → CMeta → CFlow →
  MIR-kernel path.
- **Legacy barrier** — the shape is rejected before CFlow admission and remains
  in the existing ExprTk/TurboScript eager implementation. This is a migration
  barrier, not a peer runtime; new graphable stream semantics must not be added here.
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
| heterogeneous list | No coercion to `double` | **Legacy barrier** | Intentionally outside numeric CFlow slice |
| map/object `.stream()` | Existing runtime projects map values into a list-like stream | **Legacy barrier** | Requires typed record/container design before CFlow admission |
| string `.stream()` | Existing runtime materializes string lines | **Legacy barrier** | Fold into text/file adapter work in Phase 2 |
| `stream.text(text)` | Core factory; preserves text metadata and supports `.lines()` / `.split()` | **Legacy barrier** | Factory and bare line-stream materialization stay eager; selected terminals use typed CFlow |
| `stream.text(text).lines().count()` | Borrowed line slices preserve legacy newline/CRLF/trailing-empty-line semantics | **CFlow** | Typed scalar Phase 2 terminal |
| `stream.text(text).lines().collect()` / `.toList()` | CFlow result slices are copied into a TurboScript-owned string list before source/result cleanup | **CFlow** | Typed owned materialization terminal |
| `stream.text(io.read_file(path)).lines().count/collect/toList` | File I/O remains TurboScript-owned; evaluated text feeds the same typed line Plan | **CFlow** | Verified Phase 2 file path |
| `stream.text(text).split(sep).count/collect/toList` | Reuses typed borrowed text slices; empty separator is byte-oriented and leading/trailing/adjacent empty tokens are preserved | **CFlow** | Typed split terminals |
| `stream.text(io.read_file(path)).split(sep).count/collect/toList` | File I/O remains TurboScript-owned; text + separator expressions are each evaluated once | **CFlow** | Verified Phase 2 file split path |
| `stream.text(text).lines().map(line => line.length()).toVector/toList/collect` | Exact capture-free typed MAP: borrowed LineSlice -> double | **CFlow** | First typed text callable vertical slice |
| `stream.text(io.read_file(path)).lines().map(line => line.length()).toVector` | File I/O stays TurboScript-owned; typed MAP runs in the same CFlow Plan | **CFlow** | Verified file-backed typed MAP |
| `stream.text(text).lines().filter(line => line.length() > 0).count/collect/toList` | Exact capture-free typed predicate: borrowed LineSlice -> bool; FILTER preserves LineSlice output type | **CFlow** | First typed text FILTER vertical slice |
| `stream.text(io.read_file(path)).lines().filter(line => line.length() > 0).count/toList` | File I/O stays TurboScript-owned; predicate runs in the same typed CFlow Plan | **CFlow** | Verified file-backed typed FILTER |
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
| `.forEach(fn)` | **Legacy barrier** | Effectful terminal; keep as an explicit execution barrier until semantics are specified |
| `.filterExpr(expr)` / `.where(expr)` | **Provider hook / legacy barrier** | Generic core fallback does not define a CFlow expression compiler |
| text `.lines()` | **Legacy barrier** when used as an intermediate stream; **CFlow** for exact `count/collect/toList` terminals | CFlow uses borrowed line slices synchronously; collect/toList deep-copy strings into TurboScript-owned list storage |
| text `.split(sep)` | **Legacy barrier** when used as an intermediate stream; **CFlow** for exact `count/collect/toList` terminals | Empty separator splits by byte; non-string separator yields an empty stream; collect/toList deep-copy token strings |
| text `.map(fn)` | **CFlow** only for exact `line => line.length()`; otherwise **Legacy barrier** | Uses explicit typed-adapter projection with logical `LineSlice -> double`; string->string MAP remains legacy |
| text `.filter(fn)` | **CFlow** only for exact `line => line.length() > 0`; otherwise **Legacy barrier** | Uses explicit typed FILTER projection with logical `LineSlice -> bool`; Graph output remains `LineSlice` |

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

### Cross-statement intermediate streams

The first #74 slice also admits a graphable numeric non-terminal stream on
assignment:

```text
s = stream.of([1, 2, 3]).map(x => x * 2)
        |
        v
CFlow Graph / Plan (eager)
        |
        v
TurboScript.Stream.v1
  source = owned numeric vector
  __ts_cflow_materialized = 1
```

The envelope owns only materialized data. It carries no AST, MIR callable,
Graph, or Plan pointer, so statement boundaries do not extend compiler/runtime
object lifetimes. Existing `count/collect/toList/toVector` terminals can consume
that envelope, and pure/capture-free numeric `map/filter` operations on a
materialized envelope are re-admitted as a new CFlow source boundary. Each
assignment remains eager: the resumed Graph/Plan executes immediately and
produces another owned materialized envelope.

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

The Phase 2 text slice now covers both
`stream.text(...).lines().count/collect/toList` and
`stream.text(...).split(sep).count/collect/toList`, including the same
terminals when the text expression is `io.read_file(path)`. It preserves eager
source evaluation and legacy line/token splitting while routing typed borrowed
slices through a synchronous CFlow Plan. `collect/toList` copy each slice into
TurboScript-owned string storage before the CFlow result and source text are
released. For `split`, source and separator expressions are each evaluated
exactly once; empty separators remain byte-oriented and empty tokens are
preserved.

The first typed text callable slice is also live for the exact
`line => line.length()` MAP. Its contract is deliberately separate from the
numeric callable universe:

```text
logical CFlow type: TurboScript.LineSlice.v1 -> double
CMeta callable:     sig = INVALID, dispatch = ADAPTER
CFlow admission:    typed-adapter projection with explicit input/output descriptors
MIR ABI:            double kernel(const char *data, int64_t len)
```

The callable is PURE + DETERMINISTIC + TOTAL + NO_ALIAS. The MIR adapter copies
the LineSlice descriptor alignment-safely, passes pointer and length as their
native ABI classes, and never coerces a pointer through `double`. The
FunctionDesc/FunctionAbi pair is used as the control-plane semantic/ABI contract
for the typed adapter, which keeps plugin/native functions on the same canonical
CMeta admission route instead of creating a TurboScript-private function ABI.
Only the exact capture-free `line.length()` MAP shape is admitted in that slice.

The first typed text FILTER slice admits only
`line => line.length() > 0`:

```text
logical CFlow type: TurboScript.LineSlice.v1 -> bool
CMeta callable:     sig = INVALID, dispatch = ADAPTER
CFlow admission:    typed FILTER projection with explicit input descriptor
Graph output type:  TurboScript.LineSlice.v1
MIR ABI:            int64_t predicate(const char *data, int64_t len)
```

The erased adapter converts the nonzero integer predicate carrier to canonical
`_Bool`. CFlow retains FILTER cardinality and preserves the LineSlice element
type through direct execution, normalization/optimization, and compiled Plan
execution. Unsupported string predicate shapes remain legacy and are rejected
before the text source expression is evaluated.

Before implementing `stream.lines/csv/json/xml`, reconcile each surface with
actual factory/provider registration so the migration does not preserve a
documentation-only API by accident.

## Phase 3 Reactive rule

Reactive/backpressure execution is not implied by the current `stream.*`
syntax. It must be explicitly specified and qualified. Existing file/text
streams are eager today; migration to Publisher/Subscription semantics must not
silently change ordering, ownership, error, or materialization behavior.