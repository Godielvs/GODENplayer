# Building GODENplayer

This tree is independent of the private development history. Read
`LICENSE-SCOPE.md` before redistributing it: technical completeness does not
clear the outstanding shader and icon permissions.

For the exact sources matching an installer, check out its release tag
(`v5.1.0-beta.18-public.12` for the first public beta). Each release includes a
public source archive, a corresponding-sources archive with the modified
dependencies, and their SHA-256 checksums. The tagged source inventory records
the packaged revision; `main` may contain later documentation or code changes.

## Supported environment

Windows 10 2004+ / Windows 11, x64; PowerShell 7; Git 2.40+; Python 3.12+
with the Windows `py` launcher selecting 3.12+ by default;
Node 24 LTS and npm for build tooling and the optional relay tests only.
Install Visual Studio C++ Build Tools with MSVC v142 14.29, its ATL component,
and Windows SDK 10.0.19041.0. Columns UI additionally uses MSVC v143 14.44,
vcpkg and SDK 10.0.26100.0 from VS2022. Its upstream v145 setting is
explicitly overridden by the documented compatibility recipe.

Install VS2022's C++ CMake tools (CMake 3.27+ and Ninja), its vcpkg component,
7-Zip 24+, Inno Setup 6.6+ with full archive
extraction support, and the SQLite command line shell. Microsoft toolchains
and their redistribution terms remain separate. No compiler, Python, Node,
web browser or HTTP server is needed by the installed player.

Icon assets are checked in. Optional regeneration uses the separate pinned
`tools/icon-assets` npm lock; see [icon provenance and recipe](ICON-PROVENANCE.md).
It does not add a runtime dependency or change the official host resources.

All native recipes use **one compilation worker**. Do not run them in parallel.
The fixed sources and checksums are in `third_party/dependencies.lock.json`;
vendored source inventories record the effective files, including local changes.

## Build

Run the following from the repository root in PowerShell:

```powershell
./tools/bootstrap-public-build.ps1
# Optional: -InstallMissingVisualStudioComponents provisions the declared VS components.
# CMake and Ninja are resolved from the declared VS2022 installation.
./tools/c4-bootstrap.ps1
./tools/build-columns-ui.ps1 -VsRoot '<VS2022 installation directory>'
./tools/build.ps1 -Configuration Release -Workers 1
npm --prefix services/presence-relay ci
./tools/test.ps1 -Configuration Release
```

`build.ps1` resolves the v142 environment, builds Zstandard/libsonare and
Spectrum, then the native solution. Spectrum applies all **18** checked-in
patches. Generated shader headers are produced offline by the pinned glslang,
SPIRV-Cross and SDK FXC tools. Sources are never fetched by the running player.

Output: `out/Release`. `foo_nekorox.dll`, `nekorox-analysis.exe`,
`goden-crt-render.exe`, `goden-shader-render.exe`, `foo_ui_columns.dll` and
`foo_vis_spectrum_analyzer.dll` are the six managed runtime binaries.
The independent Kerr renderer has no foobar SDK dependency; see [its build and
standalone reproduction](KERR-RENDERER.md).
Use a simple checkout path such as `C:\src\GODENplayer`, without whitespace,
quotes or command-shell metacharacters (`& % ^ ! ( )`): upstream Columns UI
post-build hooks pass paths without quoting. Bootstrap rejects these paths
before making changes. Build logs and symbols are local
diagnostics and are excluded from public source exports.

QA mutation/fault commands are disabled in the normal build. For a disposable
test profile only, `build.ps1 -EnableQaHooks` produces a separate component in
`out/qa/Release`. Runtime QA also requires both explicit environment gates.
Never substitute that component in the public staging directory. Core fault
tests link their separate QA core library, not a QA-enabled public component.

## Source correspondence and installer

After the final build and tests, generate evidence from the actual outputs:

```powershell
python tools/public_release.py manifest
python tools/public_release.py stage out/public-staging
python tools/public_release.py archive out/release/GODENplayer-corresponding-sources.zip --corresponding
python tools/public_release.py archive out/release/GODENplayer-public-source.zip
./tools/download-public-host.ps1
./tools/build-friend-installer.ps1 `
  -PayloadStaging out/public-staging `
  -FoobarInstaller out/downloads/foobar2000-x64_v2.25.10.exe `
  -CorrespondingSources out/release/GODENplayer-corresponding-sources.zip `
  -SqliteExecutable sqlite3.exe
```

Use a new staging directory for each candidate. The source archive includes
the modified Columns UI and its submodules, Spectrum, official SDK, WTL and
the actual vcpkg source trees used by Columns UI. It contains no Git history,
personal profile or pre-extracted foobar2000 installation. Its file manifest
is checked against the release source fingerprint and binary manifest.

The installer embeds the complete, unmodified official **2.25.10 x64
installer**, verified by SHA-256, architecture/version and Authenticode
publisher. It creates the portable host locally and checks the installed
executable against the official byte hash. No host-manifest patcher is used.
The official installer receives `/P` from the outset, with the NSIS `/D=`
argument last and unquoted. QA checks the host-created portable marker, absence
of the legacy nested uninstaller, unrelated foobar registration/shortcut state
and byte-identical official runtime files. See [COMPATIBILITY.md](COMPATIBILITY.md).

After preparing an official portable reference, audit runtime closure without
consulting this machine's installed redistributables:

```powershell
python tools/audit-runtime-dependencies.py --host '<official portable reference>' --managed out/Release --output out/qualification/runtime-imports.json
```

This parses imports; it does not replace execution on a clean target OS.

Playback Statistics 3.1.11 is an optional official download during installation.
Its DLL is not embedded in this package. Hash verification precedes extraction;
only the pinned x64 DLL is admitted. Offline installation remains usable.
Existing statistics, components and preferences survive an update.

The checked-in clean seed is seven allowlisted configuration files, with
recorded hashes. Do not replace it with a personal profile. Keep the source ZIP
beside the installer when sharing a qualified candidate.

## Qualification and CI

See `docs/QUALIFICATION.md`. `tools/test-installer-transaction.ps1` installs
only into its constrained disposable QA root. No personal promotion runs as
part of build or CI. UI/DPI, Discord rendering in another client, visuals and
matched performance require runtime evidence beyond source-level assertions.

`.github/workflows/build.yml` prepares a Windows build and runs deterministic
tests with read-only permissions. It has no publish/deploy steps or production
credentials. The workflow itself cannot be claimed to pass before it has run.
The standard test script includes the relay's TypeScript/protocol checks, so its
development dependencies must be installed even when no relay is deployed.

CI uses `test.ps1 -UseWarp -SkipPerformanceGates`: the D2D probe explicitly
uses software rendering, and DSP correctness runs without its timing gates.
The default local command still uses hardware D2D and enforces the original
DSP limits. A shared CI runner cannot qualify GPU cadence or isolated CPU cost.
Run `out/Release/master_dsp_integration_tests.exe` separately, without builds
or compression in parallel, to retain the strict timing tests.
For paired measurements and contemporaneous CPU/memory context use
`tools/test-master-dsp-performance.ps1`; see [QA-TOOLS.md](QA-TOOLS.md).

`test-presence-cover-headless.ps1` executes production cover-provider method
bodies and the real WIC PNG encoder against deterministic fake WinHTTP. It
covers cancellation, durable origins, delivery recovery and bounded caches,
without uploading anything. The relay client has a separate lifecycle fixture.
`test-statistics-download.py --iscc <ISCC.exe> --package <pinned-package.zip>`
compiles the exact installer download procedure into an isolated silent test
installer and exercises loopback success/failure responses. Neither test proves
full-player behavior or rendering in Discord.

The normal prerequisite is installed, licensed MSVC 14.29 ATL. An explicitly
provided `GODEN_ATL_ROOT` may point to an equivalent licensed extraction with
`include/atlbase.h` and `lib/x64/atls.lib`; it is not included in source exports.

For file-transaction qualification without launching or controlling the player,
pass `-SkipRuntimeUi` to `test-installer-transaction.ps1`. Its report explicitly
leaves first-boot, window/DPI and active-upgrade observations unqualified.

`tools/test-catbox-network.py --allow-public-test-upload --report <local-report>`
is a separate opt-in API check. It uploads one generated non-personal PNG;
the object is public and anonymous deletion is unavailable. `--reuse-origin`
checks that existing test object without uploading again. It is not run by CI
and does not prove behavior in the full player or another Discord client.

Dependency updates must change pins, effective inventories and corresponding
sources together, followed by a clean build. Do not silently accept a different
host installer, mutable SDK, personal relay endpoint or old DLL from `out`.

## Finalize one exact local candidate

After transaction qualification, close the delivery with checked-in tooling.
Use fresh destinations outside the installer input directory. This command does
not run tests, approve rights/performance, publish or promote an installation:

```powershell
./tools/finalize-release.ps1 `
  -InstallerDirectory out/release `
  -ReleaseManifest packaging/component-manifest.json `
  -TransactionEvidence artifacts/installer-transaction-qa/installer-qa-result.json `
  -OutputDirectory out/final-candidate `
  -SourceExport out/github-source-candidate `
  -Observation @('dsp=out/qualification/dsp-paired-new.json')
```

Both ZIP roles, exact embedded release manifests, source/dependency fingerprints,
installer size/hash/version, six component hashes and transaction/official-host
hashes must agree. Unsafe/ambiguous Windows ZIP paths and unsupported receipt
schemas/outcomes are rejected before output creation. Copied artifacts and every
exported file (including its manifest) are rehashed in fresh staging directories.
Existing destinations are refused. Failure removes only staging/new outputs
created by this invocation; previous candidates are never merged or overwritten.

Qualification JSON/Markdown and `SHA256SUMS.txt` come from validated fields.
`INSTALLER-BUILD-VERIFIED.json` is a sanitized receipt retaining the input hash;
arbitrary build fields, reports, raw logs and user prose are not copied. Optional
`NAME=JSON` observations preserve failure/null outcomes. Identities must match
when provided; fixture source hashes qualify only those sources. Unbound receipts
remain informational. Headless UI observations remain null; runtime, performance
and rights remain pending. Checksums detect changes, not publisher identity.

To check ZIPs separately, use `public_release.py verify`, adding `--public` for
the public-only ZIP and `--release-manifest` to bind it to a candidate. The old
private `out/` finalization helper is not required and must not finalize new work.
