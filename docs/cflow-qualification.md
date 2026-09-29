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
- 16,384 elements — large in-memory pipeline within the default ExprTk 100,000-node execution budget;
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

The first green CI run containing this benchmark is the baseline source. Exact
runner measurements are committed here before this qualification PR is merged.

Expected qualitative properties:

- `current_runtime` should expose the cost of rebuilding admission/MIR/Plan
  state on every call;
- `cached_plan` should demonstrate the benefit of reusing already-bound MIR
  kernels and a pre-decoded Plan;
- small pipelines may prefer legacy eager execution when one-shot lowering
  overhead dominates;
- medium/large repeated pipelines are the primary target for Plan caching;
- MAP + REDUCE must be measured separately because reduction changes
  cardinality and state semantics.

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
