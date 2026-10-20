# QA tools and measurement policy

The default `tools/test.ps1` includes offline delivery/privacy tests and
benchmark-runner fixtures. These do not launch the player, control Windows,
upload artwork, or run a real timing benchmark.

## Compatibility probes

- `test-constrained-shell.ps1` extracts the real shell placement methods and
  checks 128 arrangements with hidden child windows, without focus or input.
- `test-render-device-hub.ps1 -SoakSeconds 3` exercises real hardware/WARP,
  unsupported versus transient failures and messages to its own hidden window.
  Finish other builds first; its native probes compile sequentially.
- `python tools/test-installer-root-policy.py --iscc '<ISCC path>'` compiles the
  actual pure directory guards into a silent Inno fixture: 16 cases, no host,
  production registry, shortcuts or application launch.
- `audit-runtime-dependencies.py` parses ordinary/delay PE imports against the
  complete official host runtime. Unknown API sets and unshipped DLLs beside
  a developer Release binary fail the audit. Its unit tests use inert PE data.
- `test-installer-transaction.ps1 -SkipRuntimeUi` uses a disposable `artifacts`
  root, verifies official portable mode/runtime bytes and observed unrelated
  foobar registrations, audio associations and shortcuts. It does not launch
  the player. Never select a real profile as its disposable fixture.

These are bounded native/installation probes, not clean-OS, physical-monitor,
live-network, assistive-technology or full-player performance qualification.
See [COMPATIBILITY.md](COMPATIBILITY.md).

## Retained integration tools

Historical C9/R5/R6 tools are retained because they cover distinct scroll,
soak and renderer paths. Their names do not establish that they are obsolete.
They are not normal build dependencies or unattended desktop tests.

| Tool | Purpose | Explicit caller inputs |
| --- | --- | --- |
| `c9-w3-counters.ps1` | Album wheel/controller counters | disposable lab, album folder, first-track artist/title |
| `run-r5-installed-soak.ps1` | Installed-candidate soak | disposable development app, authorized media folder |
| `run-r6-ambient-qualification.ps1` | Ambient qualification | disposable development app, authorized media folder |
| `run-r6-ambient-soak.ps1` | Ambient soak | authorized media folder and disposable fixture |
| `test-album-glyphflow-transition.ps1` | Artwork transition | disposable development app, two authorized tracks |
| `test-album-scroll.ps1` | Wheel routing and lyrics/track scrolling | portable app to copy, album folder, first-track artist/title |
| `test-library-scroll-resources.ps1` | Library scroll resources | disposable development app, authorized track |
| `test-scroll-during-artwork-transition.ps1` | Scroll during artwork change | source app to copy, two authorized tracks |

Read each parameter block and side effects before use. These scripts can stage
profiles, alter test playlists/playback, launch probes, move focus or send input.
Obtain an exclusive desktop window for UI tests. Use an explicitly QA-enabled
component and the documented environment gates where required; never put that
component in public staging. Keep music, profiles, screenshots and raw logs out
of public sources. Media/app inputs have no personal defaults. Synthetic lyrics
are written only to the copied test profile.

The focused source audit rejects absolute media parameter defaults and personal
Desktop fallbacks; generic OS/per-user installation paths are allowed. This is
a bounded privacy check, not a guarantee that every secret is detectable.

## DSP timing

Run only after builds, source compression and avoidable concurrent load finish.
Preserve failures. Record a changed condition or fix before a new run; do not
repeat a failing measurement until a green result appears.

```powershell
./tools/test-master-dsp-performance.ps1 -Runs 3 `
  -BaselineExecutable out/baseline/master_dsp_integration_tests.exe `
  -CandidateExecutable out/Release/master_dsp_integration_tests.exe `
  -ManifestPath out/qualification/dsp-paired-new.json
```

Inputs must use identical audio, frames, processors and settings. The runner
alternates AB/BA, records binary hashes, each result, mean/max p99 and a descriptive
paired difference. It executes exactly the planned samples, even after a failure;
these are planned samples, not retries. Without a baseline, only the candidate
runs. Existing reports/logs are refused. Parsing/invocation failures and null
metrics are saved before returning failure. The 0.40/2.40 ms gates are unchanged.

CPU context is the `GetSystemTimes` delta around each invocation: system-wide up
to 64 processors, or the calling thread's primary processor group on larger
systems ([Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes)).
`GlobalMemoryStatusEx` supplies before/after memory context.
Unavailable or too-short observations remain null. This does not prove an
outlier's cause, controlled conditions, clock speed, whole-player CPU, GPU cost
or performance approval. Affinity/priority are unchanged. Raw logs stay private
ignored artifacts; finalization exports only sanitized summaries.

Runner tests use short scripted outputs, including failures/malformed data.
They validate order/reporting/immutability, not DSP speed.
