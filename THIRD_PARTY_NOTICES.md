# Third-Party Notices

TurboScript first-party code is licensed under the Apache License 2.0.
The components below are not relicensed by the TurboScript project; their
upstream license terms continue to apply.

| Component | Repository path | Upstream license | Notes |
| --- | --- | --- | --- |
| CRoaring | `vendor/croar/` | Apache-2.0 OR MIT | Both license notices are embedded in the amalgamated source. |
| MIR | `vendor/mir/` | MIT | See `vendor/mir/LICENSE`. |
| TRE | `vendor/tre/` | BSD-2-Clause | See `vendor/tre/LICENSE`. |
| libecc | `vendor/turbo_crypto/libecc/` | BSD-2-Clause OR GPL-2.0-or-later | TurboScript selects the BSD license for redistribution. See `vendor/turbo_crypto/libecc/LICENSE`. |
| Monocypher | `vendor/turbo_crypto/monocypher/` | BSD-2-Clause OR CC0-1.0 | License notice is embedded in the upstream source headers. |
| sha-2 (SHA-256) | `vendor/turbo_crypto/sha2/` | Unlicense OR 0BSD | Derived from `amosnier/sha-2`; upstream license copied to `vendor/turbo_crypto/sha2/LICENSE.md`. |
| xxtea-c | `vendor/turbo_crypto/xxtea/` | MIT | See `vendor/turbo_crypto/xxtea/LICENSE.md`. |
| SQLite Lemon parser generator | `tools/lemon/` | Public-domain dedication | The source headers explicitly disclaim copyright. |

External dependencies resolved through vcpkg or system package managers are
not bundled as TurboScript source and remain governed by their own licenses.

The presence of a third-party component in this repository does not change
the Apache-2.0 license of TurboScript first-party code.
