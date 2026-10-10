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

The SDK restore baseline is Salts **2.3.0-rc.4**, SaltsUtils **4.3.0-rc.2** and
CHttp **2.1.0-rc.1**, plus the dependencies in `vcpkg.json`. The versioned
`CMakeUserPresets.json` is the entry point for local builds and CI. The restore
script selects these exact native package versions; old CMake package locations
cannot override the selected SDK roots. Use matching RID/profile SDKs together.
TLS is supplied by `Salts::CNet` with its GmSSL backend; TurboScript does not
install or link a separate BoringSSL/OpenSSL implementation.
TurboScript uses its own MIR runtime and does not require Lua or QuickJS.
SaltsUtils supplies the data binding, parsing, templating and scheduling modules.
ExprTk uses CMeta Schema/Replay for ABI-stable value declarations and constructors;
runtime ownership and script conversion policy remain explicit. See the
[CMeta design and integration boundaries](docs/superpowers/specs/2026-08-25-turboscript-host-module-abi-design.md#41-cmeta-声明层).
TurboWasm, TurboDB and SaltsNet are optional future integration candidates, not
required dependencies of this build.

All native modules now publish through Salts Plugin (TurboScript module contract
2). Rebuild every plugin with the matching SDK; legacy `ts_api_create` binaries
are rejected. Script imports are unchanged. See the
[plugin publication and lifetime contract](docs/PLUGIN_SYSTEM.md).
This branch's [release preparation notes](docs/releases/cmeta-plugin-migration.md)
record validation and the deferred reconciliation with upstream 3.0.8.

Prerequisites: CMake 3.25 or newer (preset schema 6), Ninja, PowerShell 7,
.NET SDK 8, a vcpkg checkout, and `GITHUB_TOKEN` with `read:packages` access
to the qigao packages. Set `PROJECT_ROOT` to your development package parent
and `VCPKG_ROOT` to the vcpkg checkout. The shared
[vcpkg-cache](https://github.com/qigao/vcpkg-cache) checkout must exist at
`%LOCALAPPDATA%/qigao/vcpkg-cache` on Windows or
`$HOME/.cache/qigao/vcpkg-cache` on Linux; Linux also needs Mono for NuGet
binary-cache restoration. The shared feed is read-only and the local binary
cache is writable. Credentials stay in the parent process environment.

Restore the SDKs in PowerShell, then build in the same process environment:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Local
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw 'Visual Studio C++ toolchain not found' }
$env:QIGAO_VCPKG_ROOT = $env:VCPKG_ROOT
cmd /c "call `"$vsInstall/Common7/Tools/VsDevCmd.bat`" -arch=x64 -host_arch=x64 >nul && set `"VCPKG_ROOT=%QIGAO_VCPKG_ROOT%`" && cmake --preset win-release-user && cmake --build --preset win-release-user && ctest --preset win-release-user --output-on-failure"
if ($LASTEXITCODE -ne 0) { throw 'Build or tests failed' }
```

On Linux, run the same restore script from `pwsh` with `-Rid linux-x64 -Local`,
then use `linux-release-user` for configure, build and CTest in that session.
The restore script sets `SALTS_ROOT`, `SALTS_UTILS_ROOT`, `CHTTP_ROOT` and
`RE2C_ROOT` only after all packages have been restored. These variables must
also be supplied to any separately launched IDE or shell. Debug development
presets consume explicitly supplied matching SDK roots through the same
variables; restoring the published packages selects Release SDKs.

SDK downloads live under ignored `stage/nuget`; build trees remain under
`build`. Installation uses `install-win-release-user` or
`install-linux-release-user` and writes to
`$PROJECT_ROOT/external/pkgs/turbo_script/release`. Missing SDKs, invalid
credentials and unsupported dependencies stop the build rather than selecting
an older installation. Android profiles still require separately supplied
matching Android SDK installations and host re2c; native restoration and CI
currently cover Windows x64 and Linux x64.

[Native CI](.github/workflows/native-build.yml) restores the selected SDK baseline on
each run, configures and builds the complete Release graph, and runs the
existing tests through CTest. It uses the same manifest and shared binary
cache as local builds. The CI presets explicitly enable both `BUILD_TESTS`
and `ENABLE_TESTS` for the full regression suite; ordinary Release presets
follow their own test settings and do not include every test.

Native integrations use the current Salts `cmeta_*` utility APIs and
`coro_executor_*` executor API. Rebuild host applications and native plugins
against the selected SDK headers. Script function names and behavior are unchanged.

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
