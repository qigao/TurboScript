# Vendored xxtea-c

- Source: https://github.com/xxtea/xxtea-c
- Commit: `7ec961540996934d939572d885ea1d5b21689688`
- Commit date: 2020-06-15
- License: MIT; see `LICENSE.md`.

The source snapshot is from upstream `master`. TurboNet carries two local
integration changes: `CMakeLists.txt` is adapted for use below the repository's
vendor tree, and `xxtea.c` checks its output allocation before writing to it.
`../CMakeLists.txt` compiles only `xxtea.c` into `TurboNet::Crypto`; upstream
examples and standalone shared/static targets are not part of the TurboNet
build.
