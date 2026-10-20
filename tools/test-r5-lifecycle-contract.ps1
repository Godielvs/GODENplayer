Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Read-Source([string]$Relative) {
    $path = Join-Path $repo $Relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "R5 contract input is missing: $Relative"
    }
    return Get-Content -LiteralPath $path -Raw
}

$display = Read-Source 'src\component\display_environment.cpp'
foreach ($needle in @('registerPersistentRoot(', 'unregisterPersistentRoot(',
        'windowPresentationRevision')) {
    if (-not $display.Contains($needle)) { throw "Display root contract missing: $needle" }
}

$crt = Read-Source 'src\component\crt_xy_surface.cpp'
foreach ($needle in @('CreateThreadpoolWait(', 'armedGeneration_',
        'armedSerial_', 'readyMessagePending_',
        'WaitForThreadpoolWaitCallbacks(wait, TRUE)')) {
    if (-not $crt.Contains($needle)) { throw "CRT lifecycle contract missing: $needle" }
}
if ($crt.Contains('RegisterWaitForSingleObject(') -or
    $crt.Contains('UnregisterWaitEx(')) {
    throw 'CRT still contains per-frame legacy wait registration.'
}
$present = [regex]::Match($crt,
    '(?s)void CrtXySurface::present\(.*?\n\}').Value
if ($present.Contains('disarmFrameWait(')) {
    throw 'CRT valid/obsolete token is still disarmed inside present.'
}

$listen = Read-Source 'src\component\listen_window.cpp'
if ($listen -match '(?s)if \(result == FrameResult::accepted\).*?else\s*\{\s*crtXySurface_\.setActive\(false\)') {
    throw 'Main notReady still deactivates CRT.'
}
foreach ($needle in @('currentLiveLayout()', 'setLiveFocus(',
        'squareWithinListenRect(layout.crt.plot)')) {
    if (-not $listen.Contains($needle)) { throw "Listen layout contract missing: $needle" }
}

$renderer = Read-Source 'src\component\listen_visual_renderer.cpp'
foreach ($needle in @('spectrumPlotFor(', 'kLeftGutter = 44.0f',
        'kBottomGutter = 24.0f', 'PushAxisAlignedClip(')) {
    if (-not $renderer.Contains($needle)) { throw "Spectrum gutter contract missing: $needle" }
}

$database = Read-Source 'src\component\database_service.cpp'
if ($database -match '(?s)void DatabaseService::retry\(\)\s*\{\s*stop\(\)') {
    throw 'History retry still stops/joins the worker on the caller.'
}
foreach ($needle in @('retryDatabase = true', 'kMaximumPlaybackBacklog = 1024',
        'A committed event is authoritative evidence that the writer recovered')) {
    if (-not $database.Contains($needle)) { throw "History recovery contract missing: $needle" }
}

$options = Read-Source 'src\component\options_window.cpp'
foreach ($needle in @('WS_EX_CONTROLPARENT', 'UiFormFlow',
        'viewport_.setBounds(', 'CB_SETMINVISIBLE')) {
    if (-not $options.Contains($needle)) { throw "Options flow contract missing: $needle" }
}
$tag = Read-Source 'src\component\tag_editor_window.cpp'
foreach ($needle in @('WS_EX_CONTROLPARENT',
        'viewport_.setBounds(', 'CB_SETMINVISIBLE')) {
    if (-not $tag.Contains($needle)) { throw "TAG viewport contract missing: $needle" }
}
$formViewport = Read-Source 'src\component\ui_form_viewport.cpp'
foreach ($needle in @('WS_VSCROLL', 'case WM_VSCROLL:',
        'SetWindowPos(content_', '0, -scrollOffset_', 'GetNextDlgTabItem(',
        'WM_COMMAND:', 'WM_NOTIFY:', 'WM_CTLCOLORSTATIC:')) {
    if (-not $formViewport.Contains($needle)) {
        throw "Native form viewport contract missing: $needle"
    }
}

$shell = Read-Source 'src\component\player_shell_panel.cpp'
if (-not $shell.Contains('mode_ = PlayerShellMode::normal;')) {
    throw 'Cold start does not force a useful Library landing.'
}
$native = Read-Source 'src\component\native_panels.cpp'
if (-not $native.Contains('(void)revealNowPlayingInWorkspace();')) {
    throw 'Small cover does not reuse RevealNowPlaying.'
}

Write-Host 'GODENplayer R5 lifecycle/source contracts: PASS'
