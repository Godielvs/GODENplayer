# Third-party inventory and source correspondence

The root MIT license covers this project's original code only. Exact notices
remain with the sources and under `LICENSES/`. Version/commit/hash pins are in
`third_party/dependencies.lock.json`; effective vendored file hashes are in
`third_party/inventories/`. Per-file license notices take precedence over this
summary. The project is not entirely MIT or fully cleared for distribution.

| Dependency | Version/source | Terms / delivered evidence |
| --- | --- | --- |
| foobar2000 | 2.25.10 x64 | Proprietary; intact official installer only. Host created locally, never patched. Official license HTML retained. |
| Playback Statistics | 3.1.11 | Redistribution permission unverified; optional official download, never bundled. |
| Columns UI | 3.6.0, pinned commit | Per-file notices, permissive COPYRIGHT and retained GPL/LGPL legacy texts. Modified source, submodules and bounded PMv2 patch supplied. |
| Columns UI SDK / modified foobar SDK / PFC | Pinned 3.6.0 build set | Exact permissive SDK, foobar SDK and zlib-style PFC notices retained. Includes the SDK import library needed to link. |
| Spectrum Analyzer | 0.10.0.0-goden6 | MIT; upstream source, 18 patches, effective added headers and build recipe. |
| Spectrum Columns SDK / WTL | 7.0.0 / 10.0.10320 | Exact SDK notice / MS-PL. Sources included in corresponding archive. |
| Project Nayuki FFT / libmsc | Spectrum's pinned source | MIT notices retained in upstream sources and LICENSES. |
| libsonare | 1.5.2 | Apache-2.0 LICENSE and NOTICE; local integration changes in vendor/PATCHES.md and effective sources. |
| KissFFT / r8brain / librosa ports | libsonare's pinned source | BSD-3-Clause / MIT / ISC. Full notices collected; no Python librosa runtime. |
| dr_wav / minimp3 | libsonare's pinned source | Source-embedded public-domain/MIT-0 and CC0 notices; only enabled native targets are linked. |
| Eigen | 3.4.0 | MPL-2.0 and file-specific alternatives, all COPYING texts retained. |
| Zstandard | 1.5.7 | BSD-3-Clause selected; static single-thread library, no command line runtime. |
| SQLite | 3.53.3 | Public-domain amalgamation and origin notice. |
| nlohmann/json | 3.12.0 | MIT; vendored single header and notice, hashes fixed. No replacement required. |
| CUI vcpkg dependencies | cppwinrt, fmt, lexy, GSL, range-v3, WIL | Pinned baseline/overrides; actual source trees, source notices and build recipes included in corresponding archive. |
| glslang / SPIRV-Cross / FXC | Pinned sources / installed SDK | Build-only tools; source licenses retained. Microsoft SDK tools are not bundled. |
| sharp / libvips | 0.35.5 / npm platform package pins | Optional icon-authoring tooling only; sharp is Apache-2.0; the fetched Windows binary package declares Apache-2.0 AND LGPL-3.0-or-later. Per-platform/transitive notices remain in the fetched packages. No tool binaries are redistributed in the installer/source archives. npm integrity pins are in tools/icon-assets/package-lock.json. |

Shader sources, authors, modifications and **unresolved permissions** are
separately enumerated in `SHADER-PROVENANCE.md`. Keeping credits does not grant
missing rights. All approved effects are retained in this technical candidate
at the owner's request. No broad rewrite or author contact was performed.
The owner approved the B / Angular vector icon. Its close visual reference and
remaining rights question are recorded in [icon provenance](ICON-PROVENANCE.md).

The current header wordmark is original pixel geometry, not a bundled or
converted font. Its [provenance](WORDMARK-PROVENANCE.md) explains why the
historical Homemade Robot notice no longer applies to this source tree.

## Mapping sources to binaries

`packaging/component-manifest.json` is regenerated after the final build from
the six actual files, the canonical version, the public source fingerprint
and the dependency lock. It does not reuse a private Git SHA as the source ID.

`GODENplayer-corresponding-sources.zip` contains the effective public tree,
modified Columns UI/Spectrum and their dependencies. `SOURCE-MANIFEST.json`
records every member's hash; the installer manifest records the source ZIP hash
and the managed binary hashes. This file set, including build recipes, is
provided alongside each installer candidate. It is not a written offer to
provide missing sources at an unspecified future date.

The public source export does not contain music, private screenshots, profiles,
caches, credentials, telemetry logs or the laboratory's Git history. Node/npm
dependencies are build/dev-only, pinned by `services/presence-relay/package-lock.json`
and `tools/icon-assets/package-lock.json`.
The optional private relay is not required by public installations.
