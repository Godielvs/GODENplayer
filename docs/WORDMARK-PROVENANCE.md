# Header wordmark provenance

The current `GODENplayer` header wordmark is the project's hand-authored pixel
art. It is covered by the root MIT license. It does not load, install, embed or
retain outlines from the Homemade Robot font.

The source declaration in `assets/pixel-ui/source/pixels.json` states:
"Hand-authored coordinates and pixel matrices; no icon packs, fonts, AI image
generator, or raster tracing." Its `brand.wordmark_standard` and
`brand.wordmark_compact` matrices produce the corresponding SVGs and the
compiled glyphs in `src/component/pixel_ui/goden_pixel_assets.generated.hpp`.
`PanelD2D::godenWordmark` draws those prepared pixel paths.

The local provenance audit on 19 October 2026 also checked the archived
development change that introduced Pixel UI on 13 September 2026. That change
deleted `goden_wordmark_geometry.cpp/.h`, whose five uppercase glyphs explicitly
derived from Homemade Robot v2, and replaced the renderer with the pixel
assets above. The old attribution and UI descriptions survived that replacement
and have now been removed. No glyph geometry was changed by this correction.
The private development history is not exported with the public tree.

This finding concerns the header lettering only. It does not establish rights
for the separate application icon or any third-party shader.
