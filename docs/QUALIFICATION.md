# Local candidate qualification

Version: **5.1.0-beta.18-public.11**. This is a local beta candidate,
not a published release, complete runtime approval or legal clearance.

The matching delivery has a generated `QUALIFICATION.md/.json` and
`SHA256SUMS.txt`. They identify the exact installer, source archives, six managed
binaries and installer transactions. Never reuse a receipt for different bytes.
Tracked documentation describes scope without embedding its own source hash;
generated reports provide that binding. Raw ignored qualification logs can
contain local paths and do not belong in the public export.

## Candidate 11: database initialization and local beta closure

Fresh-boot qualification exposed concurrent database initialization: separate
startup workers could read an old schema version and later attempt migrations
already applied by another connection. A new concurrency fixture failed against
the old code; initialization now has bounded per-file ownership across threads
and processes. Tests cover schema/data integrity, timeouts and abandoned owners,
alongside existing migration backup and rollback checks. Ordinary database work,
audio and visual behavior are unchanged. Matching generated reports identify
the exact rebuilt binaries and repeated installer/runtime outcomes.

First-boot QA now uses the pinned host's advertised `/show` command, requires a
non-minimized workspace and waits for completed startup import. The same public.10
installer passed a diagnostic transaction run with `/show` in place of the prior
`/new` argument. This isolates a test invocation issue; that diagnostic receipt
does not qualify different public.11 bytes or erase the old failed attempts.

## Retained candidate 10: documentation closure and failed first boot

This revision synchronizes the current identity and documentation and corrects
the installer QA report: the persisted Presence preference is read and checked,
and first-boot and rollback probes have separate output files. Player runtime
behavior, shaders, quality, cadence and saved preferences remain those of public.9.
The normal one-worker build and exact installer/source binding are repeated
after these source changes. Only the matching generated delivery reports record
which first-boot, active-upgrade, rollback and UI observations completed; this
pre-build document did not assume their outcome. File-only installer transactions
passed, but runtime first boot reported a dialog titled Error. A diagnostic run
also observed it. Its contents could not be captured through the available native
window tool. The separately verified database race is not asserted to explain
that dialog. Those public.10 attempts remain failed; they are never replaced by
public.11 receipts.

The broad platform and display matrix below describes coverage limits for a beta.
Source publication does not establish a fully qualified release on every listed
environment. See [publication readiness](PUBLICATION-READINESS.md).

## Retained candidate 9: Kerr boundary and runtime observations

Kerr-Newman Original and Music run in an independent native program with no
foobar2000 SDK dependency. The ten shader sources and compiled programs remain
unchanged and are absent from the player component. Deterministic integration
produced 61 byte-identical images against the approved renderer, including mode,
pause/resume, hidden, resize and track-change cases. See [the program boundary](KERR-RENDERER.md).

The user confirmed Music-mode appearance, two pause/resume cycles, return between
Library/Album and a more square resize. Native capture failed with a window-owner
error, so this is a human observation. An additional manual Original observation
had not completed when the public.9 evidence was frozen; do not relabel GPU
equivalence as that observation.

Full-player measurements normally observed about 28.4-29.3 child images/s at the
existing 30 Hz policy. One candidate run had a 377 ms heartbeat gap and a 357 ms
Album preparation gap; a baseline run had a 1158 ms preparation gap and incomplete
GPU collection. The failed gate is retained. Variable background load does not
establish its cause or a candidate-specific regression. A stable diagnostic pair
observed 0.308% baseline versus 0.497% combined candidate CPU and about 51 MiB more
private memory. This is not an optimization claim. Rolling shader-pass p95 values
exclude image transport and composition, and publication polling does not measure
physical display completion. These are public.9 observations with their original
identities; they do not become new public.11 performance approval.

## Retained candidate 8: deployment documentation closure

The owner-authorized Discord upload is complete for the three existing static
keys and the separate general application icon. The saved portal state was
reloaded. All three static CDN PNGs are byte-identical to the approved 1024 px
source. The general icon was normalized by the portal to 256 px; its decoded
rendition was visually checked, not declared pixel-identical. These are remote
deployment checks, not a new Discord client-card observation. See
[icon provenance](ICON-PROVENANCE.md) and the public asset manifest.

This revision changes deployment documentation and native version identity.
It preserves the public.7 audio, visual, cover-delivery and cadence code. The
matching new delivery report binds any repeated build, file-transaction and
runtime checks to its exact setup. Old evidence is retained with its original
scope. The automation surface available during this closure exposed the browser
but no native Windows app capture/control; no new complete-player visual,
audible-output or matched-performance approval is inferred from it.

## Retained candidate 7: approved icon

The owner selected B / Angular. Installer/shortcut and prepared Presence assets
now use its exact approved vector geometry. This is an asset and version-identity
revision; it does not change the runtime visual catalogue, audio, cover provider
or cadence. Icon generation and package evidence are recorded in the matching
delivery. The official host is unchanged. At the time public.7 was packaged,
Discord static asset replacement had not yet been performed. The later remote
deployment is documented above; frozen public.7 receipts remain unchanged.
See [icon provenance](ICON-PROVENANCE.md).

## Retained candidate 6 cover delivery

This revision packages the Catbox fixes and corrected installer runtime harness.
It preserves the approved visual catalogue, quality and presentation cadence.
No matched full-player performance improvement is claimed for this revision.

The cover implementation was qualified before the packaging identity/docs bump:

- Production provider methods, real WIC encoding and fake WinHTTP pass origin
  and delivery validation, cancellation, durable cache, repair limits and
  metadata-free thumbnail tests. The complete production relay client passes
  its fake-transport lifecycle tests. The corruption regression fails against
  the preceding implementation and passes with the fix.
- A complete bounded PNG structure is required at both origin and delivery.
  A proxy can return a decodable image despite a truncated stored origin, so
  proxy validation alone was insufficient. Production validation checks PNG
  structure/CRCs; live qualification additionally fully decodes the images.
- Generated live repaired and new images were decoded at both endpoints with
  matching pixels. The origin URL was persisted before delivery verification;
  the final URL cache remained byte-identical across a disposable-host restart.
- The user confirmed the new generated cover for **GODEN Fresh Cover 3** in the
  real Discord client after restart. Automatic Windows capture failed, so this
  is a human observation, not automated screenshot proof.

That native run and its source/binary hashes are retained in the local Catbox
qualification receipt. Delivery observations may bind its unchanged
implementation/fixture files; they do not relabel that run as a public.6
installer test. The exact public.6 build and installer results are generated
separately. No personal artwork was uploaded.

The upstream cause of stored byte loss remains unknown. Catbox/wsrv are external
dependencies, not guaranteed infrastructure. Bounded retries and the ten-minute
repair guard can leave text-only Presence until a later admitted or revisited
request. Tests do not establish indefinite recovery, long-track or corporate
network behavior. TLS, quality and provider choice were not weakened.

## Retained evidence and its boundaries

- Candidate 5 added Windows motion preference routing and monitor-specific
  DirectWrite parameters. Production-path fixtures check routing, caching,
  stored/effective preferences and real grayscale WIC pixels without changing
  global settings. Physical display appearance remains unqualified.
- Candidate 4 compatibility probes cover unsupported-adapter dormancy/recovery,
  constrained child-window layout, import closure against the complete official
  host and bounded proxy recovery. See [COMPATIBILITY.md](COMPATIBILITY.md).
- Earlier builds used pinned MSVC/SDK toolchains and one native worker; all 56
  collection programs and declared A4/Caustics/scope/CRT passes compiled. Core,
  async text/pixel/lifecycle and visual-state suites passed. Exact delivery
  observations identify reruns; old tests are not automatically new evidence.
- Relay server TypeScript/authentication/protocol/PNG tests, production relay
  transport fixtures, archive/privacy/finalization negatives and real Inno
  Statistics-download fixtures passed in their recorded runs. The latter cover
  malformed/altered/oversized/interrupted/unavailable responses, verified x64
  installation, existing-component preservation and offline completion.
- Dependency receipts bind modified Columns UI/Spectrum sources to their
  binary hashes. The official foobar2000 executable is not modified.
- Radiance HSV conversion matched the preceding algebra over 224,361 samples
  (maximum double error below 2.3e-13); this is numerical, not visual approval.
- Historical isolated DSP timings were common p99 0.3616 ms and creative
  0.5267 ms per 1024 frames. A later strict observation failed its common gate:
  0.4471 ms against 0.4 ms (creative 0.5391 ms against 2.4 ms). Both are retained;
  no matched baseline established a regression or its cause. Runner fixtures
  and functional DSP checks do not measure whole-player performance.

The later **public.5** runtime follow-up, bound to its older installer, observed
clean first boot, no configuration dialogs, profile-identical upgrade, retained
and booted rollback, active-host shutdown on upgrade, Statistics preservation,
Unicode/spaced paths, unrelated foobar state preservation and PMv2 on the current
monitor. Separate CLI playback, pause/resume and repeated-open observations
also exist. A human reported Library/Album, cover, scope and Spectrum normal
after restart. These are bounded retained observations, not exhaustive visual
or physical-monitor qualification and not proof of the public.6 installer.

## Exact-package qualification

`tools/test-installer-transaction.ps1` distinguishes headless file/fault tests
from disposable-host runtime probes. `-SkipRuntimeUi` records runtime observations
as null, never PASS. Runtime mode launches only its isolated official host;
separate fault fields it does not execute remain null. Matching evidence must
identify the exact new setup hash, complete payload, official host/runtime,
clean/offline install, upgrade/profile/Statistics, rollback and uninstall checks.
Headless and runtime runs are separate receipts; neither silently fills the
other's skipped fields.

`tools/finalize-release.ps1` rejects mixed candidates and exports only rehashed,
allowlisted files and sanitized observations. See [QA-TOOLS.md](QA-TOOLS.md).
The final report, rather than this pre-build document, records completed checks.

## Remaining coverage

- Physical multi-monitor PMv2 at 100–200%, restored positions, menus/dialogs,
  suggested rectangles and precise clicks. A runtime PMv2 flag is narrower.
- Exhaustive Library/Album/shader comparison, actual audio output,
  scope/Spectrum cadence, GlyphFlow, seek, large artwork/indexing and matched
  whole-player CPU, memory, startup and frame-time measurements.
- Extended complete-player Presence reconnection, long-track and varied networks
  beyond the generated-cover and restart observations above.
- Fresh hosted CI (quota unavailable), clean target Windows/Windows N,
  ARM/Wine, HDR, physical pixel arrangements and full accessibility integration.
- Arbitrary power loss or termination at every transaction instruction. Bounded
  filesystem fault injection does not establish those outcomes.

Personal installation/profile promotion is outside this preparation. An early
installer QA run exposed AppId isolation affecting the personal uninstall
registration, without replacing the personal app/profile. The observed version,
location and uninstall command were restored; incidental metadata was not claimed
as an exact snapshot. QA now has a distinct AppId, refuses personal destinations,
disables production registration/previous-directory reuse and checks unrelated
state. Incomplete recovery states abort before deletion.

Private relay users must configure individual credentials before server-side
authentication enforcement. Public defaults contain no personal endpoint and
do not migrate private profiles silently.

Shader grants, Kerr-Newman's remaining contribution permissions and the approved B icon's similarity/rights
remain explicit in `LICENSE-SCOPE.md` and `SHADER-PROVENANCE.md`. Technical tests
do not resolve them. No GitHub push, release or upload is included.
