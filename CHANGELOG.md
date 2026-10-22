# Changelog

## 5.1.0-beta.18-public.13
- Add Automatic, temporary Uguu, personal-key ImgBB and catalogue artwork options alongside Catbox and custom relays. Existing provider and consent preferences are preserved.
- Reuse provider caches, renew temporary links and recover covers across Discord reconnection with clearer status messages.
- Organize Options into Appearance, Playback, Lyrics, Discord and Tools; preserve edits while navigating and warn before discarding changes.

## 5.1.0-beta.18-public.12
- Fix an Album palette bug that could leave the interface gray or showing the previous cover's colours after the visualizer switched to new artwork.
- Add installation documentation, screenshots and an audiovisual demo with original media.

## 5.1.0-beta.18-public.11
- Serialize database initialization across connections and processes to prevent competing schema migrations at startup.
- Expand database integrity, migration, timeout and recovery tests.

## 5.1.0-beta.18-public.10
- Improve installer first-boot, preference-preservation and rollback checks, with separate reports for file transactions and runtime observations.

## 5.1.0-beta.18-public.9
- Run Kerr-Newman Original and Music in an independent native renderer with bounded IPC and shared GPU images, retaining the existing shader programs and visuals.

## 5.1.0-beta.18-public.8
- Update Discord application and fallback artwork.

## 5.1.0-beta.18-public.7
- Update the application icon for setup, shortcuts and Presence; add reproducible SVG asset tooling.

## 5.1.0-beta.18-public.6
- Validate complete cover PNGs at upload origin and delivery, retain acknowledged URLs and bound repair attempts.
- Improve cover encoding and restart-cache regression coverage.

## 5.1.0-beta.18-public.5
- Follow Windows animation preferences for new profiles while preserving explicit motion settings.
- Apply monitor-specific text-rendering parameters and respect Windows font-smoothing preferences.
- Preserve the installed application and profile when staging a copy fails.

## 5.1.0-beta.18-public.4
- Improve portable installation isolation, Unicode paths, runtime dependency checks and constrained-window layouts.
- Bound unsupported-adapter retries and improve proxy recovery and analysis-worker diagnostics.

## 5.1.0-beta.18-public.3
- Improve artwork-transition timing, cancel superseded library/artwork work and suspend hidden lyrics timers.
- Resolve History entries by file/subsong and reject stale analysis results.
- Harden DSP graph exchange, renderer state recovery, network responses and source-package validation.

## 5.1.0-beta.18-public.2
- Fix cover-encoding COM teardown and bound delivery-verification cache entries.
- Pin build tools and dependencies and expand download-failure tests.

## Initial beta
- Prepare the native player, independent official foobar2000 installation and profile-preserving installer.
- Add reproducible source delivery, dependency inventories, privacy controls and third-party credits.

See [qualification and coverage](docs/QUALIFICATION.md) and the matching release
reports for validation results and limitations.
