# Final preparation review — candidate 3

Sixteen independent read-only reviews covered artwork, native builds, preferences
and rights records, source delivery, DSP, GPU ownership, installer transactions,
metadata, network transports, Presence policy, presentation/DPI, privacy,
statistics acquisition, UI/history, visual temporal state and worker IPC.
The primary maintainer checked and implemented the findings. Reviews are not
runtime qualifications; exact build/test results are kept with candidate artifacts.

## Changes and focused evidence

| Area | Change | Relevant offline verification |
| --- | --- | --- |
| Artwork | Accepted-frame clock shared with GlyphFlow; reject stale acknowledgements; retain rejected settling frames; match Workspace draw ownership | Clock tests plus actual Direct2D/WIC transition probe, delayed first frame, rejected first/final frame, fitted raster, aspect and reduced motion |
| Library / Album | Supersession epoch and resolution ticket; bounded cancellation during folder scanning, image hashing and metadata traversal | Coordinator lifetime/cancellation tests; existing Album/core tests; native compile |
| UI / history | Stop hidden lyrics timers; preserve Subtle; select exact historical file/subsong and refuse ambiguous legacy tags | SQLite history round trip and native compile; full interaction observation pending |
| Analysis | Freeze a readable source before resuming the worker; fingerprint and validate outside the UI state lock; reject stale queued cache writes | Verbatim reader/completion and writer guard with real file fingerprints; equal-size/equal-timestamp replacement and write exclusion during decoding |
| DSP | Precomputed utility coefficients; scoped denormal control; demand-gated telemetry; stale-stage rejection; safe repeated clear | Actual engine compiler, graph exchange and callback fixture; startup, polarity, stale stages, double clear, PDC flush and MXCSR; vendor functional tests |
| Visual state | Independent track identity reset even when no audio texture is used or the timeline is frozen; shared device-loss classification | Musical state/ABI tests; full GPU output and physical device-loss observation pending |
| Presentation | Spectrum root visibility/cloaking gate, partial surface cleanup and 250 ms retry backoff; WaveSeek resource retry backoff | Patch-series application, production resource-admission fixture, source contracts and native compilation; hardware recovery/occlusion observation pending |
| Presence / network | Disable Catbox POST redirects; resolve malformed relay responses even after timeout removal | Production fake-WinHTTP and relay tests; no artwork uploads or Discord observation |
| Acquisition | Validate bounded official host download before publishing cache entry; retain good existing files and allow retry after failure | Seven transfer/identity fixture cases; cached real host hash, signature and version validation |
| Delivery / privacy | Transactional staging and source archives; prevalidate path collisions; reparse/private-state refusal; redact sanitizer reports; additional PEM rules | Adversarial source, staging, archive refusal, finalization and privacy tests |
| Build correspondence | Pre/post source snapshots, actual copied Spectrum headers/targets, SDK/WTL and vcpkg source trees; reject unknown untracked build inputs | Input hashes, stale-source negative tests, dependency receipts and archive verification |

Normal visible refresh rates, render quality, shader identities, analyzer contracts,
Original modes, Clovis response and new-profile Nebula default are preserved.
Resource retry backoff only applies after failure. Frozen track changes refresh
identity-dependent state without advancing visual integration time.

## Deliberately retained behavior and limits

- The interactive Catbox choice remains preselected with the approved disclosure.
  Silent setup still needs explicit cover consent. Existing off/private choices win.
- Library grouping by tags and AlbumSession edition/folder grouping have distinct,
  tested purposes. They were not collapsed into one behavior.
- The SDK returns a fully allocated embedded-art object. Our size/decode/cache
  bounds and cancellation do not prevent that allocation inside the host.
- Some Library setup/copy/sort work remains indivisible. Moving SDK calls off the
  main thread without proving their affinity would introduce another risk.
- Spectrum partial-surface recovery is bounded to its existing device lifecycle;
  a complete independent D3D-device recreation design is not claimed here.
- Untracked dependency files, including resources and nested submodule inputs,
  are refused unless explicitly included. Ignored/generated inputs still require
  review against the declared build recipes; a filesystem snapshot alone is not
  proof of a hermetic compiler environment.
- Explicit deep analysis holds a read-only sharing guard until that run finishes;
  tag edits or file renames may require canceling analysis first. Playback and
  ordinary library indexing do not acquire this guard.
- Legacy test-only ring code was not mistaken for the production frame bus.
- Compilation is serial. No private installation, registry entry, media library,
  icon or GitHub state is changed by this review.

The previous strict DSP timing failure remains recorded in QUALIFICATION.md.
No timing result was rerun merely to replace it with a pass. Whole-player speed,
memory after prolonged use, DPI across monitors, live Presence and visual behavior
still require a matched runtime session. Shader permission/integration questions
and the icon remain open; this candidate is not represented as legally cleared.
