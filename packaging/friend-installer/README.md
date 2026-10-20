# GODENplayer Beta installer

This package builds a clean, independent, per-user GODENplayer installation at
`%LocalAppData%\Programs\Goden`. The setup embeds and runs the official,
unmodified foobar2000 installer; it does not redistribute a patched host EXE.

The builder accepts exactly three product inputs:

1. a frozen `goden.release-manifest.v2`;
2. a qualified `goden.friend-staging.v1` directory containing the explicit
   profile allowlist; and
3. the pinned official foobar2000 installer.

It never reads a live GODENplayer/foobar profile. The pristine seed contains a
host-generated baseline `config.sqlite`, an empty DSP graph, visual settings,
the native Columns UI layout and the six managed components. It excludes
libraries, playlists, history, ratings, lyrics, rejection records, caches,
credentials, editing backups and user paths. OpenLyrics is intentionally not
installed on a clean profile, while an existing user's copy is preserved during
an upgrade.

Build example:

```powershell
.\tools\build-friend-installer.ps1 `
  -PayloadStaging C:\release-inputs\goden-friend-staging `
  -FoobarInstaller C:\release-inputs\foobar2000-x64_v2.25.10.exe `
  -ReleaseManifest .\packaging\component-manifest.json `
  -OutputDirectory C:\release-output
```

The version is read only from the release manifest. Every staging and final
payload file is checked by byte length and SHA-256 before Inno Setup runs.
The clean profile seed is also checked semantically: it must select Columns UI,
contain no null configuration values, and carry a Columns UI file emitted by
the pinned native runtime. Seed revision 2 repairs the incomplete clean profile
from the earlier C4 installer while retaining its library and playlists.

Clean installs may start with no library folder. Interactive setup enables Rich
Presence and preselects album-cover delivery with its public-upload disclosure;
users may uncheck it before continuing. Silent clean installs disable covers
unless explicitly passed `/ALLOW_PUBLIC_COVERS=1`. Existing profiles keep their saved choices. Audio output
uses the host's system default and the shipped DSP graph is empty.

Upgrades retain the transactional `app.next -> app -> app.previous` swap and
preserve the live user profile. Uninstall always moves the profile to a
recoverable location before removing binaries and stops if that move fails.

The installer writes only the `Goden.Audio` ProgID and
`Goden.GODENplayer.x64` AUMID. It never writes generic foobar App Paths,
Applications or UserChoice keys. Registry values have an immutable first-install
baseline and are restored only while their current value still equals the value
owned by the installer.
