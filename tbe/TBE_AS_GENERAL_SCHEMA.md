# TBE As A General Schema Format

Updated: 2026-06-27

## Summary

TBE can serve as a schema format for performance-sensitive data, but it is not a fully general data-modeling system like JSON Schema, Protobuf, or Avro.

Its best fit is still:

- low-latency binary messages
- deterministic fixed-prefix layouts
- generated type-safe accessors
- compact wire representation
- optional runtime parser generation through MIR

## Current Capabilities

TBE currently supports:

- `schema` metadata with version and byte order attributes
- `composite`, `group`, `message`, and `union`
- enum and flags declarations
- required and optional fields
- default values
- fixed arrays and fixed bytes
- repeating groups
- var-data fields such as `string`, variable `bytes`, and dynamic collection fields in the var-data section
- C wire-access generation
- C++ / Go / Rust / Python / TypeScript type generation
- RulesForge `.rfl` declaration output
- MIR / BMIR parser module output

## Deliberate Constraints

TBE requires message fields to be ordered by layout section:

1. fixed-size fields
2. groups
3. var-data fields

This is not an accidental parser limitation. It is the core layout contract that allows generated accessors to use compile-time offsets for the fixed prefix.

Examples:

```c
message Correct {
    uint32 id;
    group<Level> bids;
    string symbol;
}
```

```c
message Broken {
    string symbol;
    group<Level> bids;
}
```

The second form is rejected because a group cannot be located after a var-data field without runtime offset metadata.

## Language Output Model

### Full Wire API

C is the full zero-copy wire target. It emits view/builder APIs, accessors, enum helpers, flags helpers, optional/default helpers, group cursors, and union tag helpers.

### Type Definition Targets

C++, Go, Rust, Python, and TypeScript currently emit type declarations. They do not yet provide complete generated wire encode/decode APIs.

This split is intentional for now:

- C owns the stable low-level wire surface.
- Other languages get schema-aligned types for adapters, bindings, tests, and generated SDK layers.

## Comparison

| Feature | TBE | Protobuf | JSON Schema |
| --- | --- | --- | --- |
| Zero-copy fixed-prefix access | Strong | No | No |
| Compact binary wire format | Strong | Strong | No |
| Optional fields | Supported | Supported | Supported |
| Union-like tagged values | Supported | Supported | Schema-dependent |
| Flexible field ordering | Constrained | Supported | Supported |
| Runtime reflection | Limited | Strong | Strong |
| Human-readable payload | No | No | Yes |
| Full multi-language codecs | C complete, others type-only | Strong | Library-dependent |

## Recommended Use

Use TBE for:

- market data and trading messages
- game/network state packets
- IoT and embedded telemetry
- high-throughput internal streams
- binary protocol boundaries where schema and layout are controlled

Avoid TBE for:

- human-authored configuration
- public JSON-style APIs
- highly dynamic object graphs
- storage models requiring ad hoc queries
- schemas that require arbitrary field reordering without performance tradeoffs

## TurboScript Integration Direction

Recommended boundaries:

- Use TBE C generation for native high-performance codecs.
- Use MIR/BMIR output where runtime schema binding is needed.
- Use C++/Go/Rust/Python/TypeScript type outputs for adapters and typed client surfaces.
- Use RulesForge output when a TBE schema should describe data consumed by rules.

## Conclusion

TBE is a specialized high-performance schema and wire-layout system. It has grown beyond a minimal fixed-message format, but its value still comes from keeping layout constraints explicit instead of becoming a fully dynamic schema language.
