# Qualification and coverage

This page summarizes validation scope and known limits. For an installer, use
its matching `QUALIFICATION.md`, `QUALIFICATION.json` and `SHA256SUMS.txt`.
Those reports bind results to the exact installer, source archives and binaries;
results from one build do not qualify another.

## Test scope

Offline tests cover core behavior, renderer state, provider protocols, privacy,
source correspondence and installer failure handling. Optional runtime probes
exercise a disposable player installation. See [build instructions](BUILD.md)
and [QA tools](QA-TOOLS.md) for the commands and their side effects.

Headless installer transactions and runtime observations are separate results.
Skipped runtime checks remain untested. Functional tests and GPU reference-image
comparisons do not establish complete visual or performance qualification.

## Performance observations

A historical strict isolated DSP run missed the common timing gate: 0.4471 ms
p99 against 0.40 ms per 1024 frames. Creative DSP measured 0.5391 ms against
2.40 ms. Earlier values of 0.3616 ms and 0.5267 ms do not replace the failed
common gate. No matched baseline established a regression or its cause.

Historical public.9 full-player observations included 28.4–29.3 child images/s,
a 377 ms heartbeat gap and a 357 ms Album preparation gap. A baseline run had a
1158 ms preparation gap and incomplete GPU collection. Variable background load
prevents attributing these observations to a candidate-specific regression.
These are historical measurements, not current release results or an
optimization claim.

Shader-pass timings omit image transport and composition. Frame publication
does not measure physical display completion. Matched full-player performance
remains unqualified.

## Remaining coverage

- Physical multi-monitor DPI at 100–200%, restored positions, menus, dialogs
  and click accuracy.
- Exhaustive visual/audio comparison, seek, artwork transitions, large-artwork
  indexing and matched startup, CPU, memory and frame-time measurements.
- Extended Presence reconnection, long-track playback and varied networks.
- Fresh hosted CI, clean Windows/Windows N, ARM emulation, Wine, HDR, pixel
  arrangements and full accessibility integration.
- Arbitrary power loss. Bounded filesystem fault injection does not establish
  recovery from termination at every transaction instruction.

See [compatibility](COMPATIBILITY.md) for the supported platform contract.
Technical checks do not resolve the permissions recorded in
[license scope](../LICENSE-SCOPE.md), [shader provenance](SHADER-PROVENANCE.md)
and [icon provenance](ICON-PROVENANCE.md).
