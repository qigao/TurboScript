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

The build consumes the latest published Salts, SaltsUtils, CHttp and TurboDB
native SDKs, with floating NuGet versions resolved on every restore. Third-party
libraries, including MIR, come from `vcpkg.json` and the shared
[vcpkg-cache](https://github.com/qigao/vcpkg-cache) toolchain. TurboScript does not
require Lua or QuickJS. CNet supplies HTTP TLS through GmSSL; this project no
longer installs BoringSSL.

The current MIR port supports Linux, macOS and Android. It explicitly rejects
Windows; the Windows presets retained in this repository cannot currently build
the latest source tree. Earlier Windows test results do not apply to this tree.

On Linux, install CMake 3.25+, Ninja, PowerShell 7, .NET SDK 8, Mono and the
native C/C++ toolchain. Set `GITHUB_TOKEN` with `read:packages` access,
`PROJECT_ROOT` and `VCPKG_ROOT`. Keep the shared cache checkout at
`$HOME/.cache/qigao/vcpkg-cache`. Credentials remain in the process environment.
In one PowerShell session:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid linux-x64 -Local
cmake --preset linux-release-user
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build --preset linux-release-user
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
cmake --build --preset install-linux-release-user
if ($LASTEXITCODE -ne 0) { throw 'Install failed' }
```

The restore script publishes `SALTS_ROOT`, `SALTS_UTILS_ROOT`, `CHTTP_ROOT`,
`TURBODB_ROOT` and `RE2C_ROOT` only after the complete SDK set is available.
A separately launched IDE or shell must receive these variables too. Package
payloads live under ignored `stage/nuget`; installation goes to
`$PROJECT_ROOT/external/pkgs/turbo_script/release`. Debug profiles require
explicitly selected matching SDK roots; published packages contain Release SDKs.

[Native CI](.github/workflows/native-sdk-release.yml) uses the
`ci-linux-release-user` preset for the full source suite and
`ci-linux-sanitizer-user` for the existing Host/CFlow sanitizer suite. Configure,
build and CTest use the same user preset. The CI environment supplies
`QIGAO_TARGET_TRIPLET` and `VCPKG_CACHE_REPOSITORY_ROOT`; both test switches are
enabled explicitly. Ordinary Release profiles retain their own test settings.
The CFlow qualification benchmark runs through `ci-linux-benchmark-user` in CTest.

PR checks also build and install the native SDKs with `ci-linux-sdk-user`,
`ci-macos-sdk-user` and `ci-android-sdk-user`; each has an `install-` build preset.
All platforms use the same floating SDK restore script. Android uses a native
`ci-host-tools-user` build for Lemon and the host re2c package. TurboDB is included
on Linux and Android; its published SDK has no macOS slice, so the existing
macOS package continues to omit the database module.

The existing installed Host ABI and interpreter/JIT CTest suites have their own
versioned user presets under `cmake/ci/host-abi` and `cmake/ci/sdk-smoke`. They
reuse the root vcpkg manifest and run against the installed/relocated SDK.
Android consumers are cross-linked; no device runtime test is claimed.


Native integrations use the current `cmeta_*`, `coro_executor_*` and
`cmeta_plugin_*` APIs. Plugin macros are `CMETA_PLUGIN_*`, and the query entry
is `cmeta_plugin_query`; the header remains `<salts/plugin.h>`. Rebuild native
plugins against the selected SDK's current ABI before loading them. Script
function names and behavior are unchanged.

Run a script with:

```text
turbo_script_repl --file hello.tbs
```

See [Getting Started](docs/getting-started.md), the
[language guide](docs/language-guide.md), the
[C API](turbo_script/include/turbo_script.h), and the
[plugin system](docs/PLUGIN_SYSTEM.md).

## License

TurboScript first-party code is licensed under the Apache License 2.0. See
[LICENSE](LICENSE). Bundled and vendored third-party components retain their
respective upstream licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
