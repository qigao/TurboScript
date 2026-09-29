# CFlow Qualification Gate

This document is the release gate for TurboScript issue #19 and the final
cleanup gate for #18.

## What is being qualified

TurboScript keeps the `stream.*` language facade, but graphable regions are
executed by:

```text
TurboScript AST
  -> CMeta callable/type admission
  -> CFlow Surface Graph
  -> normalize / optimize
  -> compiled CFlow Plan
  -> MIR scalar kernels inside admitted nodes
```

The qualification gate deliberately separates semantic evidence from timing
telemetry. Correctness is gating; wall-clock ratios are recorded and reviewed,
but are not hard-coded into CI because hosted runner noise is not a stable API.

## Deterministic semantic and structural gate

`test_turbo_script_cflow_qualification` records a representative real
TurboScript pipeline:

```javascript
stream.of(values)
  .map(x => x + 1)
  .map(x => x * 2)
  .count()
```

The stable structural baseline is:

```text
Surface Graph:
  INPUT -> MAP -> MAP
  nodes = 3

Normalized Graph:
  INPUT -> MAP -> MAP

Optimized Graph:
  INPUT -> MAP(fn_chain = 2)
  nodes = 2
  map_nodes_fused = 1
```

The test then proves:

- the legacy ExprTk terminal returns the same result count;
- normalized direct execution and optimized compiled Plan return identical
  values;
- the optimized Plan records both MAP callbacks;
- one compiled Plan can be executed repeatedly without rebuilding MIR kernel
  bindings.

This is structural evidence that CFlow is not merely an adapter around the
legacy stream evaluator.

## Lifetime / sanitizer gate

The existing Linux Host sanitizer job runs:

- `test_turbo_script_cflow_lower`;
- `test_turbo_script_cflow_mir_kernel`;
- `test_turbo_script_cflow_qualification`.

ASan, UBSan and leak detection therefore cover:

- CMeta callable captures;
- MIR kernel ownership;
- Graph -> optimized Graph cloning;
- compiled Plan lifetime;
- repeated Plan results;
- teardown ordering between Plan, Graph, kernel binding and TurboScript context.

## Performance telemetry

`bench_turbo_script_cflow` is executed explicitly by the existing
`Linux source / full CTest` CI job so successful benchmark output remains
visible in the job log.

For the same already-parsed `filter -> map -> count` AST it records three
paths:

1. `legacy_eager` — direct ExprTk stream evaluator;
2. `current_runtime` — current TurboScript CFlow cutover, including
   admission/lowering/MIR binding/Plan compilation on each call;
3. `cached_plan` — lower/bind/compile once, then repeatedly execute the
   pre-decoded CFlow Plan.

The benchmark covers:

- 16 elements — fixed lowering/compilation overhead dominates;
- 4,096 elements — medium in-memory pipeline;
- 8,192 elements — large in-memory pipeline within the default ExprTk 100,000-node execution budget;
- 4,096-element MAP + seeded REDUCE.

Each run emits machine-readable lines:

```text
CFLOW_BENCH scenario=... elements=... path=... us_per_eval=... iterations=...
CFLOW_PLAN  scenario=... elements=... graph_nodes=... instructions=...
            map_callbacks=... runtime_over_cached=... legacy_over_cached=...
```

These numbers are telemetry, not a brittle hosted-runner pass/fail threshold.
A material regression must be explained before changing the documented
baseline.

## Performance baseline

The first green qualification telemetry is GitHub Actions run #169 on
2026-09-29 (Ubuntu 24.04 hosted runner). These measurements are machine-specific
and remain non-gating; correctness and graph structure are the stable gates.

```text
CFLOW_BENCH scenario=filter_map_count elements=16 path=legacy_eager us_per_eval=62.927 iterations=300
CFLOW_BENCH scenario=filter_map_count elements=16 path=current_runtime us_per_eval=1481.500 iterations=12
CFLOW_BENCH scenario=filter_map_count elements=16 path=cached_plan us_per_eval=0.415 iterations=2000
CFLOW_PLAN scenario=filter_map_count elements=16 graph_nodes=3 instructions=2 map_callbacks=1 inference_queries=2 runtime_over_cached=3565.584 legacy_over_cached=151.448

CFLOW_BENCH scenario=filter_map_count elements=4096 path=legacy_eager us_per_eval=12279.033 iterations=30
CFLOW_BENCH scenario=filter_map_count elements=4096 path=current_runtime us_per_eval=1613.167 iterations=6
CFLOW_BENCH scenario=filter_map_count elements=4096 path=cached_plan us_per_eval=76.644 iterations=160
CFLOW_PLAN scenario=filter_map_count elements=4096 graph_nodes=3 instructions=2 map_callbacks=1 inference_queries=2 runtime_over_cached=21.048 legacy_over_cached=160.209

CFLOW_BENCH scenario=filter_map_count elements=8192 path=legacy_eager us_per_eval=24993.188 iterations=16
CFLOW_BENCH scenario=filter_map_count elements=8192 path=current_runtime us_per_eval=1730.750 iterations=4
CFLOW_BENCH scenario=filter_map_count elements=8192 path=cached_plan us_per_eval=153.483 iterations=120
CFLOW_PLAN scenario=filter_map_count elements=8192 graph_nodes=3 instructions=2 map_callbacks=1 inference_queries=2 runtime_over_cached=11.276 legacy_over_cached=162.840

CFLOW_BENCH scenario=map_reduce elements=4096 path=legacy_eager us_per_eval=18845.650 iterations=20
CFLOW_BENCH scenario=map_reduce elements=4096 path=current_runtime us_per_eval=1581.000 iterations=6
CFLOW_BENCH scenario=map_reduce elements=4096 path=cached_plan us_per_eval=111.850 iterations=120
CFLOW_PLAN scenario=map_reduce elements=4096 graph_nodes=3 instructions=2 map_callbacks=1 runtime_over_cached=14.135 legacy_over_cached=168.490

CFLOW_FILE_BASELINE scenario=read_file_lines_count source_lines=4096 path=legacy_eager us_per_eval=1786.375 iterations=8
CFLOW_FILE_BASELINE scenario=read_file_lines_count source_lines=4096 path=current_runtime us_per_eval=97.875 iterations=8 runtime_over_legacy=0.055
```

The baseline demonstrates the intended separation: one-shot runtime cutover
includes lowering/binding/Plan construction, while cached Plan execution
measures reuse of already-bound kernels. The file/text case remains an eager
ordering baseline and is not evidence for reactive/backpressure semantics.

## Rollout and fallback policy

There is **no silent post-admission fallback**.

```text
before CFlow admission:
    unsupported shape/effect/type -> legacy facade may own execution

after CFlow admission:
    lowering/kernel/Plan/runtime failure -> explicit TurboScript error
    never retry through legacy evaluator
```

This matters for ownership, side effects, diagnostics and exactly-once source
evaluation. Running the same admitted syntax twice through different runtimes
would be semantically unsafe.

Rollback is therefore a control-plane change:

- narrow or disable the relevant admission rule;
- keep the unsupported shape on the legacy path before source evaluation;
- do not catch a CFlow execution failure and retry it in ExprTk.

Legacy execution code is removed only after:

- parity tests are green;
- sanitizer coverage is green;
- optimized structure is verified;
- benchmark telemetry is reviewed;
- unsupported-shape diagnostics are stable.

## File/text baseline before Reactive work

Current file/text migration remains eager by design. Verified paths such as:

```javascript
stream.text(io.read_file(path)).lines().count()
stream.text(io.read_file(path)).lines().filter(...).toList()
```

perform file I/O and source materialization before synchronous CFlow Plan
execution. Reactive/backpressure migration is a separate opt-in phase and must
not use this benchmark gate as permission to silently change file ordering,
ownership or error semantics.
