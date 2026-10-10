# Release notes draft: CMeta and Salts Plugin migration

Status: preparation branch only; not a release candidate for publication.
The user requested that upstream integration be deferred.

| Item | Revision |
| --- | --- |
| Branch | `work/cmeta-plugin-migration` |
| Tested implementation | `6bbaefe64c04cb06c5eb149ea491dec07f494414` |
| Local starting point | `5e190f4` (project version 3.0.0) |
| Upstream inspected | `b0aaa17cc0d00e000c46a83e358da74a61992ebf` |
| Latest published version inspected | [3.0.8](https://github.com/qigao/TurboScript/releases/tag/v3.0.8) |
| Intended release version | Unassigned until upstream reconciliation |

No release tag, package, or published GitHub Release has been created for this
branch. The 3.0.0 project version records its starting point; it must not be used
to publish these changes over an existing version.

## Changes in this branch

- Generate native value tags, direct payload constructors and typed-array storage
  declarations from CMeta Schema/Replay. Existing tag numbers, value layout and
  script conversion policies remain unchanged.
- Share checked typed-array byte-size calculation between creation and copying,
  rejecting multiplication overflow before allocation.
- Publish all 11 built-in native plugins through Salts Plugin manifests and
  exact CMeta `open`/`close` Function adapters. Host admission checks module
  contract and native signature compatibility.
- Hold a registry lease across module registration, script calls and instance
  cleanup. Failed close retains the instance and lease for explicit retry.
- Preserve unfinished HTTP/WebSocket client owners when drain/destroy fails.
  Successfully cleaned clients are cleared and are not destroyed twice.
- Update native SDK integration, plugin documentation and formal regression tests.

## Compatibility and migration

The branch uses TurboScript module contract 2 (`qigao.turboscript.module`) and
Salts Plugin publication ABI 5. These versions are separate from package versions
and the Host API version. Every plugin must be rebuilt with matching headers and
libraries. The retired `ts_api_create` publication is rejected.

Script imports and `http.*`/`ws.*` names are unchanged. The net module continues
to use `CHttp::Client`; this change does not introduce a new network progress
owner or background thread.

Applications using the loader directly can call `ts_plugin_unload_ex()` to retain
and retry an unfinished close. The void `ts_plugin_unload()` wrapper and context
destruction cannot return a retry obligation and fail fast on cleanup failure.
This behavior must be included in eventual release migration guidance.

The tested SDK combination is Salts 2.3.0-rc.4, SaltsUtils 4.3.0-rc.2 and
CHttp 2.1.0-rc.1. The local branch does not add TurboDB, TurboWasm or SaltsNet as
required dependencies. Upstream has since added capabilities and dependencies
that must be preserved when this branch is reconciled.

## Verification

Windows x64/MSVC Release full build succeeded with the specified official
release SDKs. Full CTest passed **60/60**; the focused schema/plugin/net suite
passed **4/4**. The commands were:

```text
cmake --preset ci-win-release-user
cmake --build --preset ci-win-release-user
ctest --preset ci-win-release-user --output-on-failure
```

The build used a Visual Studio developer environment and the matching SDK roots.
Formal tests cover C11/C++17 headers, all 11 real plugin DLLs in two contexts,
re-import, continued use after another context closes, incompatible admission,
initialization failure and close failure with lease retention/retry.

These results apply to the implementation commit above, not to an integrated
upstream release. Linux/macOS/mobile, Debug/sanitizers, installed-package
qualification and public-network HTTPS/WebSocket tests were not run. CHttp drain
timeout was not fault-injected; retry behavior was tested with a loader fixture.
NuGet restoration returned 403/NU1301 with the available local credential, so
the successful release-asset SDK build does not qualify the NuGet restore path.

## Deferred integration and release work

The local starting point is 326 commits behind the inspected upstream master.
A non-mutating merge audit found conflicts in build configuration, native SDK
restoration, value declarations, plugin publication/loading, tests and docs.
Upstream already contains a different Salts Plugin adapter, canonical Function
binding, DataBind integration and managed value lifecycle work. Its SQLite module
layout also differs. Replacing those files wholesale would discard upstream
capabilities; the branch must be reconciled semantically before release.

The eventual release must use upstream's existing native SDK release workflow,
preserve its platform/package qualification, and align CMake, vcpkg, public-header
and NuGet versions. Choose a new version reflecting the reconciled compatibility
impact, rerun qualification on that exact commit, then tag and publish only when
separately requested. Rollback requires a matching host/plugin/SDK artifact set.

See [the plugin contract](../PLUGIN_SYSTEM.md) and
[the architecture decision](../superpowers/specs/2026-08-25-turboscript-host-module-abi-design.md#46-全部内置插件迁移至-salts-plugin).
