# GODENplayer B / Angular icon

The owner selected study B on 19 October 2026. Its canonical source is
`packaging/brand/goden.svg`: hand-authored paths, angular eyes, tapered cheeks,
a short mouth and a rose-to-cyan outline. The selected geometry is unchanged
from the approved study. No raster tracing, original image pixels or font
outlines are used by the current generator. The header pixel wordmark is separate.

The design deliberately remains visually close to the preceding GODEN icon,
which was made from foobar2000 artwork. Newly drawn paths do **not** by
themselves establish independent rights or permission for a similar design.
The [official foobar2000 license](https://www.foobar2000.org/license) credits
its icons separately; this project has not obtained an adaptation grant.
Visual approval is recorded, but the similarity/rights question remains open.
The root code MIT license does not license the icon or third-party artwork.
No trademark clearance or endorsement is claimed.

## Reproduction and consumers

The checked-in ICO has ten transparent PNG layers: 16, 20, 24, 32, 40, 48,
64, 96, 128 and 256 px. The Presence PNG is 1024 px. To regenerate them:

```powershell
npm --prefix tools/icon-assets ci --ignore-scripts
./tools/build-goden-icon.ps1 `
  -AssetOutput packaging/brand/goden-presence-1024.png `
  -ManifestOutput packaging/brand/goden-assets.json
```

Node/sharp/libvips are optional design-time tools, pinned by
`tools/icon-assets/package-lock.json`. They are not installed with the player
or needed for ordinary native builds; existing assets are used instead.
`goden-assets.json` records source, renderer and per-layer/output hashes.
Changing the Presence PNG also requires refreshing the three source hashes in
`packaging/presence-assets-manifest.json`; local hash verification does not
upload anything.

The installer uses `goden.ico` for setup, uninstall, shortcuts and file
associations. The official foobar2000 executable remains byte-identical; its
own window icon is not replaced by a host resource patch.

On 19 October 2026, with the owner's authorization, the approved PNG replaced
Discord's `goden_default`, `goden_daw` and `goden_playing` static assets under
application `1538647960269627472`. The saved portal state was reloaded; all three
1024 px CDN files matched the source byte for byte. The separate general
application icon was also saved and reloaded. Discord's image editor normalized
that icon to 256 px; its decoded CDN rendition was visually checked, without
claiming source-pixel or encoded-byte equality. Public deployment metadata is in
`packaging/presence-assets-manifest.json`. This remote state can change later and
does not prove that every Discord client has refreshed its cache.

No new Discord client-card observation is claimed for the static icons.
Track-cover delivery and its earlier human observations are unchanged. This
deployment does not resolve the similarity/rights question above.
