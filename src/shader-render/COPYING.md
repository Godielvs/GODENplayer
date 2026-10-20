# Independent Kerr-Newman image program

`goden-shader-render.exe` is a standalone native image program. It does not link
to foobar2000, its SDK or the GODENplayer component. It owns the entire retained
five-pass Kerr graph, camera, feedback, bloom, temporal state and resize policy.
Its command line can render value fixtures into BMP frames without a player.

The NPGS-derived core is covered by GNU GPL version 3. The complete text is
[`../../LICENSES/NPGS-GPL-3.0.txt`](../../LICENSES/NPGS-GPL-3.0.txt), and its pinned
upstream source and modifications are identified in
[`../../LICENSES/NPGS-NOTICE.txt`](../../LICENSES/NPGS-NOTICE.txt). `main.cpp` is
offered under GPL-3.0-only. Project-authored renderer, build and protocol code
retains its MIT grant and can be combined under GPLv3. The covered executable
and modifications are distributed with the corresponding source, build recipes
and notices. GPLv3 applies to the covered combined program as a whole, not only
to the two core GLSL files; the root MIT license does not replace those
obligations. Independently MIT-licensed project files remain reusable under MIT.

This does **not** assert a GPL grant for the separately credited sonicether
bloom/camera or code-rain contributions. Those permissions remain unverified;
neither this boundary nor the license of newly written code clears them.
See [`../../docs/SHADER-PROVENANCE.md`](../../docs/SHADER-PROVENANCE.md).

Source correspondence includes this directory, the ten `wXdfzj_*` GLSL files,
`src/component/collection/collection_renderer.{h,cpp}`, the MIT native frame
client used by the `--via-client` reproduction mode, registry/graph/audio/
history/motion/policy headers and their project-authored includes, the offline
compiler `tools/c4_compile_collection.py`, pinned compiler dependencies and the
project/build files. The normal full corresponding-source archive contains all
of these, with no requirement to obtain private lab files.

The player is a separate client. The public MIT-licensed `frame_protocol.h`
carries dimensions, time, playback state, palette and ordinary spectral values.
Only the final RGBA image crosses back. No SDK objects, shader calls, camera,
intermediate buffers, or private C++ layouts are exchanged. The client never
loads the Kerr bytecode or a GPL renderer DLL into the foobar process.

The separation is an engineering response to the integration issue, not a legal
opinion or a declaration that the whole shader catalogue is cleared.
