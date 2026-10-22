# Demo media

The screenshots and video show GODENplayer with a fictional music library and original demonstration tracks.

| File | Content |
| --- | --- |
| [Album screenshot](media/album.png) | Cover art, synchronized lyrics and Nebula 3 |
| [Library screenshot](media/library.png) | Genre, artist and album filters, with playback and audio views |
| [Video](https://github.com/user-attachments/assets/9f2f36aa-5e02-4f8d-bde2-a0f044c588fb) | 36 seconds with audio: Library, Album and a cover/palette change |
| [Animated preview](media/album-transition.gif) | Six-second silent album transition |
| [Social preview](media/social-preview.png) | Repository sharing card, 1280 × 640 |

The original MP4 is available in `media/godenplayer-demo.mp4`.

## Credits

- **Music and lyrics:** original synthesized tracks, with original lyrics spoken by Microsoft Zira Desktop.
- **Featured tracks:** Quiet Current by Low Tide Club, from Tidal Signal; Silver Line by Glass Orchard, from After the Rain. These names are fictional.
- **Cover art:** two AI-generated featured covers and seven procedural covers. Prompts are included in `media/source/covers/`.
- **Screenshots and footage:** captured from the player. Video audio uses the same original tracks, paired in editing.

The player, voice engine, icon and shaders retain their respective license terms. See [license scope](../LICENSE-SCOPE.md) and [shader credits](SHADER-PROVENANCE.md).

## Sources

Optional generation recipes are in `media/source/`; capture metadata and file hashes are in [provenance.json](media/provenance.json).

To regenerate the demo library in a separate folder, copy `media/source/`, run `make-voices.ps1`, then `python generate-demo-library.py`. This requires Windows with Microsoft Zira Desktop and Segoe UI, Python, NumPy, SciPy, Pillow and FFmpeg. These tools are only needed to regenerate the demo, not to use GODENplayer.

The social card places the project icon and original pixel wordmark beside
the Album view, with cover art, lyrics and Spectrum. Its editable SVG and input/output hashes are in
`media/source/social-preview.svg` and `media/source/social-preview.json`.
Regenerate with `npm --prefix tools/icon-assets ci --ignore-scripts`, then
`node tools/icon-assets/render-social.cjs`. The checked-in rendering uses Segoe UI
on Windows.
