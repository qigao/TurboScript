# tbe_compiler Command Line Options

## Overview

`tbe_compiler` generates code and DSL declarations from TBE schema files for multiple target languages and RulesForge integration.

## Basic Usage

```bash
tbe_compiler <schema_file> [options]
```

## Options

### Required

- `<schema_file>`
  - Path to the `.schema` definition file
  - Example: `order.schema`

### Language Output

- `--output <file>` or `-o <file>`
  - Output file path for generated code
  - Default: stdout
  - Example: `--output order.h`

- `--lang <language>` or `-l <language>`
  - Target language or module artifact format
  - Options: `c`, `cpp`, `go`, `rust`, `python`, `py`, `ts`, `typescript`, `mir`, `bmir`
  - Default: `c`
  - Example: `--lang c`

- `--template <file>` or `-t <file>`
  - Path to custom Mustache template file
  - Overrides `--lang` option
  - Example: `--template my_template.mustache`

### DSL Integration (RulesForge)

- `--dsl-output <file>` or `-d <file>`
  - Generate DSL type declarations (.rfl file)
  - Contains `declare` statements for use in RulesForge
  - Example: `--dsl-output order.rfl`

## Usage Examples

### Example 1: Generate C Header Only

```bash
tbe_compiler order.schema --output order.h
```

### Example 2: Generate DSL Type Declarations

```bash
tbe_compiler order.schema --dsl-output order.rfl
```

Output `order.rfl`:
```rfl
package OrderSchema

declare Order
    id: int
    total: double
    tier: String
end
```

### Example 3: Generate Both

```bash
tbe_compiler order.schema --output order.h --dsl-output order.rfl
```

### Example 4: Generate C++ Types

```bash
tbe_compiler order.schema --lang cpp --output order.hpp
```

### Example 5: Generate Go Types

```bash
tbe_compiler order.schema --lang go --output order.go
```

### Example 6: Generate TypeScript Types

```bash
tbe_compiler order.schema --lang ts --output order.ts
```

### Example 7: Generate Rust Types

```bash
tbe_compiler order.schema --lang rust --output order.rs
```

### Example 8: Generate Python Types

```bash
tbe_compiler order.schema --lang py --output order.py
```

### Example 9: Generate MIR IR Module

```bash
tbe_compiler order.schema --lang mir --output order.mir
```

### Example 10: Generate Binary MIR Module

```bash
tbe_compiler order.schema --lang bmir --output order.bmir
```

## Removed Options

- `--codec-project` (REMOVED)
  - Codec DLL project generation is no longer supported by the compiler directly.
- `--rfl-output` (REPLACED)
  - Replaced by `--dsl-output`.

## Notes

- `--dsl-output` uses `templates/rfl_types.mustache` by default.
- DSL output is intended for use in RulesForge to define the structure of data being processed.
- C output is the complete wire-access target with generated view/builder APIs.
- C++, Go, Rust, Python, and TypeScript outputs currently generate schema type definitions, not complete wire codecs.
- MIR outputs are loadable modules, not standalone executables. The host must bind runtime callbacks such as `create_obj`, `set_int`, `set_dbl`, `set_str`, and `read_varstr`, then link or JIT the module before calling generated functions such as `parse_Order`.
