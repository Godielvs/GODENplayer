param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$spectrum = Join-Path $repo `
    'out\third_party\spectrum-analyzer\foo_vis_spectrum_analyzer'

$frameClockPath = Join-Path $repo 'src\component\frame_clock.cpp'
$frameClockSource = Get-Content -LiteralPath $frameClockPath -Raw
if ($frameClockSource -notmatch '(?s)void deadlinesChanged\(\) noexcept\s*\{.*?lock\(mutex_\);\s*armNextLocked\(\);\s*\}') {
    throw 'Presentation coordinator logical-deadline rearm contract is missing'
}
$requestAtOrAfterSource = [regex]::Match(
    $frameClockSource,
    '(?s)bool WindowFrameClock::requestAtOrAfter\(.*?(?=bool WindowFrameClock::consume\()').Value
if ($requestAtOrAfterSource.Contains('presentationCoordinator().environmentChanged();')) {
    throw 'requestAtOrAfter still rebases every presentation clock'
}
$completeOpportunitySource = [regex]::Match(
    $frameClockSource,
    '(?s)void WindowFrameClock::completePresentationOpportunity\(\s*bool needsAnotherFrame,\s*std::uint64_t.*?(?=void WindowFrameClock::disarmReadinessWait\()').Value
if ($completeOpportunitySource.Contains('presentationCoordinator().environmentChanged();')) {
    throw 'completePresentationOpportunity still rebases every presentation clock'
}
if ($frameClockSource -notmatch 'deadlinesChanged\(\);') {
    throw 'Logical scheduling does not notify the deadline-only coordinator path'
}
$nativeSurfaceSource = Get-Content -LiteralPath `
    (Join-Path $repo 'src\component\native_d2d_surface.cpp') -Raw
$nativeResize = [regex]::Match($nativeSurfaceSource,
    '(?s)bool NativeD2DSurface::resize\(\).*?(?=FrameResult NativeD2DSurface::present)').Value
if ($nativeResize.Contains('++swapchainEpoch_')) {
    throw 'ResizeBuffers still changes swapchain identity'
}
$crtSource = Get-Content -LiteralPath `
    (Join-Path $repo 'src\component\crt_xy_surface.cpp') -Raw
$crtResize = [regex]::Match($crtSource,
    '(?s)void CrtXySurface::setViewport\(.*?(?=void CrtXySurface::setActive)').Value
if ($crtResize.Contains('CloseHandle(frameLatencyHandle_)') -or
        $crtResize.Contains('GetFrameLatencyWaitableObject()')) {
    throw 'CRT resize still destroys/requeries the stable latency handle'
}

function Require-Text(
    [string]$Path,
    [string[]]$Needles,
    [string]$Contract) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Contract input is missing: $Path"
    }
    $text = Get-Content -LiteralPath $Path -Raw
    foreach ($needle in $Needles) {
        if (-not $text.Contains($needle)) {
            throw "$Contract is incomplete: $needle"
        }
    }
    return $text
}

$direct2d = Require-Text `
    (Join-Path $spectrum 'Windows\Direct2D.cpp') `
    @('MonitorFromWindow', 'description.Monitor == wantedMonitor',
      'QueryDisplayConfig', 'path.targetInfo.refreshRate.Numerator') `
    'Spectrum monitor/refresh selection'
if ($direct2d.Contains('EnumOutputs(0')) {
    throw 'Spectrum still binds presentation cadence to adapter output 0.'
}

$spectrumUi = Require-Text `
    (Join-Path $spectrum 'UIElement.cpp') `
    @('Goden.DisplayEnvironmentChanged.V1',
      '_DisplayEnvironmentDirty.store(true',
      '_DisplayMonitor.exchange') `
    'Spectrum runtime display transition'
$spectrumScheduler = Require-Text `
    (Join-Path $spectrum 'UIElementRendering.cpp') `
    @('GetAnalysisFrameTime', 'GetPresentationFrameTime',
      'std::min)(ConfiguredRefreshRate, _DisplayRefreshRate)',
      'RefreshDisplayEnvironment') `
    'Spectrum analysis/presentation separation'

Require-Text (Join-Path $repo 'src\component\chrome.cpp') `
    @('Goden.DisplayEnvironmentChanged.V1',
      'publishRootDisplayEnvironment(window, true)',
      'case WM_WINDOWPOSCHANGED:') `
    'Root display broadcast' | Out-Null
Require-Text (Join-Path $repo 'src\component\render_device_hub.cpp') `
    @('isDeviceCompatibleWithWindow(',
      'currentDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE',
      'snapshot(window).adapterLuid',
      'windowIdentity(window)',
      'identity.topologyGeneration',
      'identity.rootLifetimeToken',
      'identity.windowPresentationRevision',
      'compatibility_.push_back') `
    'Cross-adapter render-device selection' | Out-Null
Require-Text (Join-Path $repo 'src\component\crt_xy_surface.cpp') `
    @('lastTopologyGeneration_', 'presentationPeriodTicks(',
      'now - lastPresentQpc_', 'nextPresentQpc_ = 0',
      'currentMonitor != renderMonitor_',
      'isDeviceCompatibleWithWindow(') `
    'CRT rational refresh/rebase' | Out-Null
$daw = Require-Text (Join-Path $repo 'src\component\daw_window.cpp') `
    @('WindowFrameClock frameClock', 'kFrameClockMessage',
      'frameClock.setLatencyCritical(true)', 'frameClock.consume(wParam)') `
    'DAW refresh-aware presentation'
if ($daw.Contains('SetTimer(window, kFrameTimer')) {
    throw 'DAW still uses the fixed 16 ms presentation timer.'
}
Require-Text (Join-Path $repo 'src\component\listen_visual_renderer.cpp') `
    @('DYNAMICS', 'dynamicsHoldRect_', 'state.model->dynamics') `
    'Listen Dynamics panel' | Out-Null
Require-Text (Join-Path $repo 'src\component\listen_visual_renderer.cpp') `
    @(
      'currentMonitor != renderMonitor_',
      'renderRecoveryEpoch_ != recoveryEpoch') `
    'Listen display/adapter migration' | Out-Null
$listenWindow = Require-Text `
    (Join-Path $repo 'src\component\listen_window.cpp') `
    @('ArtworkTransitionRenderer starts its semantic clock after accepting',
      'syncFrameClock();') `
    'Listen static-view artwork presentation'
if ($listenWindow -notmatch '(?s)void ListenWindow::paint\(\).*?renderer_\.draw\(.*?syncFrameClock\(\);\s*\}') {
    throw 'Listen paint does not re-arm its semantic artwork frame clock.'
}
Require-Text (Join-Path $repo 'src\component\workspace_panel.cpp') `
    @('Goden.DisplayEnvironmentChanged.V1',
      'adapterCheckRequired_ = true',
      'currentMonitor != renderMonitor_',
      'isDeviceCompatibleWithWindow(') `
    'Workspace display/adapter migration' | Out-Null
Require-Text (Join-Path $repo 'src\component\ui_preferences.cpp') `
    @('void initializeAlbumAmbientPreference()',
      'sanitizedAlbumAmbient(static_cast<unsigned>(g_albumAmbientMode))',
      'g_albumAmbientReactiveDefaultMigrated',
      'AlbumAmbientMode::reactive') `
    'Album Ambient persisted preference restore' | Out-Null
Require-Text (Join-Path $repo 'src\component\rollout_flags.cpp') `
    @('const auto defaultOff = flag == RolloutFlag::tagImportV1;') `
    'Album Ambient default-on rollout' | Out-Null
Require-Text (Join-Path $repo 'src\component\component.cpp') `
    @('initializeAlbumAmbientPreference, init_stages::after_config_read') `
    'Album Ambient startup preference restore' | Out-Null

$panelD2D = Require-Text (Join-Path $repo 'src\component\panel_d2d.cpp') `
    @('drawingRetained_ = native_.active();',
      'if (ensurePartialBacking()) {',
      'partialBackingValid_ = SUCCEEDED(result);',
      'bool PanelD2D::retainedContentsReady(HWND window)') `
    'Retained native full-frame backing'
if ($panelD2D.Contains(
        'drawingRetained_ = native_.active() && partialBacking_ != nullptr;')) {
    throw 'First native full frame can still bypass the retained backing.'
}
$nativePanels = Require-Text `
    (Join-Path $repo 'src\component\native_panels.cpp') `
    @('render_.retainedContentsReady(window)',
      'Promote that monitor tick to a complete frame immediately.',
      'monitorOnly = false;',
      'began = render_.begin(window, paintStruct.hdc);') `
    'Header partial-paint recovery'

$playerShell = Require-Text `
    (Join-Path $repo 'src\component\player_shell_panel.cpp') `
    @('if (mode_ == PlayerShellMode::album) {',
      'frameClock_.setReadinessBinding(readiness);',
      'frameClock_.beginPresentationOpportunity();',
      'Library''s partial navigation frame is intentionally frame-clock',
      'frameClock_.setReadinessBinding({});',
      'void redrawNavigationNow() const noexcept',
      'InvalidateRect(window_, &navigation, FALSE);',
      'if (mode_ == PlayerShellMode::normal) {',
      'redrawNavigationNow();',
      'UiInvalidationBus::instance().registerWindow(',
      'UiInvalidationSurface::nativePanels,',
      'window_, kShellRefreshMessage',
      'Album owns no playback child that can invalidate its parent.',
      'frameClock_.completePresentationOpportunity(') `
    'PlayerShell palette-transition readiness lifecycle'
if ($playerShell.Contains('kNavigationThemeWakeTimer')) {
    throw 'Navigation still has a competing 10 ms timer.'
}
$navigationDamage = [regex]::Match($playerShell,
    '(?s)void redrawNavigationNow\(\) const noexcept\s*\{(.*?)\n    \}').Value
if (-not $navigationDamage -or $navigationDamage.Contains('RDW_UPDATENOW')) {
    throw 'Navigation must queue narrow damage without recursive synchronous painting.'
}
if ($playerShell -notmatch
        'else\s*\{(?s:.*?)frameClock_\.setReadinessBinding\(\{\}\);(?s:.*?)paintNavigation') {
    throw 'Library navigation is still coupled to Album readiness pacing.'
}
Require-Text (Join-Path $repo 'src\component\album_mode_renderer.cpp') `
    @('const auto theme = ThemeService::instance().snapshot();',
      'albumActive ? secondary : accent',
      'albumActive ? accent : secondary',
      'if (!render_.beginPartial(window, dc, dirty)) {',
      'return render_.lastFrameOutcome();',
      'const auto status = render_.end();',
      'onResourceEpochChanged(render_.resourceEpoch());') `
    'Atomic navigation palette sample' | Out-Null

$surfaceContracts = Require-Text `
    (Join-Path $repo 'src\component\render_surface_contracts.h') `
    @('struct SurfaceBindingKey', 'std::uint64_t lifetime',
      'std::uint64_t swapchainEpoch', 'HANDLE source') `
    'Surface lifetime/epoch identity'
$frameClock = Require-Text (Join-Path $repo 'src\component\frame_clock.cpp') `
    @('if (readinessBinding_ == binding) return;',
      'WaitForThreadpoolWaitCallbacks(wait, TRUE)',
      'activeOpportunityGeneration_ !=',
      'setReadinessBinding(') `
    'Idempotent readiness binding and stale callback fencing'
foreach ($consumer in @('listen_window.cpp', 'player_shell_panel.cpp')) {
    $consumerText = Get-Content -LiteralPath (
        Join-Path $repo "src\component\$consumer") -Raw
    if ($consumerText.Contains('setReadinessHandle(')) {
        throw "Legacy HANDLE-only readiness binding remains in $consumer."
    }
}

$transport = Require-Text (Join-Path $repo 'src\component\native_panels.cpp') `
    @('std::chrono::milliseconds(200)',
      'Runtime::instance().presentationPlaybackPosition()',
      'presentationPosition - 0.25',
      'presentationPosition + 0.25',
      'std::lower_bound(bins.begin(), bins.end(), start',
      'coverage >= requested * 0.9',
      'local / %.0f avg kbps') `
    'PacketCost 500 ms/5 Hz transport presentation'

$database = Require-Text (Join-Path $repo 'src\component\database_service.cpp') `
    @('historyRetryDecision(', 'kMaximumWriteSpool = 1024',
      'writeSpool_.push_back', 'if (inserted) publishEvent') `
    'Classified bounded idempotent history replay'

$dspHud = Require-Text (Join-Path $repo 'src\component\master_dsp_window.cpp') `
    @('bounds.left + 50', 'bounds.right - 18',
      'constexpr std::array<float, 7> domains',
      'draggedEqHalfRange = hudDbHalfRange',
      'const auto cacheHit = curveCacheKey == hudCurveCacheKey',
      'hudCurveCache = points') `
    'DSP retained curve, gutters and frozen quantized domain'

# A wall-clock exponential must land at the same state regardless of how many
# presentation samples the monitor requested. This mirrors the QPC/timestamp
# contract used by CRT persistence and the retained visualizers.
function Measure-Decay([double]$Rate, [double]$Duration, [double]$Tau) {
    $time = 0.0
    $value = 1.0
    while ($time -lt $Duration) {
        $step = [Math]::Min(1.0 / $Rate, $Duration - $time)
        $value *= [Math]::Exp(-$step / $Tau)
        $time += $step
    }
    return $value
}

$reference = [Math]::Exp(-2.0 / 0.095)
foreach ($rate in @(59.94, 60.0, 75.0, 119.88, 120.0, 144.0,
        165.0, 200.0, 240.0)) {
    $actual = Measure-Decay $rate 2.0 0.095
    if ([Math]::Abs($actual - $reference) -gt 1.0e-12) {
        throw "Refresh-dependent visual response at $rate Hz: $actual"
    }
}

Write-Host 'GODENplayer refresh/presentation contract: PASS'
