# TurboScript

**A high-performance embeddable scripting engine with TypeScript-like syntax, a unified MIR execution backend, MIR interpreter/JIT modes, and an extensible plugin system.**

TurboScript is designed for host applications that need fast, flexible scripting without embedding a full JavaScript, Python, or Lua runtime. It provides a TypeScript/JavaScript-like syntax, one MIR lowering pipeline shared by `turbo_script_run()` and `turbo_script_run_jit()`, and built-in modules for data binding, text processing, math, matrix/linear algebra, time series, technical analysis, and finance.

It is not a full TypeScript implementation and does not try to replace Pandas/NumPy. The intended scope is embedded automation, data transformation, lightweight data analysis, quantitative indicators, and host-driven extension.

---

## 🚀 Quick Links

### For Users
- **[Getting Started](getting-started.md)** - 5-minute tutorial to write your first script
- **[Language Guide](language-guide.md)** - Complete syntax reference and language features
- **[API Reference](api-reference.md)** - Built-in functions and standard library

### For Developers
- **[Plugin Development](plugin-development.md)** - Extend TurboScript with C/C++ plugins
- **[Module Documentation](modules/)** - Domain-specific module guides
- **[Architecture](advanced/architecture.md)** - Internal design and implementation
- **[DSL Charter](DSL_CHARTER.md)** - Language scope, semantic contracts, and roadmap
- **[DSL Execution Plan](DSL_EXECUTION_PLAN.md)** - Prioritized backlog and parser/evaluator convergence plan

### Quick Reference
- **[Math Cheatsheet](math_cheatsheet.md)** - Mathematical functions
- **[Vector Operations](vec_cheatsheet.md)** - Vector/array operations
- **[Technical Analysis](ta_fin_cheatsheet.md)** - TA indicators and finance functions

---

## ✨ Key Features

### Modern Language Design
- **TypeScript/JavaScript-like syntax** for familiar embedded scripting
- **Dynamic typing** with type introspection (`typeof`, `is_number`, etc.)
- **Arrow functions** and closures: `(x) => x * 2`
- **Destructuring assignment**: `let [a, b] = [10, 20]`
- **Pipe operator**: `data |> filter(x > 0) |> sum()`
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
- **Parser/Data bind**: CSV, JSON, XML, TBE schema bind/emit/validate with plain object results
- **Time series / TA / finance**: Rolling windows, indicators, risk metrics, portfolio helpers
- **File I/O**: Read/write files, directory operations
- **Date/Time**: Parsing, formatting, timestamps

### Extensible Architecture
- **Plugin system**: Load C/C++ DLLs dynamically via `import("plugin_name")`
- **Module-based**: Clean namespace separation (`csv.*`, `json.*`, `ta.*`, `strategy.*`)
- **Unified MIR backend**: `turbo_script_run()` executes generated MIR with MIR's interpreter; `turbo_script_run_jit()` uses the same IR and asks MIR to generate native code
- **Pure MIR execution contract**: unsupported lowering forms report errors from the MIR pipeline
- **Optimized hot paths**: direct math calls, pre-bound vector access, map numeric pointers, monomorphic OOP method lowering, `super.method(...)` inline, and public numeric instance field slot access
- **Zero-copy FFI**: Efficient data exchange with host applications

---

## 📦 Installation

### From Source
```bash
git clone https://github.com/your-org/turbonet.git
cd turbonet/tScript
mkdir build && cd build
cmake ..
make
```

### Using Pre-built Binaries
Download the latest release from [Releases](https://github.com/your-org/turbonet/releases).

---

## 🎯 Quick Example

```javascript
// Load plugins
import("csv");
import("ta");

// Read and process data
var data = csv.read("prices.csv");
var close = csv.col(data, "close");

// Calculate technical indicators
var sma20 = ta.sma(close, 20);
var rsi14 = ta.rsi(close, 14);

// Generate signals
var signal = (rsi14 < 30) ? "BUY" : (rsi14 > 70) ? "SELL" : "HOLD";

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
| `parser` / `csv` / `json` | CSV parsing, JSON querying, TBE schema bind/emit/validate | [parser README](../modules/parser/README.md) |
| `ta` | Technical analysis indicators | [ta_fin_cheatsheet.md](ta_fin_cheatsheet.md) |
| `fin` / `strategy` | Risk metrics, strategy context, portfolio helpers | [ta_fin_cheatsheet.md](ta_fin_cheatsheet.md) |
| `vec` | Advanced vector operations | [vec_cheatsheet.md](vec_cheatsheet.md) |
| `net` | HTTP/WebSocket networking | [modules/net.md](modules/net.md) |
| `sqlite` | Database access | [modules/sqlite.md](modules/sqlite.md) |
| `data_bind` | Binary parsing using JIT-compiled TBE schemas | [modules/data_bind/README.md](../modules/data_bind/README.md) |
| `wasm` | WebAssembly execution | [../../modules/wasm/README.md](../../modules/wasm/README.md) |

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

### Contributing
See [CONTRIBUTING.md](../CONTRIBUTING.md) for guidelines.

---

## 📄 License

[Your License Here]

---

## 🤝 Community

- **Issues**: [GitHub Issues](https://github.com/your-org/turbonet/issues)
- **Discussions**: [GitHub Discussions](https://github.com/your-org/turbonet/discussions)
- **Discord**: [Join our server](https://discord.gg/your-invite)

---

**Built with ❤️ for high-performance scripting**
