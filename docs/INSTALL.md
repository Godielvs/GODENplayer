# Install GODENplayer

GODENplayer is a native Windows interface and component set for the pinned
foobar2000 2.25.10 x64 host. Setup prepares that host for you in portable mode.
You do not need an existing foobar2000 installation.

## Get the beta

Download **5.1.0-beta.18-public.12** from the
[GitHub release](https://github.com/Godielvs/GODENplayer/releases/tag/v5.1.0-beta.18-public.12).
Start with `GODENplayer-Beta-Setup-x64.exe`. The release also includes:

- `SHA256SUMS.txt`
- `GODENplayer-public-source.zip` and `GODENplayer-corresponding-sources.zip`
- `QUALIFICATION.md`, with the checks and limitations for that exact installer

Keep the corresponding sources and notices with the installer when sharing.
Read [license scope](../LICENSE-SCOPE.md) before redistribution: technical
packaging does not resolve the recorded permissions.

The installer is unsigned. Compare its SHA-256 to the supplied checksum:

```powershell
Get-FileHash .\GODENplayer-Beta-Setup-x64.exe -Algorithm SHA256
```

A matching checksum checks the received file against the package; it is not a
publisher signature. If security software reports a detection, retain its exact
name and report it with the installer hash. Do not disable antivirus.

## Requirements

- Windows 10 2004 / build 19041 or later, or Windows 11, x64.
- A supported audio device.
- Direct3D feature level 11_0 for the full GPU visuals.
- Enough space for the application, your library database and local caches.

Use a Windows release receiving security updates. The complete official host
includes its required Microsoft runtime. You do not need Python, Node, FFmpeg,
a compiler or a separate Visual C++ Redistributable for ordinary playback.
Optional analysis on Windows N may require the Media Feature Pack.

Native Linux/macOS and Windows ARM builds are not provided. Wine, ARM emulation,
physical multi-monitor DPI/HDR and clean Windows variants have the boundaries
listed in the [compatibility matrix](COMPATIBILITY.md).

## First installation

1. Run the installer and choose the application folder.
2. Select a music folder, or add your library later.
3. Review optional Discord Presence and cover sharing. Automatic covers are
   preselected with disclosure: Uguu first, Catbox as an alternative, then catalogue
   lookup. Uncheck this choice to disable external cover requests. Existing
   configured profiles keep their choices.
4. Optionally install Playback Statistics for star ratings and play counts.
5. Open GODENplayer and let the first library scan finish. Its duration depends
   on file count, tags, embedded artwork and storage.

The installer uses the complete, unchanged official foobar2000 installer locally.
GODEN's managed folder is separate from an unrelated foobar installation.
Do not choose another player's folder or copy individual host DLLs into GODEN.

Playback Statistics is downloaded from its pinned official source rather than
bundled. If the download is unavailable or you are offline, installation can
finish without it. Re-run setup or install the official component manually later.
Existing statistics are preserved during GODEN updates.

## Find your way around

- **Library:** search, narrow by genre/artist/album, open the queue or revisit session history.
- **Album:** cover, tracks, lyrics and visual selection. Use **Panels** to change
  what is visible and to enable Album theater mode.
- **DSP:** playback processing chain and presets.
- **DAW:** temporary sample/stem arrangement and preview workspace.
- **TAG:** batch metadata and artwork editing, with Apply and Undo.
- **Listen:** dedicated audio visualization views.
- **Options:** Appearance, Playback, Lyrics, Discord and Tools sections. General switches save immediately; Discord provider edits use **Apply / Reconnect** in the fixed footer. Technical Discord controls are under **Advanced**.

The initial visual is Nebula 3. Existing visual choices and preferences survive
updates. The demo media shown in the README is an original fictional catalogue;
the installer does not include a music collection.

## Update or recover

Choose the **same GODEN installation root** when updating. A second registered
production root is refused. Setup stages the replacement, preserves the profile
and retains the previous application as `app.previous` for rollback.

Do not overwrite a root containing `app.rollback.pending` or
`install-recovery.pending`. Close the player and follow the transaction log and
[support guidance](SUPPORT.md); do not delete the only preserved profile.

Uninstallation must complete a profile backup before deletion can proceed.
A failed backup is marked `.partial` and does not replace a completed backup.
Cancelling an uninstall after the backup can leave that extra backup in place.

## Online features and privacy

Your library, playlists, preferences and analysis caches are stored locally.
Discord Presence, cover uploads and online lyrics are optional external features.
Cover uploads send a newly encoded thumbnail, not your music file or its path.
Public uploads are not ephemeral and cannot be recalled by disabling the feature.
See [privacy and external services](PRIVACY.md) for providers, cache behavior,
relay options and offline lyrics.
