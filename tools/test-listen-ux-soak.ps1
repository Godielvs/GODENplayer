param(
    [Parameter(Mandatory = $true)]
    [string]$CandidateApp,
    [ValidateRange(1, 200)]
    [int]$Cycles = 30,
    [ValidateRange(20, 3600)]
    [int]$DurationSeconds = 90,
    [ValidateRange(0, 50)]
    [int]$ResizeCycles = 0,
    [ValidateRange(0, 200)]
    [int]$TransportCycles = 0,
    [switch]$RequireAlbumResume,
    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$candidate = [IO.Path]::GetFullPath($CandidateApp)
$labRoot = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts'))
if (-not $candidate.StartsWith($labRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $candidate)) {
    throw 'Only a disposable app under this repository artifacts directory is allowed.'
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repo 'artifacts\listen-ux-soak'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if (-not (Test-Path -LiteralPath $probe)) {
    throw "Build nekorox_ui_probe.exe first: $probe"
}
$running = @(Get-CimInstance Win32_Process -Filter "Name = 'foobar2000.exe'" |
    Where-Object { $_.ExecutablePath -ieq $candidate })
if ($running.Count -ne 1) {
    throw "Expected exactly one running disposable candidate, found $($running.Count)."
}
$candidatePid = [int]$running[0].ProcessId

function Invoke-Probe {
    param([string]$Action, [string]$Value = '')
    $arguments = @($candidate, $Action)
    if ($Value) { $arguments += $Value }
    $started = [Diagnostics.Stopwatch]::StartNew()
    $result = & $probe @arguments 2>&1
    $code = $LASTEXITCODE
    $started.Stop()
    [pscustomobject]@{
        action = $Action
        value = $Value
        code = $code
        elapsedMilliseconds = [Math]::Round($started.Elapsed.TotalMilliseconds, 3)
        output = ($result -join "`n")
    }
}

$preflight = Invoke-Probe 'playback-state'
if ($preflight.code -ne 0 -or $preflight.output -notmatch 'state=playing') {
    throw 'Start playback in the disposable candidate before the UX soak.'
}
$listenPreflight = Invoke-Probe 'listen'
if ($listenPreflight.code -ne 0) {
    throw "Could not open Listen before the UX soak: $($listenPreflight.output)"
}

$ringPath = Join-Path $output 'performance-ring.json'
$ringStdout = Join-Path $output 'performance-ring.stdout.txt'
$ringStderr = Join-Path $output 'performance-ring.stderr.txt'
$powershell = (Get-Command pwsh -ErrorAction Stop).Source
$watcher = Join-Path $repo 'tools\watch-performance-event-ring.ps1'
$watcherArguments = @(
    '-NoProfile', '-File', ('"' + $watcher + '"'),
    '-ProcessId', [string]$candidatePid,
    '-DurationSeconds', [string]$DurationSeconds,
    '-HeartbeatLimitMilliseconds', '150',
    '-UiProbeExecutable', ('"' + $probe + '"'),
    '-AppExecutable', ('"' + $candidate + '"'),
    '-OutputPath', ('"' + $ringPath + '"'))
$ringProcess = Start-Process -FilePath $powershell `
    -ArgumentList $watcherArguments -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $ringStdout -RedirectStandardError $ringStderr

$actions = [Collections.Generic.List[object]]::new()
try {
    for ($cycle = 0; $cycle -lt $Cycles; ++$cycle) {
        foreach ($action in @(
                'listen-close', 'listen', 'listen-live',
                'listen-focus-crt', 'listen-focus-return',
                'listen-overview')) {
            $result = Invoke-Probe $action
            $result | Add-Member -NotePropertyName cycle -NotePropertyValue $cycle
            $actions.Add($result)
            if ($result.code -ne 0) {
                throw "UI action failed in cycle $cycle`: $action — $($result.output)"
            }
        }
    }
    foreach ($action in @('listen-live', 'listen-focus-crt')) {
        $result = Invoke-Probe $action
        $result | Add-Member -NotePropertyName cycle -NotePropertyValue $Cycles
        $actions.Add($result)
        if ($result.code -ne 0) { throw "Final focus failed: $action" }
    }
    $crtStartPath = Join-Path $output 'crt-start.json'
    $crtEndPath = Join-Path $output 'crt-end.json'
    $null = Invoke-Probe 'listen-crt-stats' $crtStartPath
    Start-Sleep -Seconds 5
    $null = Invoke-Probe 'listen-crt-stats' $crtEndPath
    $crtStart = Get-Content -LiteralPath $crtStartPath -Raw | ConvertFrom-Json
    $crtEnd = Get-Content -LiteralPath $crtEndPath -Raw | ConvertFrom-Json
    if (-not $crtEnd.active -or
            $crtEnd.presentedFrames - $crtStart.presentedFrames -lt 30) {
        throw 'CRT stopped presenting while focused and playback was active.'
    }
    for ($cycle = 0; $cycle -lt $ResizeCycles; ++$cycle) {
        foreach ($size in @('640x480', '1600x900', '900x620')) {
            $result = Invoke-Probe 'window-resize' $size
            $result | Add-Member -NotePropertyName cycle `
                -NotePropertyValue ($Cycles + $cycle + 1)
            $actions.Add($result)
            if ($result.code -ne 0) {
                throw "Resize failed in cycle $cycle`: $size"
            }
        }
    }
    for ($cycle = 0; $cycle -lt $TransportCycles; ++$cycle) {
        foreach ($action in @('play', 'play', 'waveseek-seek-percent')) {
            $value = if ($action -eq 'waveseek-seek-percent') {
                [string](10 + ($cycle % 8) * 10)
            } else { '' }
            $result = Invoke-Probe $action $value
            $result | Add-Member -NotePropertyName cycle `
                -NotePropertyValue ($Cycles + $ResizeCycles + $cycle + 1)
            $actions.Add($result)
            if ($result.code -ne 0) {
                throw "Transport action failed in cycle $cycle`: $action"
            }
        }
    }
    $closeResult = Invoke-Probe 'listen-close'
    $closeResult | Add-Member -NotePropertyName cycle `
        -NotePropertyValue ($Cycles + $ResizeCycles + 1)
    $actions.Add($closeResult)
    if ($closeResult.code -ne 0) { throw 'Listen did not close.' }
    if ($RequireAlbumResume) {
        $before = Invoke-Probe 'shell-album-ambient-state'
        if ($before.code -ne 0 -or
                $before.output -notmatch 'presentedRevision=(\d+)' -or
                $before.output -notmatch 'albumMode=true') {
            throw 'Album resume check requires a playing Album view.'
        }
        $beforeRevision = [uint64]([regex]::Match(
            $before.output, 'presentedRevision=(\d+)').Groups[1].Value)
        Start-Sleep -Seconds 2
        $after = Invoke-Probe 'shell-album-ambient-state'
        if ($after.code -ne 0 -or
                $after.output -notmatch 'presentedRevision=(\d+)') {
            throw 'Album progress could not be read after closing Listen.'
        }
        $afterRevision = [uint64]([regex]::Match(
            $after.output, 'presentedRevision=(\d+)').Groups[1].Value)
        if ($afterRevision -le $beforeRevision) {
            throw "Album stayed frozen after Listen closed: revision $afterRevision."
        }
    }
}
finally {
    $actions | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $output 'ui-actions.json') -Encoding UTF8
    $elapsed = 0
    while (-not $ringProcess.HasExited -and $elapsed -lt $DurationSeconds + 20) {
        Start-Sleep -Milliseconds 500
        $elapsed += 0.5
        $ringProcess.Refresh()
    }
    if (-not $ringProcess.HasExited) {
        throw 'The performance ring watcher did not finish within its bounded duration.'
    }
}

$ring = Get-Content -LiteralPath $ringPath -Raw | ConvertFrom-Json
$report = [ordered]@{
    schema = 'goden.listen-ux-soak.v1'
    candidate = $candidate
    processId = $candidatePid
    cycles = $Cycles
    resizeCycles = $ResizeCycles
    transportCycles = $TransportCycles
    requiredAlbumResume = [bool]$RequireAlbumResume
    actionCount = $actions.Count
    actionFailures = @($actions | Where-Object { $_.code -ne 0 }).Count
    maximumUiHeartbeatGapMilliseconds = $ring.maximumHeartbeatGapMilliseconds
    maximumActionMilliseconds = @($actions | Measure-Object elapsedMilliseconds -Maximum)[0].Maximum
    crtPresentedFramesDuringFocus = $crtEnd.presentedFrames - $crtStart.presentedFrames
    crtDroppedSegmentsDuringFocus = $crtEnd.droppedSegments - $crtStart.droppedSegments
    albumPresentedRevisionBeforeResume = if ($RequireAlbumResume) {
        $beforeRevision
    } else { $null }
    albumPresentedRevisionAfterResume = if ($RequireAlbumResume) {
        $afterRevision
    } else { $null }
    passed = $ring.passed -and $ringProcess.ExitCode -eq 0 -and
        @($actions | Where-Object { $_.code -ne 0 }).Count -eq 0
}
$reportPath = Join-Path $output 'result.json'
$report | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath $reportPath -Encoding UTF8
if (-not $report.passed) { throw "Listen UX soak failed: $reportPath" }
Write-Host "Listen UX soak: PASS ($Cycles cycles, max UI gap $([Math]::Round($report.maximumUiHeartbeatGapMilliseconds, 2)) ms)"
Write-Host "Evidence: $reportPath"
