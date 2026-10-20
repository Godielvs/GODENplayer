# NekoRoX Spectrum Analyzer Fork

This directory records the minimal, source-built fork of Spectrum Analyzer
0.10.0.0 used by NekoRoX V3. The visual model and serialized preset format are
unchanged. The fork replaces the render-thread busy wait with a high-resolution
waitable timer during playback and an indefinite event wait while playback is
stopped, paused, hidden, frozen or minimized. Playback, configuration,
visibility and host-window events wake exactly one immediate frame before the
renderer either resumes or returns to sleep. On pause, current and peak layers
perform a bounded, frame-rate-independent release to the baseline before the
renderer returns to event-idle. The configured 200 Hz rate is preserved during
active playback.

The 18-patch series also makes the x64/v142 build explicit, labels the component
`0.10.0.0-goden6`, and removes build date/time strings so identical pinned
inputs can produce deterministic release binaries.

Run `tools/build-spectrum-analyzer.ps1`. It validates every pinned revision,
downloads pinned dependencies only when missing, applies the patch series, builds
outside the foobar profile, and writes a dependency/hash manifest beside the
output DLL.

Upstream source and build dependencies retain their original licenses under
`licenses/`. No upstream source tree or compiler output is installed in the
foobar profile.
