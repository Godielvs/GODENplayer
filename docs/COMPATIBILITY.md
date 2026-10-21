# Compatibility and other computers

This is the compatibility contract retained by local candidate
**5.1.0-beta.18-public.12**. A declared target is not a test result. Consult the
matching delivery's qualification report and hashes. Disposable-host runtime
observations are distinguished from silent transactions; the personal
installation is not promoted by this preparation.

## Platform matrix

| Environment | Contract and evidence | Remaining qualification |
| --- | --- | --- |
| Windows 10 2004 / build 19041 or later, Windows 11, x64 | Declared minimum; installer gate and PE import inspection. Local build and silent transactions run on one Windows PC. Use a Windows release receiving security updates. | Clean OS/VM runs on representative Windows versions; this is not certification of every build. |
| Windows N | Basic player and visuals do not import Media Foundation. The isolated analysis worker does. Missing Media Foundation is now reported with the worker exit code and Media Feature Pack guidance. | Actual Windows N with and without the pack; no claim that optional analysis works without it. |
| Windows 11 ARM64 | Installer permits x64-compatible execution. Microsoft provides x64 emulation on Windows 11 Arm. | No native ARM build or emulation qualification. Windows 10 Arm is outside this x64 target. |
| Windows 7/8, 32-bit Windows | Outside the supported installer target. | No promise of compatibility. |
| Linux/Wine, Proton, macOS compatibility layers | No native build. Dependencies include D3D11, Direct2D, DirectWrite, WIC/COM, Media Foundation and Win32 process/job controls. | Unqualified. Installing or reaching the UI alone does not establish audio, visuals, analysis or DPI support. Proton is primarily a Steam game compatibility tool. |

Microsoft documents [Arm emulation](https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation),
[Media Feature Pack for N editions](https://support.microsoft.com/en-us/windows/experience/platform-variants/media-feature-pack-for-windows-n),
and [nested job constraints](https://learn.microsoft.com/en-us/windows/win32/procthread/nested-jobs).
A sandbox or managed launcher with incompatible job restrictions can prevent
optional workers from starting. GODEN retains its containment rather than
silently weakening it. Report the launch error and retry outside that launcher.

## Prerequisites and damaged installations

Normal playback needs no Python, Node, compiler, ffmpeg or separately installed
Visual C++ Redistributable. The complete official foobar2000 2.25.10 x64 host
contains the app-local Microsoft runtime required by Spectrum and the host.
Do not copy only `foobar2000.exe`, remove its DLLs, or mix host versions.

`tools/audit-runtime-dependencies.py` parses ordinary and delay-loaded imports
recursively against the delivered host and six managed binaries. It does not
use this developer PC's installed redistributables to satisfy a missing DLL.
The installer QA additionally compares the official runtime files byte for
byte. This is distribution closure evidence, not a clean-OS execution test.

Optional analysis now distinguishes missing DLLs (`0xC0000135`), invalid image
or architecture (`0xC000007B`), unsupported instructions (`0xC000001D`) and DLL
initialization failure (`0xC0000142`). A missing DLL can also mean quarantine or
damage, not necessarily Windows N. Repair from a verified installer and retain
the error code; do not download individual DLLs from third-party DLL sites.

Build tools remain development prerequisites. The standalone CRT export tool
uses ffmpeg/ffprobe when invoked for export and currently requires an ordinary
Windows temporary path; extended-length TEMP paths are not qualified. Neither
is a normal playback prerequisite.

## Graphics, displays and accessibility

- Full GPU visuals request D3D feature level **11_0 or 11_1**, not merely a
  driver exposing a D3D11 API. A hardware device is preferred; WARP is the
  software fallback and can cost more CPU. Basic UI has software fallbacks.
- An adapter reporting `DXGI_ERROR_UNSUPPORTED` no longer receives continuous
  hardware creation attempts while WARP is serving the UI. Topology changes
  wake it. Transient failures retain bounded retries. The real device-hub
  fixture covers both, plus display/device messages and hardware recovery.
- HDMI versus DisplayPort alone does not establish a cause. Compare actual
  resolution, scale, refresh, driver, adapter selection and HDR settings.
  Scope/Spectrum cadence and visual quality were not reduced by this pass.
- Output is SDR; float intermediate textures do not constitute an HDR output
  pipeline. HDR, Remote Desktop/virtual adapters, multiple GPUs, old drivers
  and physical mixed-DPI migration remain separate observation targets.
- Layout probes cover 96-384 DPI, small/zero-sized windows, portrait, ultrawide
  and 8K geometry. A second fixture calls the real panel placement methods on
  hidden Windows child windows, including shrinking exhausted bands to zero.
  This does not verify physical text appearance, clicks or monitor migration.
- Native panels, Workspace and Listen apply DirectWrite rendering parameters
  for the current monitor, including the RGB/BGR geometry reported by Windows.
  Disabled font smoothing selects aliased text; standard smoothing, unknown
  geometry or unavailable settings use grayscale. Settings and monitor changes
  invalidate retained text rasters. Steady frames reuse the parameters.
  DirectWrite/WIC tests verify routing, caching and grayscale pixels; physical
  RGB/BGR, rotated and OLED panels remain unqualified. Windows cannot reliably
  identify every physical subpixel layout; this is not automatic OLED detection.
- Motion offers **Follow Windows** in OPTIONS and View > GODENplayer > Motion.
  New profiles use it; existing Normal/Reduced/Off selections stay explicit.
  When Windows client-area animations are disabled, Follow Windows selects
  the existing Reduced behavior; query failure falls back to Normal. Changes
  are event-driven and do not overwrite saved preferences. This governs
  nonessential UI motion, not a global playback/visualizer refresh reduction.
- Windows High Contrast colors are not integrated across all custom surfaces.
  This remains an accessibility limitation; motion and text work do not imply
  complete screen-reader, keyboard or High Contrast qualification.

See Microsoft's [feature levels](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-downlevel-intro)
and [WARP documentation](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp).

## Coexistence, paths and installation

The outer installer is per-user, defaults to LocalAppData and creates its own
portable foobar host. It now requests official **`/P` portable mode from the
start**, verifies the host-created marker and retires a legacy nested host
uninstaller before committing an upgrade. The official executable is unchanged.
The NSIS `/D=` argument intentionally remains last and unquoted, as required by
the [NSIS command-line contract](https://nsis.sourceforge.io/Docs/Chapter3.html).

An unrelated foobar installation and its profile are not upgrade targets. GODEN
registers its own shortcuts and `Goden.Audio` identity; it does not overwrite
generic foobar registrations or Windows `UserChoice`. Explicit default-app
changes made by the user in Windows are separate choices. Uninstall restores
its saved registration baseline. Production allows one registered GODEN root per
user: choose that root to update it, or uninstall it before choosing a second
root. This avoids shared-registration restoration pointing at a removed root.

Silent QA uses disposable roots, no production GODEN registration and no desktop
input. Its headless mode does not launch the player; a separate runtime mode
launches the disposable official host and uses read-only native probes and
supported command-line controls. It snapshots the relevant foobar registry entries
and named shortcuts, plus common audio extensions and their `UserChoice`, and
checks the observed state remains unchanged. This does not claim an
exact snapshot of all Windows state or interactive association prompts.
Paths with spaces, accents and Japanese characters are exercised. Arbitrarily
long paths, protected folders, network shares, removable media and low disk
space still need dedicated qualification. The build checkout has stricter
path rules than the installed application; see [BUILD.md](BUILD.md).

If staging an update fails, the installer now retains the Robocopy exit code
in its error and logs source/destination paths locally. Check available space
and folder permissions; a code alone does not establish the cause. A silent
fixture exercises copying, exclusions, an unchanged copy and an inaccessible
destination beneath a regular file, checking live/profile bytes are preserved.
This does not simulate disk exhaustion, antivirus quarantine or power loss.

## Networks, security software and support

Text Presence and playback do not depend on a cover service succeeding.
Catbox/wsrv cover verification now retries a transport failure once through
Windows automatic proxy settings, including a timeout after HTTP headers.
It keeps the same URL and does not issue another upload. HTTP/content failures
do not trigger that fallback. The chosen provider never silently changes.
TLS verification remains enabled. See [PRIVACY.md](PRIVACY.md).

New or unverified Catbox covers require bounded, complete PNG structure at both
origin and delivery; a valid proxy image alone cannot conceal a damaged stored
origin. Acknowledged URLs and encoding progress survive restart. Verified
deliveries now persist a URL-bound SHA-256 fingerprint: after restart, a complete
CDN response matching that fingerprint can survive an origin outage. Legacy or
acknowledged-only rows still require healthy origin and delivery checks first.
Public network failures get at most two slow recovery attempts beyond the four
ordinary attempts; metadata and RPC reconnects cannot reset the budget. Repair
remains bounded and may stay text-only until a later admitted/revisited request.
Generated live images were fully decoded and compared at both endpoints, and a
human confirmed a new cover in Discord after restart on an earlier native
candidate. The persistent-proof and slow-recovery changes are covered by native
fake-transport and extracted-worker tests; new live upload/Discord qualification
is pending while the origin refuses connections. This is bounded evidence, not an
availability guarantee or comprehensive network/Discord qualification.

Proxy authentication, TLS inspection, DNS filtering, CDN delay, offline operation,
firewalls and clock skew can affect external services. Fake-transport tests
cover bounded failure/recovery; they do not replace corporate-network testing.
The separate human Discord observation above covers one generated image.
Keep Windows time and trust stores current; do not disable
certificate checking or firewall protections as a workaround.

The GODEN installer and managed binaries are **not Authenticode signed**.
The embedded official foobar installer is separately signature/hash verified.
SmartScreen reputation warnings or antivirus quarantine can occur. Check the
matching SHA-256 manifest and exact detection/protection history, then report
the detection and Windows error. Do not disable antivirus globally or create
broad exclusions. Microsoft describes the distinction between file reputation
and signing in [SmartScreen documentation](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/).

Report version/hash, Windows edition/build/architecture, GPU and driver,
display scale/refresh, storage, relevant security/network software and steps.
Keep credentials, private URLs, music and full personal profiles out of reports.
See [SUPPORT.md](SUPPORT.md). No matched full-player performance result is
claimed from this compatibility pass.

### Additional Presence cover choices

New public setup offers Automatic (Uguu, Catbox, catalogue), with versioned disclosure; legacy Catbox markers and configured profiles retain their narrower scope. Options can explicitly enable Uguu, ImgBB (a personal API key), catalogue lookup or an Automatic allowlist. Upload caches are isolated per host. Automatic never enables unchecked hosts or private relay. Uguu links carry conservative expiry and renewal; catalogue lookup is bounded and rate-limited. Native deterministic fixtures cover fallback, cache restart, expiry, cancellation, malformed JSON and provider boundaries. These fixtures do not establish live host or Discord availability; consult the candidate qualification report for network/client observations. No new runtime helper process is required.
