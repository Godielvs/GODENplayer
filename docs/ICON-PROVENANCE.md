# GODENplayer icon

The canonical source is `packaging/brand/goden.svg`: vector paths with angular
eyes, tapered cheeks, a short mouth and a rose-to-cyan outline. The header pixel
wordmark is a separate asset.

The design retains a close visual reference to the preceding icon, which was
made from foobar2000 artwork. Newly drawn paths do not establish independent
rights to a similar design. The [foobar2000 license](https://www.foobar2000.org/license)
credits its icons separately; this project has not obtained an adaptation grant.
The similarity/rights question remains open. The root code MIT license does not
license the icon or third-party artwork. No trademark clearance or endorsement
is claimed.

## Reproduction and use

The ICO contains transparent PNG layers at 16, 20, 24, 32, 40, 48, 64, 96, 128
and 256 px. The Presence PNG is 1024 px. Regenerate them with:

```powershell
npm --prefix tools/icon-assets ci --ignore-scripts
./tools/build-goden-icon.ps1 `
  -AssetOutput packaging/brand/goden-presence-1024.png `
  -ManifestOutput packaging/brand/goden-assets.json
```

Node/sharp/libvips are optional asset tools, pinned by
`tools/icon-assets/package-lock.json`. They are not shipped with the player or
needed for ordinary native builds. `goden-assets.json` records source, renderer
and output hashes.

The installer uses `goden.ico` for setup, uninstall, shortcuts and file
associations. The official foobar2000 executable is not patched.

Presence asset keys and deployed hashes are recorded in
`packaging/presence-assets-manifest.json`. Discord normalizes the general
application icon to 256 px; that rendition is separate from the original
1024 px static assets. Regenerating local files does not upload them to Discord.
