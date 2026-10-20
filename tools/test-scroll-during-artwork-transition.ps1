param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [string]$QaRoot = '',
    [string]$ManifestPath = '',
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FirstTrack,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$SecondTrack
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $QaRoot) {
    $QaRoot = Join-Path $repo 'artifacts\scroll-transition-qa'
}
$qaRootFull = [IO.Path]::GetFullPath($QaRoot).TrimEnd('\')
if (-not ($qaRootFull + '\').StartsWith(
        $artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Scroll-transition QA must stay under ${artifacts}: $qaRootFull"
}
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $qaRootFull 'manifest.json'
}
$capturePath = Join-Path $qaRootFull 'bottom-frame.bmp'

$source = [IO.Path]::GetFullPath($SourceRoot)
$appRoot = Join-Path $qaRootFull 'app'
$appRootFull = [IO.Path]::GetFullPath($appRoot).TrimEnd('\')
if (-not ($appRootFull + '\').StartsWith(
        $qaRootFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Disposable QA app escaped its artifact root: $appRootFull"
}
$app = Join-Path $appRoot 'foobar2000.exe'
$component = Join-Path $repo "out\$Configuration\foo_nekorox.dll"
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
foreach ($required in @($source, $component, $probe, $FirstTrack, $SecondTrack)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Scroll-transition QA input is missing: $required"
    }
}

function Get-QaProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) { throw 'Multiple hidden QA players are running.' }
    if ($matches.Count -eq 0) { return $null }
    return Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue
}

function Wait-QaProcess([bool]$Present, [int]$TimeoutMs = 15000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-QaProcess
        if ($Present -and $null -ne $process) { return $process }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 40
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw 'Hidden QA player did not reach its expected process state.'
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $probe @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        throw "Background probe '$Action' failed with exit code ${exitCode}: $($output -join "`n")"
    }
    return ($output | ForEach-Object { $_.ToString() }) -join "`n"
}

function Invoke-ProbeEventually(
        [string]$Action,
        [string]$Argument = '',
        [int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $lastError = $null
    do {
        try { return Invoke-Probe $Action $Argument }
        catch {
            $lastError = $_
            Start-Sleep -Milliseconds 100
        }
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw $lastError
}

function Test-TransitionEvidence([string]$State) {
    if ($State -match 'transition=true') { return $true }
    return $State -match 'reconstructionPrepared=true' -and
        $State -match 'reconstructionFrames=([1-9][0-9]*)'
}

if ($null -ne (Get-QaProcess)) {
    & $app /exit | Out-Null
    Wait-QaProcess $false | Out-Null
}
if (Test-Path -LiteralPath $qaRootFull) {
    Remove-Item -LiteralPath $qaRootFull -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $appRoot | Out-Null
& robocopy.exe $source $appRoot /E /COPY:DAT /DCOPY:T /R:2 /W:1 `
    /NFL /NDL /NJH /NJS /XF running nobadshutdown | Out-Null
if ($LASTEXITCODE -gt 7) {
    throw "QA portable copy failed with robocopy exit code $LASTEXITCODE."
}
$componentTarget = Join-Path $appRoot `
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll'
Copy-Item -LiteralPath $component -Destination $componentTarget -Force

$benchmark = ''
$baselineBenchmark = ''
$transitionState = ''
$facetPreservation = ''
$randomAlbumReveal = ''
$themeFrameState = ''
$completedTransitionState = ''
$stableCpuMilliseconds = 0.0
$stableCpuWindowMilliseconds = 0.0
$transitionCpuMilliseconds = 0.0
$transitionCpuWindowMilliseconds = 0.0
try {
    # Minimized startup lets Columns UI construct its child HWND graph while
    # staying out of the foreground. The probe hides it immediately afterward.
    Start-Process -FilePath $app -WindowStyle Minimized
    Wait-QaProcess $true | Out-Null
    Start-Sleep -Milliseconds 750
    Invoke-ProbeEventually 'window-offscreen-show' | Out-Null
    Start-Sleep -Milliseconds 180
    Invoke-Probe 'window-redraw' | Out-Null
    Invoke-Probe 'mute-on' | Out-Null
    $workspaceState = Invoke-Probe 'workspace-state'
    if ($workspaceState -notmatch 'tab=4') {
        Invoke-Probe 'lib' | Out-Null
    }
    $facetPreservation = Invoke-ProbeEventually `
        'verify-facet-scroll-preservation' '' 10000
    if ($facetPreservation -notmatch 'Facet scroll preservation: PASS') {
        throw "Facet scroll preservation did not pass: $facetPreservation"
    }
    $randomAlbumReveal = Invoke-ProbeEventually `
        'verify-random-album-now-playing-reveal' '' 10000
    if ($randomAlbumReveal -notmatch
            'Random album NP reveal: PASS') {
        throw "Random album NP reveal did not pass: $randomAlbumReveal"
    }
    Invoke-Probe 'drop-file' $FirstTrack | Out-Null
    Start-Sleep -Milliseconds 750
    $playbackState = Invoke-Probe 'cover-state'
    if ($playbackState -notmatch 'track=true') {
        Invoke-Probe 'play' | Out-Null
        Start-Sleep -Milliseconds 500
        $playbackState = Invoke-Probe 'cover-state'
    }
    if ($playbackState -notmatch 'track=true') {
        throw "Disposable QA player did not start playback: $playbackState"
    }

    $qaProcess = Get-QaProcess
    $qaProcess.Refresh()
    $stableCpuStart = $qaProcess.TotalProcessorTime.TotalMilliseconds
    $stableCpuClock = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Milliseconds 900
    $stableCpuClock.Stop()
    $qaProcess.Refresh()
    $stableCpuMilliseconds =
        $qaProcess.TotalProcessorTime.TotalMilliseconds - $stableCpuStart
    $stableCpuWindowMilliseconds = $stableCpuClock.Elapsed.TotalMilliseconds

    $baselineBenchmark = Invoke-Probe 'benchmark-scroll-input' '20'
    if ($baselineBenchmark -notmatch 'p95_ms=([0-9.]+)') {
        throw "Could not parse baseline scroll input benchmark: $baselineBenchmark"
    }
    $baselineP95 = [double]::Parse(
        $Matches[1],
        [Globalization.CultureInfo]::InvariantCulture)
    Start-Sleep -Milliseconds 180

    # A second album guarantees a new artwork identity. This drives the same
    # drag-and-drop playback path exposed to the shell without depending on a
    # restored playlist's size, selection or playback order.
    $qaProcess.Refresh()
    $transitionCpuStart = $qaProcess.TotalProcessorTime.TotalMilliseconds
    $transitionCpuClock = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Probe 'drop-file' $SecondTrack | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $transitionState = Invoke-Probe 'cover-state'
        if (Test-TransitionEvidence $transitionState) { break }
        Start-Sleep -Milliseconds 8
    } while ($watch.ElapsedMilliseconds -lt 900)
    if (-not (Test-TransitionEvidence $transitionState)) {
        throw "Artwork transition produced no live or cumulative frame evidence after changing albums: $transitionState"
    }

    $benchmark = Invoke-Probe 'benchmark-scroll-input' '20'
    if ($benchmark -notmatch 'p95_ms=([0-9.]+)') {
        throw "Could not parse transition scroll input benchmark: $benchmark"
    }
    $p95 = [double]::Parse(
        $Matches[1],
        [Globalization.CultureInfo]::InvariantCulture)
    if ($p95 -gt 12.0) {
        throw "Transition scroll input p95 exceeded 12 ms: $benchmark"
    }
    $transitionOverhead = $p95 - $baselineP95
    if ($transitionOverhead -gt 6.0) {
        throw "Artwork transition added more than 6 ms at p95. Baseline: $baselineBenchmark Transition: $benchmark"
    }
    $transitionAfterBenchmark = Invoke-Probe 'cover-state'
    if (-not (Test-TransitionEvidence $transitionAfterBenchmark)) {
        throw "Artwork transition lost its cumulative frame evidence during input: $transitionAfterBenchmark"
    }
    Start-Sleep -Milliseconds 900
    $completedTransitionState = Invoke-Probe 'cover-state'
    $transitionCpuClock.Stop()
    $qaProcess.Refresh()
    $transitionCpuMilliseconds =
        $qaProcess.TotalProcessorTime.TotalMilliseconds - $transitionCpuStart
    $transitionCpuWindowMilliseconds =
        $transitionCpuClock.Elapsed.TotalMilliseconds
    if ($completedTransitionState -notmatch 'reconstruction=true' -or
        $completedTransitionState -notmatch 'fadeFallback=false' -or
        $completedTransitionState -notmatch 'reconstructionPrepared=true') {
        throw "Artwork reconstruction changed presentation path during input: $completedTransitionState"
    }
    $themeFrameState = Invoke-Probe 'workspace-render-state'
    if ($themeFrameState -notmatch 'lastThemeFrames=([0-9]+)') {
        throw "Could not parse theme presentation frames: $themeFrameState"
    }
    $themeFrames = [int]$Matches[1]
    if ($themeFrames -lt 8) {
        throw "Theme transition presented too few distinct frames: $themeFrameState"
    }
    Invoke-Probe 'window-capture' $capturePath | Out-Null

    [ordered]@{
        schema = 'nekorox.scroll-during-artwork-transition.v2'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        source = $source
        app = $app
        capture = $capturePath
        firstTrack = $FirstTrack
        secondTrack = $SecondTrack
        hidden = $true
        muted = $true
        transitionState = $transitionState
        transitionAfterBenchmark = $transitionAfterBenchmark
        completedTransitionState = $completedTransitionState
        themeFrameState = $themeFrameState
        themePresentedFrames = $themeFrames
        minimumThemePresentedFrames = 8
        facetScrollPreservation = $facetPreservation
        randomAlbumNowPlayingReveal = $randomAlbumReveal
        baselineBenchmark = $baselineBenchmark
        baselineP95Milliseconds = $baselineP95
        benchmark = $benchmark
        p95Milliseconds = $p95
        transitionOverheadMilliseconds = $transitionOverhead
        stableCpuMilliseconds = $stableCpuMilliseconds
        stableCpuWindowMilliseconds = $stableCpuWindowMilliseconds
        stableCpuOneCorePercent = 100.0 * $stableCpuMilliseconds /
            [Math]::Max(1.0, $stableCpuWindowMilliseconds)
        transitionWithInputCpuMilliseconds = $transitionCpuMilliseconds
        transitionWithInputCpuWindowMilliseconds =
            $transitionCpuWindowMilliseconds
        transitionWithInputCpuOneCorePercent =
            100.0 * $transitionCpuMilliseconds /
            [Math]::Max(1.0, $transitionCpuWindowMilliseconds)
        thresholdMilliseconds = 12.0
        maximumTransitionOverheadMilliseconds = 6.0
        measurement = 'cross-process wheel dispatch and handler return'
        note = 'Offscreen DWM present latency is intentionally excluded because occluded swap chains are throttled.'
        status = 'pass'
    } | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath $ManifestPath -Encoding UTF8
}
finally {
    if ($null -ne (Get-QaProcess)) {
        & $app /exit | Out-Null
        try { Wait-QaProcess $false | Out-Null } catch {}
    }
    if (Test-Path -LiteralPath $appRootFull) {
        $cleanupError = $null
        for ($attempt = 0; $attempt -lt 20 -and
                (Test-Path -LiteralPath $appRootFull); ++$attempt) {
            try {
                Remove-Item -LiteralPath $appRootFull -Recurse -Force `
                    -ErrorAction Stop
                $cleanupError = $null
            }
            catch {
                $cleanupError = $_
                Start-Sleep -Milliseconds 100
            }
        }
        if (Test-Path -LiteralPath $appRootFull) { throw $cleanupError }
    }
}

Write-Host $benchmark
Write-Host "Scroll-during-transition QA: PASS"
Write-Host "Manifest: $ManifestPath"
