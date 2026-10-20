# Shader provenance and unresolved permissions

Reviewed 19 October 2026. Adaptation, a visual difference, a credit or a source link is not a redistribution license. All approved effects remain in this local candidate as explicitly requested. No author contact or broad reimplementation was undertaken.

| Effect | Source / author | Status |
|---|---|---|
| Cloud Bay | [@diatribes](https://fragcoord.xyz/s/cxbf4gfw.md) | MIT + Unlicense helper; adapted ABI, palette and music response |
| Tile | [@Xor](https://fragcoord.xyz/s/tcsfryp6.md) | CC BY 4.0; adapted ABI, palette and music response |
| Radiance Cascades | [@youssef_afella](https://fragcoord.xyz/s/u4uhc9b9.md) | MIT; identified IQ SDF helpers MIT; adapted ABI, palette and music response |
| Cosmic | [source](https://fragcoord.xyz/s/csln1b0s.md) | Specific redistribution grant not verified |
| Gltch | [source](https://fragcoord.xyz/s/gtl4w04i.md) | Specific redistribution grant not verified |
| Fitas / Ribbons | [source](https://fragcoord.xyz/s/lnbg0175.md) | Specific redistribution grant not verified |
| Port contribution | [source](https://fragcoord.xyz/s/paisgvhv.md) | Specific redistribution grant not verified |
| Roadway contribution | [source](https://fragcoord.xyz/s/rq3v1xn2.md) | Specific redistribution grant not verified |
| Nebula 3 | [source](https://fragcoord.xyz/s/nj47a5wg.md) | Specific redistribution grant not verified |
| Phosphor 2 | [source](https://fragcoord.xyz/s/pnxxz6xp.md) | Specific redistribution grant not verified |
| Radar | [source](https://fragcoord.xyz/s/rw3dfaou.md) | Specific redistribution grant not verified |
| Disco Hive | [source](https://fragcoord.xyz/s/di76dy6c.md) | Specific redistribution grant not verified |
| Fluid Sim Multigrid | [source](https://fragcoord.xyz/s/fspiu4nn.md) | Specific redistribution grant not verified |
| Rainbow Travel | [source](https://fragcoord.xyz/s/rxhy4i5t.md) | Specific redistribution grant not verified |
| Spectrogram | [source](https://fragcoord.xyz/s/avtqpukv.md) | Specific redistribution grant not verified |
| Starship S | [compute.toys 3114](https://compute.toys/view/3114) | Specific redistribution grant not verified |
| Kerr-Newman | [NPGS core](https://github.com/baopinshui/NPGS/tree/ee45822d0a1c9ff28706f50a67d96b48590e88d7) / original wXdfzj | Matching core GPLv3; independent renderer with corresponding source; bloom/camera/code-rain permissions remain unresolved |

Caustics/Lagrangian derives from David A. Roberts and CornusAmmonis, MIT, compute-toys/wgpu-compute-toy commit `60d0bec4bd912a54d5049f2c28c1bd6a0916e5ec`. Identified A4 hash12/hash44 derive from Dave Hoskins / David A. Roberts, MIT ([source](https://gist.github.com/davidar/5f9677a0ccfbd63d7a8657ad9af3a856)). Cloud Bay lowbias32 derives from Chris Wellons, hash-prospector commit `396dbe235c94dfc2e9b559fc965bcfda8b6a122c`, Unlicense. Radiance identified segment/arc SDF snippets derive from Inigo Quilez (MIT). The unused fib function was removed, and hsv2rgb was replaced with an independently expressed six-sector conversion. These targeted changes do not resolve other catalogue permissions.

The historical Original menu denotes the retained upstream visual composition; the ABI/coordinate port is still an adaptation. The Music modes additionally carry audio, palette and temporal changes. Per-file upstream notices are retained. Full license texts are under LICENSES.

## Follow-up primary-source checks

The twelve unresolved FragCoord effect pages above currently explicitly report
no specified license. Rechecking their credited ancestors did not establish a
whole-effect redistribution grant. Starship S identifies qinsx's upload and
[Xor's original l3cfW4](https://www.shadertoy.com/view/l3cfW4); the latter was
not accessible to this audit, so no terms are inferred for it.

Spectrogram credits [Audio Equalizer Demo / ao6dc217 by MilkDrop2077](https://fragcoord.xyz/s/ao6dc217.md).
That original contribution explicitly publishes CC0 1.0 and contains SPDX
notices. Its markdown SHA-256 was
`0e4ea55771f4520f9386036be6466fa0fbb52ca5e90e967d5dffda1f40e75437`.
This is a verified permission for the original contribution only; the derivative
Spectrogram page's different rights statement remains unresolved. It is not
presented as clearance of the complete effect.

The exact [NPGS license at the pinned revision](https://github.com/baopinshui/NPGS/blob/ee45822d0a1c9ff28706f50a67d96b48590e88d7/LICENSE)
is retained as `LICENSES/NPGS-GPL-3.0.txt`, with mapping and dated modification
notice in `LICENSES/NPGS-NOTICE.txt` and the two `wXdfzj_a` sources. Its SHA-256
is `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`.
On 2026-10-20 the integration moved into the independent native
`goden-shader-render.exe`, with the entire retained graph, camera and feedback.
No Kerr bytecode is embedded in `foo_nekorox.dll`. Only value inputs and a final
image cross its documented boundary. It runs fixtures without foobar2000 or its
SDK, and its covered combined program is offered GPLv3 with corresponding source.
See [architecture and reproduction](KERR-RENDERER.md). The dated shader comments
retain the preceding audit's historical notice; no shader formulas were changed.
Separate sonicether bloom/camera and code-rain credits still need their own
permission evidence. The process boundary does not clear those contributions.
