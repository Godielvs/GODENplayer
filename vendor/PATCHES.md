# Vendored libsonare integration patches

The repository pins libsonare v1.5.2 at commit
`30931acd0d16e5d890b728548dd74675ec13758c` and carries this narrow integration
patch for the NekoRoX worker:

1. `src/analysis/music_analyzer.cpp` reuses the unified analyzer's existing
   harmonic chroma for key estimation. Upstream's automatic audio-candidate path
   computes four complete full-song feature paths, including HPSS, concurrently
   with the unified caches. That cannot satisfy the worker's 512 MiB containment
   contract on ordinary album-length tracks.
2. `src/analysis/boundary_detector.*` evaluates local checkerboard novelty via
   the exact Gram identity `||sum(left) - sum(right)||^2`. This removes the
   quadratic full-song self-similarity allocation while preserving the uniform
   checkerboard response and the full temporal grid.

3. `mastering/eq/cut_filter.h` and
   `mastering/maximizer/adaptive_release.h` no longer advertise setters as
   callback-safe when their composed implementation may rebuild dynamic
   storage. GODENplayer routes those changes through shadow compilation.
4. `mastering/saturation/bitcrusher.cpp` gives valid one-bit mode a finite
   positive quantisation step instead of dividing by zero.

The analysis changes do not affect decoding or playback. Their accuracy is
covered by the local BPM/key/beat/section promotion corpus. The mastering
changes are covered by `master_dsp_libsonare_smoke`, including irregular and
8192-frame blocks, finite output and allocation-counted RT setters.
