# Changelog
## Public preparation candidate 13
- Add independent Discord cover options: temporary Uguu, personal-key ImgBB, catalogue artwork lookup and Automatic, alongside Catbox and a custom relay. New setup recommends Automatic with explicit permission; existing preferences and legacy Catbox consent are preserved.
- Reuse enabled-provider caches before uploads, bound retries and recovery, renew temporary links, and retain cover results across Discord reconnection. Show clearer provider and failure status without interrupting text Presence or playback.
- Organize Options into Appearance, Playback, Lyrics, Discord and Tools. Keep Apply / Reconnect accessible, put technical Discord controls under Advanced, preserve edits across navigation and warn before discarding unapplied settings.
- Add provider, consent, retry/reconnect, Windows credential protection and Options layout regression coverage. Preserve the approved audio, shaders, visual quality and presentation cadence.

## Public preparation candidate 12
- Keep the shared artwork palette while Album content is loading, adopt cached artwork when content becomes ready, and resynchronize on restoring Album. This fixes the interface turning gray or retaining the preceding album's colours while the visual already shows the new palette.
- Add a production-method palette event oracle covering loading snapshots, cache hits, stale events, hidden views, missing artwork and restoration. The corrected source passes 13 checks; the preceding source fails 7 of them.
- Present Library, Album, high-refresh scope/Spectrum, DSP, DAW, TAG, queue, history, ratings and visual choices in a revised README with a dedicated installation guide.
- Include two native screenshots, a 36-second video and an optional GIF, with original demo music, lyrics and cover art; retain exact capture provenance and optional generation recipes.
- Preserve audio processing, shader design, quality, cadence and saved settings. This local preparation does not publish to GitHub or update a personal installation.

## Public preparation candidate 11
- Serialize database initialization across connections and processes using bounded ownership keyed to SQLite's actual file identity. This prevents startup workers from applying migrations from a stale schema version; ordinary database operations keep their existing concurrency.
- Add concurrent thread/process initialization, persisted-value, integrity, timeout and abandoned-owner regression coverage. Retain existing migration backup, rollback and data-preservation checks.
- Correct the first-boot test to use the pinned host's advertised /show argument, require a non-minimized workspace and check completed startup import. A same-installer experiment passed without the prior /new argument; retain the earlier failed attempts separately from the verified database race and exact public.11 outcomes.
- Keep the current visuals, audio, quality, cadence and saved choices. No personal promotion or GitHub publication is performed.

## Public preparation candidate 10
- Synchronize the README, architecture, qualification and beta publication scope with the independent Kerr renderer and the current candidate identity.
- Correct installer QA to verify the persisted Presence preference instead of reporting an assumed default, and retain separate first-boot and rollback diagnostic receipts.
- Bind the matching rebuilt installer and corresponding sources to their exact file transactions and runtime observations. Preserve earlier timing stalls and hardware/capture limitations; only the matching generated reports identify newly completed checks.
- Preserve public.9 runtime behavior, visual programs, quality, cadence, defaults and profile migration. No personal installation promotion or GitHub publication is performed.

## Public preparation candidate 9
- Move Kerr-Newman Original and Music to an independent native image program with value-only IPC and a final shared GPU image; the program has no foobar2000 SDK dependency.
- Retain all ten Kerr shader sources/programs, formulas and passes. Deterministic integration produced 61 byte-identical reference images across Original/Music, pause/resume, hiding, resize and track changes.
- Record direct Music-mode UI confirmation and complete-player measurements separately from GPU fixtures. Uncontrolled repetitions include stalls in both the candidate and baseline; no global performance pass or optimization gain is claimed.
- Keep the remaining contribution permissions and other shader rights questions explicit.

## Public preparation candidate 8
- Record the owner's completed B / Angular upload for the three existing Discord Presence keys and the separate general application icon. Static asset CDN files match the approved source; the portal normalized the general icon to a 256 px rendition.
- Synchronize deployment and publication documentation while retaining source, installer and runtime qualification boundaries. Only the native candidate version identity changes; audio, visuals, quality, cadence and cover delivery are unchanged.
- Keep the frozen public.7 delivery intact. Publication, personal installation promotion and resolution of remaining rights questions are not part of this revision.

## Public preparation candidate 7
- Adopt the owner's B / Angular vector icon for installer, shortcuts and prepared Presence assets, preserving the approved geometry.
- Replace the obsolete foobar-image extraction recipe with optional pinned SVG tooling and record source, layer and output hashes. Native runtime and the official host executable remain unchanged apart from the candidate version identity.
- Document the visual reference and unresolved similarity/rights question; Discord static asset upload remains a separate external step.

## Public preparation candidate 6
- Validate complete bounded PNG structure at both Catbox origin and delivery before trusting a new cover; preserve acknowledged URLs even if delivery fails.
- Persist bounded encoding progress for damaged stored objects, avoid redundant uploads, and retain cancellation, retry limits, repair guard and text-only fallback.
- Add origin-versus-proxy corruption regression coverage with real WIC and production provider/relay methods. Generated live covers were decoded at both endpoints; a human confirmed the new cover in Discord after restart on the preceding native build with the same cover implementation.
- Correct installer runtime qualification tooling and consolidate bounded evidence. The exact new delivery report identifies the rebuilt installer, sources and transaction results; physical display, clean-OS, full-player performance and rights limits remain explicit.

## Public preparation candidate 5
- Add Follow Windows motion preference for fresh profiles; preserve existing Normal/Reduced/Off selections and react to Windows animation settings without polling.
- Apply monitor-specific DirectWrite parameters to native panels, Workspace and Listen; respect Windows font-smoothing choices and rebuild retained text rasters after changes.
- Preserve the staging copy failure code in installer errors/logs; verify failed copying leaves the live application and profile unchanged.
- Add production-path motion, real DirectWrite/WIC raster/cache and actual Inno staging-copy fixtures. Physical displays, clean target OS and full-player runtime/performance remain separate qualification gates.

## Public preparation candidate 4
- Request official portable-host mode before installation, verify its marker and retire the legacy nested host uninstaller.
- Guard a single registered GODEN root; qualify Unicode/spaced destinations and snapshot unrelated foobar registrations and shortcuts.
- Audit ordinary/delay import closure against the complete official runtime, without relying on developer-machine redistributables.
- Stop continuous unsupported-adapter retries while retaining topology wake and transient-error recovery.
- Retry cover GET transport failures through Windows automatic proxy settings without redundant uploads or provider changes.
- Fit exhausted shell bands into small/high-DPI windows, including actual zero-height child-window placement.
- Report optional analysis worker loader failures with actionable causes and exit codes.
- Document platform, security, display and accessibility boundaries. Physical multi-monitor, clean-OS, Wine/ARM and full-player performance qualification remain separate.

## Public preparation candidate 3
- Final review across sixteen areas; retain the approved visuals and visible presentation cadence.
- Start artwork/GlyphFlow time on accepted frames; keep rejected final frames pending and schedule cover animation in the tabs that draw it.
- Cancel superseded Library/Album work, suspend hidden lyrics timers, and play History entries by file/subsong rather than ambiguous tags.
- Reject analysis results after the source changes, including changes while a cache write is queued.
- Harden DSP stale-graph exchange and double-clear, precompute utility coefficients, and omit undemanded gain-reduction telemetry.
- Reset Cloud Bay/history on track changes during freeze; recover partial Spectrum surfaces, respect root cloaking and bound WaveSeek preparation retries.
- Preserve explicit ambient preferences; harden provider redirects, relay malformed-response completion, host downloads and public-source staging/privacy.
- Bind effective dependency inputs and native build snapshots to their receipts; include all eighteen Spectrum patches and generated headers.
- Add actual-engine, production-completion and Direct2D/WIC regression probes. Full player/DPI/Presence and matched performance remain separate qualification gates.

## Public preparation candidate 2
- Remove obsolete Homemade Robot notices: the shipped wordmark uses hand-authored pixel geometry.
- Fix WIC/COM destruction order during cover encoding and bound delivery-verification cache entries alongside the URL cache.
- Add deterministic production cover-provider and real installer download failure fixtures.
- Select declared compiler, SDK, CMake/Ninja and vendored Eigen explicitly; distinguish functional CI checks from isolated performance qualification.
- Include the verified NPGS GPL-3.0 license and document the remaining shader permission/integration questions.

## Public preparation candidate
- Separate source export from the complete approved dirty laboratory state; private checkpoint retained outside the export.
- Explicit Catbox/private/off cover policy, disclosure, DPAPI private credentials, strict relay origin handling, durable origin cache and bounded retry/renewal work.
- Unchanged official foobar2000 host; bounded Columns UI PMv2 changes.
- Optional official Playback Statistics 3.1.11 download with package/x64 pins; offline completion and existing statistics preservation.
- Production QA mutation/fault gate; targeted unused flag/API, disabled DSP and retired Clovis state cleanup.
- Restored Tile attribution; removed inactive Radiance fib and independently expressed its small HSV converter.
- Own-code MIT scope, dependency/source/license inventory and explicit pending shader/icon rights.

This entry describes implementation scope, not a claim that every qualification gate has passed. See docs/QUALIFICATION.md.
