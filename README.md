# TurboScript

TurboScript is an embeddable scripting engine for C/C++ applications. It uses
a TypeScript/JavaScript-like syntax and a shared MIR lowering pipeline for
interpreted and native-code execution.

The current practical focus is host-driven automation and data processing:

- math, statistics, vectors, tables, strings, files and date/time helpers;
- cooperative tasks and timers;
- HTTP/1, HTTP/2 and WebSocket clients;
- technical analysis, strategy, risk and time-series modules;
- SQLite, structured-data mapping, cryptography, rules and OS integration;
- a C API for compiling scripts, binding host values and registering native
  functions.

TurboScript is not a TypeScript implementation or a security sandbox. Native
plugins execute with the host process's permissions, so only trusted scripts
and plugins should be loaded. Embedders can reject unapproved native plugin
names before DLL loading with `turbo_script_init_with_plugin_authorizer()`; this
is admission control, not process isolation.

## Build and test

The build expects installed Salts and SaltsUtils packages plus the vcpkg
dependencies declared in `vcpkg.json`. Configure their
locations in a local `CMakeUserPresets.json`, then use the repository presets:

```text
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user --output-on-failure
```

Run a script with:

```text
turbo_script_repl --file hello.tbs
```

See [Getting Started](docs/getting-started.md), the
[language guide](docs/language-guide.md), the
[C API](turbo_script/include/turbo_script.h), and the
[plugin system](docs/PLUGIN_SYSTEM.md).
