# Kerr-Newman program boundary

The two retained modes run in `goden-shader-render.exe`, a native program beside
`foo_nekorox.dll`. All ten shader sources, formulas, pass order, formats, camera
state, feedback and final presentation are retained. Other effects stay on the
existing renderer. No network service, runtime compiler or extra runtime is used.

The player sends value records using [the public protocol](../src/shader-render/frame_protocol.h).
One mutex-protected mailbox coalesces requests; two inherited events notify work
and results. Only explicitly allowlisted handles are inherited. The renderer
uses the same GPU adapter and publishes the final image through a keyed shared
RGBA8 texture. The player takes ownership without waiting and copies the valid
image rectangle for its normal Direct2D presentation. No intermediate image or
state is shared. The private job terminates the child when its owner closes.
When measuring Kerr CPU or memory, include both processes. The extra native
process and final-image GPU copy are costs of the separation; other effects
do not launch it. Per-renderer GPU timing remains measured in the image program.

Missing executable, invalid protocol, terminated child, invalid output and
bounded timeout produce a recoverable renderer failure. There is no fallback to
an embedded Kerr program. Explicit visual Retry recreates the process. Hidden
surfaces suspend work; pause retains the existing four-second slowdown and
freeze. Resize is handled by the program without resetting its camera.
Motion eligibility and permission to render are separate values. A stopped or
reduced-motion view can obtain a static first image; temporary work deferral
does not masquerade as visibility loss. Suspension is latched across coalesced
hide/show requests. The player polls outstanding results at the preset's 30 Hz,
including startup, without waiting for the child or GPU on its UI thread.

## Standalone reproduction

Build the independent project with MSBuild v142 / Windows SDK 19041, one worker:

```powershell
./tools/c4-bootstrap.ps1 -Workers 1
MSBuild src/shader-render/goden-shader-render.vcxproj /m:1 /p:Configuration=Release /p:Platform=x64
python -B tools/test-kerr-separation.py --generate out/kerr-fixture.bin
./out/Release/goden-shader-render.exe --fixture out/kerr-fixture.bin --out out/kerr-frames
MSBuild tests/kerr_client_probe.vcxproj /m:1 /p:Configuration=Release /p:Platform=x64
python -B tools/test-kerr-separation.py --renderer out/Release/goden-shader-render.exe --host-probe out/Release/goden-kerr-client-probe.exe --out out/kerr-comparison
```

`--warp` chooses the Windows software adapter for reproducible headless checks.
Windows x64 fixture files contain four little-endian uint32 values (magic, version, record
size, count), followed by at most 4096 `Input` records. They contain no paths,
media, credentials or player objects. See the fixture generator for the exact
little-endian encoding. Sizes, offsets and IEEE-754 representation are asserted
in the public header. BMP readback is a CLI test
feature; the player path stays on the GPU.

The source export includes the program, ten GLSL files, renderer, protocol,
compiler recipes and pinned dependency descriptions. The release manifest binds
the helper hash to the same source snapshot as its native client. See
[COPYING](../src/shader-render/COPYING.md) for the GPL-covered core and the
separate contributions whose permissions remain unresolved.

The architectural rationale follows the GNU FAQ's distinction between separate
programs communicating at arm's length and modules with intimate shared state:
[proprietary systems](https://www.gnu.org/licenses/gpl-faq.en.html#GPLInProprietarySystem),
[aggregation](https://www.gnu.org/licenses/gpl-faq.en.html#MereAggregation).
An executable suffix or IPC alone is not a legal exemption.
