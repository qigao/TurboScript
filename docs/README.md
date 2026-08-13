# TurboScript

**A high-performance embeddable scripting engine with TypeScript-like syntax, a unified MIR execution backend, MIR interpreter/JIT modes, and an extensible plugin system.**

TurboScript is designed for host applications that need fast, flexible scripting without embedding a full JavaScript, Python, or Lua runtime. It provides a TypeScript/JavaScript-like syntax, one MIR lowering pipeline shared by `turbo_script_run()` and `turbo_script_run_jit()`, and built-in modules for data binding, text processing, math, matrix/linear algebra, time series, technical analysis, and finance.

It is not a full TypeScript implementation and does not try to replace Pandas/NumPy. The intended scope is embedded automation, data transformation, lightweight data analysis, quantitative indicators, and host-driven extension.

---

## 🚀 Quick Links

### For Users
- **[Getting Started](getting-started.md)** - 5-minute tutorial to write your first script
- **[Language Guide](language-guide.md)** - Complete syntax reference and language features
- **[API Reference](api/api-reference.md)** - Built-in functions and standard library

### For Developers
- **[Plugin Development](PLUGIN_SYSTEM.md)** - Extend TurboScript with C/C++ plugins
- **[Module Documentation](modules/)** - Domain-specific module guides
- **[Architecture](advanced/architecture.md)** - Internal design and implementation
- **[DSL Charter](DSL_CHARTER.md)** - Language scope, semantic contracts, and roadmap

### Quick Reference
- **[Math Cheatsheet](api/math_cheatsheet.md)** - Mathematical functions
- **[Vector Operations](api/vec_cheatsheet.md)** - Vector/array operations
- **[Technical Analysis](api/ta_fin_cheatsheet.md)** - TA indicators and finance functions

---

## ✨ Key Features

### Modern Language Design
- **TypeScript/JavaScript-like syntax** for familiar embedded scripting
- **Dynamic typing** with type introspection (`typeof`, `is_number`, etc.)
- **Arrow functions** and closures: `(x) => x * 2`
- **Destructuring assignment**: `let [a, b] = [10, 20]`
- **Pipe operator**: `data |> filter(x => x > 0) |> sum()`
- **Optional chaining**: `user?.address?.city`
- **OOP support**: `class`, `interface`, `extends`, `implements`, `super`

### Rich Data Types
- **Numbers**: 64-bit floating point
- **Strings**: UTF-8 with template literals
- **Vectors**: Efficient numeric arrays
- **Objects**: Plain records returned by parser/data binding APIs
- **Maps**: Hash-based key-value stores
- **Lists**: Heterogeneous collections

### Built-in Libraries
- **Math & matrix**: Scalar math, statistics, dense matrix helpers, linear algebra
- **String**: UTF-8 text operations, parsing, formatting, templates
- **Parser/Mapper**: configuration text parsing plus class-first JSON/YAML/XML mapping
- **Time series / TA / finance**: Rolling windows, indicators, risk metrics, portfolio helpers
- **File I/O**: Read/write files, directory operations
- **Date/Time**: Parsing, formatting, timestamps

### Extensible Architecture
- **Plugin system**: Load C/C++ DLLs dynamically via `import("plugin_name")`
- **Module-based**: Clean namespace separation (`parser.*`, `mapper.*`, `ta.*`, `strategy.*`)
- **Unified MIR backend**: `turbo_script_run()` executes generated MIR with MIR's interpreter; `turbo_script_run_jit()` uses the same IR and asks MIR to generate native code
- **Pure MIR execution contract**: unsupported lowering forms report errors from the MIR pipeline
- **Optimized hot paths**: direct math calls, pre-bound vector access, map numeric pointers, monomorphic OOP method lowering, `super.method(...)` inline, and public numeric instance field slot access
- **Typed host API**: Bind scalars, strings and vectors and register native callbacks

---

## 📦 Installation

The source build requires installed TurboUtils, TurboNet, TurboHTTP and
RulesForge packages plus the dependencies in `vcpkg.json`. Configure their
locations in a local `CMakeUserPresets.json`, then follow the commands in the
[repository README](../README.md#build-and-test).

---

## 🎯 Quick Example

```javascript
import("ta");

var close = [100, 101, 102, 101, 103, 105, 104, 106, 108, 107,
             109, 111, 110, 112, 114, 113, 115, 117, 116, 118];

// Calculate technical indicators
var sma20 = ta.sma(close, 20);
var rsi14 = ta.rsi(close, 14);

// Generate signals
var latest_rsi = rsi14[len(rsi14) - 1];
var signal = (latest_rsi < 30) ? "BUY" : (latest_rsi > 70) ? "SELL" : "HOLD";

print("Signal: " + signal);
```

---

## 🌍 Language Support

- **English**: Primary documentation (this directory)
- **中文**: [Chinese translation](zh/README.md)

---

## 📚 Module Ecosystem

TurboScript comes with a rich set of optional modules:

| Module | Description | Documentation |
|--------|-------------|---------------|
| `parser` | Configuration-oriented text parsing (INI, dotenv, TOML, command line) | [parser README](../modules/parser/README.md) |
| `mapper` | Class-first JSON/YAML/XML serialization through `turbo_parser.h` | [mapper README](../modules/mapper/README.md) |
| `ta` | Technical analysis indicators (`ta.*`) | [TA/finance cheatsheet](api/ta_fin_cheatsheet.md) |
| `fin` | Risk metrics and strategy helpers (`strategy.*`) | [TA/finance cheatsheet](api/ta_fin_cheatsheet.md) |
| `ts` | Time-series helpers (`ts.*`) | [module source](../modules/ts/) |
| built-in vectors | Vector operations; no import required | [vector cheatsheet](api/vec_cheatsheet.md) |
| `net` | HTTP/1, HTTP/2 and WebSocket clients | [task/network guide](advanced/tasks.md) |
| `sqlite` | SQL, embeddings and local RAG | [SQLite README](../modules/sqlite/README.md) |
| `img` | Image handles and pixel/image operations | [image README](../modules/img/README.md) |
| `crypto`, `hash`, `fuzzy` | Cryptography, hashing and fuzzy matching | [API reference](api/api-reference.md) |
| `rules_forge` | Rules engine integration and TurboScript RHS plugin loading | [rules_forge README](../modules/rules_forge/README.md) |
| `os` | Platform information, shell-free child processes, logging, service and power management | [os README](../modules/os/README.md) |
| `wasm` | Experimental source present, not enabled by the default build | [integration plan](../modules/wasm/WASI_INTEGRATION_PLAN.md) |

---

## 🛠️ Development

### Project Structure
```
tScript/
├── exprtk/          # Core parser, interpreter, built-in modules
├── turbo_script/    # Host API, MIR JIT, script runtime
├── modules/         # Built-in and plugin modules
├── docs/            # Documentation (you are here)
└── tests/           # Test suite
```

This checkout does not currently include repository-level licensing metadata;
resolve licensing before redistributing source or binaries.
