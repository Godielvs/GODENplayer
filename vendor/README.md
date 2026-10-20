# Vendor policy

Third-party source is pinned by manifest or Git submodule and is never copied
from an installed component directory.

- Columns UI 3.6.0 build set, pinned to the commits referenced by `v3.6.0`:
  `columns_ui-sdk` `2ac32c02dcf4685c120e4a3a8eaf2ea58a7df89d`,
  `foobar2000-sdk-modified` `85f56870d5e8e5dc7bb8cd07179a44e3a6e5d834`
  and `pfc-modified` `3d3e10ac27d6dce36de4a63dc82a54613b6edd9a`.
- No WebView2 SDK or runtime is used by the public build.
- libsonare 1.5.2: tag `v1.5.2`, commit
  `30931acd0d16e5d890b728548dd74675ec13758c`, vendored with its Apache-2.0
  `LICENSE` and `NOTICE`. The static core/RT, mastering, FX and acoustic-sim
  targets are built for the native serial master rack; mixing and graph targets
  remain disabled. Local qualification patches correct two over-broad RT-safe
  declarations and keep one-bit bitcrusher output finite.
- Eigen 3.4.0: commit `3147391d946bb4b6c68edd901f2add6ac1f31f8c`,
  vendored as libsonare's header-only dependency so builds never fetch code.
- Zstandard 1.5.7: tag `v1.5.7`, commit
  `f8745da6ff1ad1e7bab384bd1f9d742439278e99`, built as the static
  compression/decompression library with programs, tests, dictionary builder and
  multithreading disabled.
- SQLite 3.53.3: official amalgamation `3530300`, SHA3-256
  `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`.
- zstd: pinned source release with its license notice before first use.

See `PATCHES.md` for the reviewable analysis-only integration delta.
