param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory=$true)][string]$DevRoot,
    [int]$RestartCycles = 50,
    [int]$TrackChanges = 100,
    [int]$ListenCycles = 20,
    [int]$CpuSamples = 20,
    [string]$ManifestPath = '',
    [string]$LayoutPath = '',
    [switch]$Background,
    [switch]$ColdAnalysisCache,
    [int]$LifecycleCandidates = 12
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
$app = Join-Path $devRootFull 'app\foobar2000.exe'
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\runtime-qa.json'
}
if (-not $LayoutPath) {
    $LayoutPath = Join-Path $repo 'config\layout\nekorox-v3.fcl'
}

if ($devRootFull -match 'foobar2000-64bit-lab' -or
        $devRootFull -match 'backup-2026-06-03') {
    throw "Refusing runtime QA against a protected installation: $devRootFull"
}
$artifactsRoot = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if ($Background) {
    if (-not ($devRootFull + '\').StartsWith(
            $artifactsRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Background QA hooks are restricted to disposable artifacts.'
    }
    # Every production hook requires both guards. The candidate also sees the
    # background marker so auxiliary windows never activate or steal focus.
    $env:NEKOROX_QA_HOOKS = '1'
    $env:NEKOROX_QA_PROFILE = '1'
    $env:NEKOROX_QA_BACKGROUND = '1'
}
foreach ($required in @($app, $probe, $LayoutPath,
        (Join-Path $PSScriptRoot 'verify-runtime-layout.js'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Runtime QA input is missing: $required"
    }
}
foreach ($value in @($RestartCycles, $TrackChanges, $ListenCycles, $CpuSamples,
        $LifecycleCandidates)) {
    if ($value -lt 1) { throw 'Runtime QA iteration counts must be positive.' }
}

function Get-AppProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) {
        throw "Multiple development foobar2000 processes are running."
    }
    if ($matches.Count -eq 0) { return $null }
    try {
        return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop
    }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        # The process can disappear between the CIM snapshot and Get-Process
        # during the restart soak. That is the expected stopped state.
        return $null
    }
}

function Wait-App([bool]$Present, [int]$TimeoutMs = 15000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-AppProcess
        if ($Present -and $null -ne $process -and
                ($Background -or $process.MainWindowHandle -ne 0)) {
            return $process
        }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    $expectation = if ($Present) { 'start' } else { 'exit' }
    throw "Development foobar2000 did not $expectation within ${TimeoutMs}ms."
}

function Prepare-ColdAnalysisCache {
    if (-not $ColdAnalysisCache) { return $null }
    $artifactsRoot = [IO.Path]::GetFullPath(
        (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
    if (-not ($devRootFull + '\').StartsWith(
            $artifactsRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Cold analysis cache preparation is restricted to disposable artifacts.'
    }

    if ($null -ne (Get-AppProcess)) {
        & $app /exit | Out-Null
        Wait-App $false | Out-Null
    }

    $databaseRoot = Join-Path $devRootFull 'app\profile\nekorox'
    $backupRoot = Join-Path $repo (
        'artifacts\runtime-cold-cache\' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
    $moved = New-Object 'System.Collections.Generic.List[string]'
    foreach ($name in @('nekorox.db', 'nekorox.db-wal', 'nekorox.db-shm')) {
        $source = Join-Path $databaseRoot $name
        if (-not (Test-Path -LiteralPath $source)) { continue }
        Move-Item -LiteralPath $source -Destination $backupRoot
        $moved.Add((Join-Path $backupRoot $name))
    }
    return [pscustomobject]@{
        requested = $true
        databaseRoot = $databaseRoot
        backupRoot = $backupRoot
        moved = @($moved)
    }
}

function Write-LifecycleWave([string]$Path) {
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $sampleRate = 44100
    $durationSeconds = 30
    $samples = $sampleRate * $durationSeconds
    $dataBytes = $samples * 2
    $stream = [IO.File]::Create($Path)
    $writer = New-Object IO.BinaryWriter($stream)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([int](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
        $writer.Write([int]16)
        $writer.Write([int16]1)
        $writer.Write([int16]1)
        $writer.Write([int]$sampleRate)
        $writer.Write([int]($sampleRate * 2))
        $writer.Write([int16]2)
        $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([int]$dataBytes)
        for ($index = 0; $index -lt $samples; $index++) {
            $fundamental = [Math]::Sin(
                2.0 * [Math]::PI * 220.0 * $index / $sampleRate)
            $harmonic = [Math]::Sin(
                2.0 * [Math]::PI * 440.0 * $index / $sampleRate)
            $writer.Write([int16]([Math]::Round(
                ($fundamental * 0.65 + $harmonic * 0.25) * 1800.0)))
        }
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
    return [pscustomobject]@{
        path = $Path
        durationSeconds = $durationSeconds
        sampleRateHz = $sampleRate
        bytes = (Get-Item -LiteralPath $Path).Length
        playbackConfirmed = $false
    }
}

function Invoke-Probe(
    [string]$Action,
    [string]$Argument = '',
    [switch]$AllowFailure
) {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $probeArguments = @($app, $Action)
        if ($Argument) { $probeArguments += $Argument }
        if ($Background) { $probeArguments += '--background' }
        $output = & $probe @probeArguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "UI probe '$Action' failed with exit code ${exitCode}: $text"
    }
    return [pscustomobject]@{
        Success = $exitCode -eq 0
        Output = $text
    }
}

function Get-ListenAnalysisState {
    $probeResult = Invoke-Probe 'listen-analysis-state'
    if ($probeResult.Output -notmatch
            'lifecycleVisible=(true|false)\s+pending=(true|false)\s+status=([a-z]+)') {
        throw "Could not parse Listen analysis state: $($probeResult.Output)"
    }
    return [pscustomobject]@{
        LifecycleVisible = $Matches[1] -eq 'true'
        Pending = $Matches[2] -eq 'true'
        Status = $Matches[3]
    }
}

function Start-AppProcess {
    if ($Background) {
        Start-Process -FilePath $app -WindowStyle Hidden
    } else {
        Start-Process -FilePath $app
    }
}

function Start-App {
    if ($null -eq (Get-AppProcess)) {
        Start-AppProcess
    }
    return Wait-App $true
}

function Assert-BackgroundMainWindow {
    if (-not $Background) { return }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $hidden = Invoke-Probe 'window-hide' -AllowFailure
        $state = Invoke-Probe 'window-state' -AllowFailure
        if ($hidden.Success -and $state.Success -and $state.Output -match
                'visible=false\s+iconic=false\s+zoomed=false') {
            return
        }
        Start-Sleep -Milliseconds 25
    } while ($watch.ElapsedMilliseconds -lt 3000)
    throw "Background main window state was intrusive: $($state.Output)"
}

function Set-BackgroundAudioSafe {
    if (-not $Background) { return '' }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $result = Invoke-Probe 'mute-on' -AllowFailure
        if ($result.Success) { return $result.Output }
        Start-Sleep -Milliseconds 25
    } while ($watch.ElapsedMilliseconds -lt 3000)
    throw "Could not force the background QA volume to silence: $($result.Output)"
}

function Assert-RuntimeLayout([string]$Label) {
    $directory = Join-Path $repo 'artifacts\runtime-layout-proof'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    $output = Join-Path $directory "$Label-$stamp.fcl"
    if ($Background) {
        Start-Process -FilePath $app -WindowStyle Hidden -Wait `
            -ArgumentList @('/columnsui:export-quiet', "`"$output`"")
    } else {
        & $app /columnsui:export-quiet $output | Out-Null
    }
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ((Test-Path -LiteralPath $output) -and
                (Get-Item -LiteralPath $output).Length -gt 0) {
            break
        }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt 15000)
    if (-not (Test-Path -LiteralPath $output)) {
        throw "Columns UI did not export the $Label runtime layout."
    }
    & node (Join-Path $PSScriptRoot 'verify-runtime-layout.js') $LayoutPath $output | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Columns UI $Label runtime layout does not match the golden FCL."
    }
    return $output
}

function Stop-App {
    $process = Get-AppProcess
    if ($null -eq $process) { return }
    if ($Background) {
        Start-Process -FilePath $app -WindowStyle Hidden -Wait `
            -ArgumentList '/exit'
    } else {
        & $app /exit | Out-Null
    }
    Wait-App $false | Out-Null
}

function Get-Descendants([int]$RootProcessId) {
    $all = @(Get-CimInstance Win32_Process)
    $pending = New-Object 'System.Collections.Generic.Queue[int]'
    $pending.Enqueue($RootProcessId)
    $result = @()
    while ($pending.Count -gt 0) {
        $parent = $pending.Dequeue()
        foreach ($child in @($all | Where-Object ParentProcessId -eq $parent)) {
            $result += $child
            $pending.Enqueue([int]$child.ProcessId)
        }
    }
    return @($result)
}

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $rank = [Math]::Ceiling($Percentile * $sorted.Count)
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $rank - 1))
    return [double]$sorted[$index]
}

function Get-AnalysisWorkers(
    [int]$RootProcessId,
    [ValidateSet('all', 'analyze', 'cost')]
    [string]$Kind = 'all'
) {
    $filter = "Name='nekorox-analysis.exe' AND ParentProcessId=$RootProcessId"
    return @(Get-CimInstance Win32_Process -Filter $filter | Where-Object {
        if ($_.Name -ine 'nekorox-analysis.exe') {
            $false
        } elseif ($Kind -eq 'all') {
            $true
        } else {
            $commandLine = [string]$_.CommandLine
            $commandLine -match "(?i)(?:^|\s)$Kind(?:\s|$)"
        }
    })
}

function Wait-AnalysisQuiet(
    [int]$RootProcessId,
    [ValidateSet('all', 'analyze', 'cost')]
    [string]$Kind = 'all',
    [int]$TimeoutMs = 5000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        if (@(Get-AnalysisWorkers $RootProcessId $Kind).Count -eq 0) {
            return $true
        }
        Start-Sleep -Milliseconds 25
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    return $false
}

function Wait-AnalysisWorker(
    [int]$RootProcessId,
    [ValidateSet('analyze', 'cost')]
    [string]$Kind,
    [Diagnostics.Stopwatch]$Watch,
    [int]$TimeoutMs
) {
    do {
        $workers = @(Get-AnalysisWorkers $RootProcessId $Kind)
        if ($workers.Count -gt 0) {
            return [pscustomobject]@{
                ElapsedMs = [double]$Watch.Elapsed.TotalMilliseconds
                ProcessIds = @($workers | Select-Object -ExpandProperty ProcessId)
            }
        }
        Start-Sleep -Milliseconds 25
    } while ($Watch.ElapsedMilliseconds -lt $TimeoutMs)
    return $null
}

function Wait-WorkerIdsExit(
    [int[]]$ProcessIds,
    [Diagnostics.Stopwatch]$Watch,
    [int]$TimeoutMs = 2000
) {
    do {
        $survivors = @($ProcessIds | Where-Object {
            Get-Process -Id $_ -ErrorAction SilentlyContinue
        })
        if ($survivors.Count -eq 0) { break }
        Start-Sleep -Milliseconds 5
    } while ($Watch.ElapsedMilliseconds -lt $TimeoutMs)
    $Watch.Stop()
    return [pscustomobject]@{
        Milliseconds = [double]$Watch.Elapsed.TotalMilliseconds
        Survivors = @($survivors)
    }
}

function Close-Listen {
    Invoke-Probe 'listen-close' -AllowFailure | Out-Null
    Start-Sleep -Milliseconds 100
}

function Open-Listen {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Probe 'listen' | Out-Null
    do {
        $result = if ($Background) {
            Invoke-Probe 'listen-window-state' -AllowFailure
        } else {
            Invoke-Probe 'listen-overview' -AllowFailure
        }
        if ($result.Success) {
            if ($Background -and $result.Output -notmatch
                    'visible=true\s+iconic=false\s+zoomed=false\s+foreground=false') {
                throw "Background Listen window state was intrusive: $($result.Output)"
            }
            $watch.Stop()
            return [double]$watch.Elapsed.TotalMilliseconds
        }
        Start-Sleep -Milliseconds 20
    } while ($watch.ElapsedMilliseconds -lt 3000)
    throw 'Listen did not become interactive within 3000ms.'
}

function Invoke-BackgroundScrollSmoke {
    $hangThresholdMs = 3000
    $result = [ordered]@{
        exercised = $false
        metric = 'probe wall-clock smoke latency; not input-to-present'
        framePresentGate = $false
        hangThresholdMs = $hangThresholdMs
        responsive = $true
        maxWallClockMs = 0.0
        samples = @()
    }
    if (-not $Background) { return [pscustomobject]$result }

    $bursts = @(
        [pscustomobject]@{ Action = 'scroll-artist'; Count = 4 },
        [pscustomobject]@{ Action = 'scroll-album'; Count = 4 },
        [pscustomobject]@{ Action = 'scroll-results'; Count = 6 }
    )
    $samples = @()
    foreach ($burst in $bursts) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $probeResult = Invoke-Probe $burst.Action ([string]$burst.Count) `
            -AllowFailure
        $watch.Stop()
        $process = Get-AppProcess
        $responsive = $null -ne $process -and $process.Responding
        $elapsedMs = [double]$watch.Elapsed.TotalMilliseconds
        $samples += [pscustomobject]@{
            action = $burst.Action
            events = $burst.Count
            wallClockMs = $elapsedMs
            probeSuccess = $probeResult.Success
            responsive = $responsive
        }
        if (-not $probeResult.Success) {
            $failures.Add(
                "Background $($burst.Action) smoke failed: $($probeResult.Output)")
        }
        if (-not $responsive) {
            $result.responsive = $false
            $failures.Add("Player stopped responding during $($burst.Action) smoke.")
        }
        if ($elapsedMs -gt $hangThresholdMs) {
            $failures.Add(
                "Background $($burst.Action) smoke exceeded the " +
                "${hangThresholdMs}ms hang threshold: $elapsedMs")
        }
    }
    $result.exercised = $true
    $result.samples = $samples
    $result.maxWallClockMs = [double](
        $samples | Measure-Object -Property wallClockMs -Maximum).Maximum
    return [pscustomobject]$result
}

function Invoke-ForegroundScrollGate {
    Invoke-Probe 'lib' | Out-Null
    $benchmark = Invoke-Probe 'benchmark-scroll' '80'
    $match = [regex]::Match($benchmark.Output,
        'median_ms=(?<median>[0-9.]+)\s+p95_ms=(?<p95>[0-9.]+)\s+max_ms=(?<max>[0-9.]+)')
    if (-not $match.Success) {
        throw "Could not parse scroll benchmark: $($benchmark.Output)"
    }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $median = [double]::Parse($match.Groups['median'].Value, $culture)
    $p95 = [double]::Parse($match.Groups['p95'].Value, $culture)
    $maximum = [double]::Parse($match.Groups['max'].Value, $culture)
    if ($p95 -gt 8.0) {
        $failures.Add("Scroll input-to-present p95 exceeded 8ms: $p95")
    }
    if ($maximum -gt 12.0) {
        $failures.Add("Scroll input-to-present max exceeded 12ms: $maximum")
    }
    return [pscustomobject][ordered]@{
        exercised = $true
        metric = 'QPC input-to-DXGI-Present-return latency'
        framePresentGate = $true
        samples = 80
        medianMs = $median
        p95Ms = $p95
        maxMs = $maximum
    }
}

function Invoke-ListenLifecycleQa([int]$RootProcessId) {
    $dwellMs = 1500
    $workerTimeoutMs = 5000
    $result = [ordered]@{
        exercised = $false
        dwellFloorMs = $dwellMs
        candidatesTried = 0
        hiddenBeforeOpenSuppressed = $false
        automaticStartMs = $null
        trackChangeCancellationMs = $null
        trackChangeRestartMs = $null
        minimizeCancellationMs = $null
        minimizedSuppressed = $false
        minimizedAnalysisStatus = $null
        restoreAfterMinimizeMs = $null
        restoreAfterMinimizeResolution = $null
        hideCancellationMs = $null
        hiddenSuppressed = $false
        hiddenAnalysisStatus = $null
        restoreAfterHideMs = $null
        restoreAfterHideResolution = $null
        closeCancellationMs = $null
        residualWorkers = @()
    }

    try {
        Close-Listen
        Invoke-Probe 'stop' | Out-Null
        Invoke-Probe 'play' | Out-Null
        if (-not (Wait-AnalysisQuiet $RootProcessId 'analyze' 5000)) {
            $failures.Add('Deep-analysis workers were not quiet before lifecycle QA.')
        }

        $hiddenWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'next' | Out-Null
        $hiddenWorker = Wait-AnalysisWorker $RootProcessId 'analyze' `
            $hiddenWatch ($dwellMs + 250)
        if ($null -eq $hiddenWorker) {
            $result.hiddenBeforeOpenSuppressed = $true
        } else {
            $failures.Add('Hidden Listen started automatic deep analysis.')
        }

        $automatic = $null
        for ($candidate = 0; $candidate -lt $LifecycleCandidates; $candidate++) {
            Close-Listen
            Invoke-Probe 'next' | Out-Null
            Invoke-Probe 'stop' | Out-Null
            Invoke-Probe 'play' | Out-Null
            if (-not (Wait-AnalysisQuiet $RootProcessId 'analyze' 2000)) {
                $failures.Add('Track change did not clear the previous deep-analysis worker.')
            }

            $visibleWatch = [Diagnostics.Stopwatch]::StartNew()
            Open-Listen | Out-Null
            $result.candidatesTried = $candidate + 1
            $automatic = Wait-AnalysisWorker $RootProcessId 'analyze' `
                $visibleWatch $workerTimeoutMs
            if ($null -ne $automatic) { break }
        }

        if ($null -eq $automatic) {
            $failures.Add(
                "Automatic Listen analysis was not exercised across " +
                "$LifecycleCandidates candidate tracks; all may already be cached.")
            return [pscustomobject]$result
        }

        $result.exercised = $true
        $result.automaticStartMs = $automatic.ElapsedMs
        if ($automatic.ElapsedMs -lt $dwellMs) {
            $failures.Add(
                "Automatic analysis started before 1.5s visible: " +
                "$($automatic.ElapsedMs)ms")
        }

        $cancelWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'next' | Out-Null
        $cancelled = Wait-WorkerIdsExit `
            ([int[]]$automatic.ProcessIds) $cancelWatch 2000
        $result.trackChangeCancellationMs = $cancelled.Milliseconds
        if ($cancelled.Survivors.Count -gt 0) {
            $failures.Add('Deep-analysis worker survived track change for 2000ms.')
        } elseif ($cancelled.Milliseconds -gt 350.0) {
            $failures.Add(
                "Track-change cancellation exceeded 350ms (250ms grace): " +
                "$($cancelled.Milliseconds)")
        }

        $trackRestartWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'previous' | Out-Null
        $afterTrackChange = Wait-AnalysisWorker $RootProcessId 'analyze' `
            $trackRestartWatch $workerTimeoutMs
        if ($null -eq $afterTrackChange) {
            $failures.Add('Track change did not rearm automatic deep analysis.')
            return [pscustomobject]$result
        }
        $result.trackChangeRestartMs = $afterTrackChange.ElapsedMs
        if ($afterTrackChange.ElapsedMs -lt $dwellMs) {
            $failures.Add(
                "Track change inherited the previous dwell: " +
                "$($afterTrackChange.ElapsedMs)ms")
        }

        $minimizeWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'listen-minimize' | Out-Null
        $minimized = Wait-WorkerIdsExit `
            ([int[]]$afterTrackChange.ProcessIds) $minimizeWatch 2000
        $result.minimizeCancellationMs = $minimized.Milliseconds
        if ($minimized.Survivors.Count -gt 0) {
            $failures.Add('Deep-analysis worker survived Listen minimization for 2000ms.')
        }
        if ($Background) {
            $state = Invoke-Probe 'listen-window-state'
            if ($state.Output -notmatch
                    'visible=true\s+iconic=true\s+zoomed=false\s+foreground=false') {
                $failures.Add("Background minimized Listen state was intrusive: " +
                    $state.Output)
            }
        }
        $minimizedWatch = [Diagnostics.Stopwatch]::StartNew()
        $whileMinimized = Wait-AnalysisWorker $RootProcessId 'analyze' `
            $minimizedWatch ($dwellMs + 250)
        if ($null -eq $whileMinimized) {
            $result.minimizedSuppressed = $true
        } else {
            $failures.Add('Minimized Listen restarted deep analysis.')
        }
        $minimizedState = Get-ListenAnalysisState
        $result.minimizedAnalysisStatus = $minimizedState.Status

        $restoreWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'listen-show-noactivate' | Out-Null
        if ($Background) {
            $state = Invoke-Probe 'listen-window-state'
            if ($state.Output -notmatch
                    'visible=true\s+iconic=false\s+zoomed=false\s+foreground=false') {
                $failures.Add("Background restored Listen state was intrusive: " +
                    $state.Output)
            }
        }
        $afterMinimize = Wait-AnalysisWorker $RootProcessId 'analyze' `
            $restoreWatch $workerTimeoutMs
        if ($null -eq $afterMinimize) {
            $restoredState = Get-ListenAnalysisState
            if ($restoredState.Status -eq 'complete') {
                $result.restoreAfterMinimizeResolution = if (
                        $minimizedState.Status -eq 'complete') {
                    'already-complete'
                } else {
                    'completed-between-worker-probes'
                }
            } else {
                $failures.Add(
                    'Listen restore did not rearm automatic deep analysis ' +
                    "(status=$($restoredState.Status), " +
                    "pending=$($restoredState.Pending)).")
                return [pscustomobject]$result
            }
        } else {
            $result.restoreAfterMinimizeMs = $afterMinimize.ElapsedMs
            $result.restoreAfterMinimizeResolution = 'worker-observed'
            if ($afterMinimize.ElapsedMs -lt $dwellMs) {
                $failures.Add(
                    "Listen restore inherited the previous dwell: " +
                    "$($afterMinimize.ElapsedMs)ms")
            }
        }

        $workersBeforeHide = if ($null -ne $afterMinimize) {
            [int[]]$afterMinimize.ProcessIds
        } else {
            [int[]]@(Get-AnalysisWorkers $RootProcessId 'analyze' |
                Select-Object -ExpandProperty ProcessId)
        }
        $hideWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'listen-hide' | Out-Null
        $hidden = Wait-WorkerIdsExit `
            $workersBeforeHide $hideWatch 2000
        $result.hideCancellationMs = $hidden.Milliseconds
        if ($hidden.Survivors.Count -gt 0) {
            $failures.Add('Deep-analysis worker survived Listen hide for 2000ms.')
        }
        if ($Background) {
            $state = Invoke-Probe 'listen-window-state'
            if ($state.Output -notmatch
                    'visible=false\s+iconic=false\s+zoomed=false\s+foreground=false') {
                $failures.Add("Background hidden Listen state was intrusive: " +
                    $state.Output)
            }
        }
        $hiddenWatch = [Diagnostics.Stopwatch]::StartNew()
        $whileHidden = Wait-AnalysisWorker $RootProcessId 'analyze' `
            $hiddenWatch ($dwellMs + 250)
        if ($null -eq $whileHidden) {
            $result.hiddenSuppressed = $true
        } else {
            $failures.Add('Hidden Listen restarted deep analysis.')
        }
        $hiddenState = Get-ListenAnalysisState
        $result.hiddenAnalysisStatus = $hiddenState.Status

        $showWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'listen-show-noactivate' | Out-Null
        $afterHide = Wait-AnalysisWorker $RootProcessId 'analyze' `
            $showWatch $workerTimeoutMs
        if ($null -eq $afterHide) {
            $shownState = Get-ListenAnalysisState
            if ($shownState.Status -eq 'complete') {
                $result.restoreAfterHideResolution = if (
                        $hiddenState.Status -eq 'complete') {
                    'already-complete'
                } else {
                    'completed-between-worker-probes'
                }
            } else {
                $failures.Add(
                    'Listen show did not rearm automatic deep analysis ' +
                    "(status=$($shownState.Status), " +
                    "pending=$($shownState.Pending)).")
                return [pscustomobject]$result
            }
        } else {
            $result.restoreAfterHideMs = $afterHide.ElapsedMs
            $result.restoreAfterHideResolution = 'worker-observed'
            if ($afterHide.ElapsedMs -lt $dwellMs) {
                $failures.Add(
                    "Listen show inherited the previous dwell: " +
                    "$($afterHide.ElapsedMs)ms")
            }
        }

        $workersAtClose = @(Get-AnalysisWorkers $RootProcessId 'all')
        $closeWatch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'listen-close' | Out-Null
        $closed = Wait-WorkerIdsExit `
            ([int[]]@($workersAtClose | Select-Object -ExpandProperty ProcessId)) `
            $closeWatch 2000
        $result.closeCancellationMs = $closed.Milliseconds
        if ($closed.Survivors.Count -gt 0) {
            $failures.Add('Analysis worker survived Listen close for 2000ms.')
        }
    }
    finally {
        Close-Listen
        if (-not (Wait-AnalysisQuiet $RootProcessId 'all' 2000)) {
            $result.residualWorkers = @(
                Get-AnalysisWorkers $RootProcessId 'all' |
                    Select-Object Name, ProcessId, ExecutablePath, CommandLine)
            $failures.Add('An analysis worker remained after Listen lifecycle QA.')
        }
    }

    return [pscustomobject]$result
}

$failures = New-Object 'System.Collections.Generic.List[string]'
$startupSamples = New-Object 'System.Collections.Generic.List[double]'
$listenOpenSamples = New-Object 'System.Collections.Generic.List[double]'
$forbiddenNames = @('msedgewebview2.exe', 'node.exe', 'python.exe',
    'ffmpeg.exe', 'ffprobe.exe')
$audioSafetyProof = ''
$coldAnalysisCacheProof = Prepare-ColdAnalysisCache
$lifecycleFixtureProof = $null

$process = Start-App
if ($Background) {
    Assert-BackgroundMainWindow
    $audioSafetyProof = Set-BackgroundAudioSafe
} else {
    Invoke-Probe 'window-native-maximize' | Out-Null
}
Invoke-Probe 'stop' | Out-Null
Close-Listen
$initialLayoutProof = Assert-RuntimeLayout 'initial'

for ($cycle = 0; $cycle -lt $RestartCycles; $cycle++) {
    Stop-App
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Start-AppProcess
    $process = Wait-App $true
    if ($Background) {
        Assert-BackgroundMainWindow
        $audioSafetyProof = Set-BackgroundAudioSafe
        $ready = Invoke-Probe 'window-state'
    } else {
        $ready = Invoke-Probe 'window-native-maximize'
    }
    $watch.Stop()
    if (-not $ready.Success) { throw "Restart cycle $cycle was not interactive." }
    $startupSamples.Add($watch.Elapsed.TotalMilliseconds)
}
$restartLayoutProof = Assert-RuntimeLayout 'restart'

Invoke-Probe 'play' | Out-Null
Start-Sleep -Milliseconds 500
$np = Invoke-Probe 'benchmark-np' '100'
$npMatch = [regex]::Match($np.Output,
    'median_ms=(?<median>[0-9.]+)\s+p95_ms=(?<p95>[0-9.]+)\s+max_ms=(?<max>[0-9.]+)')
if (-not $npMatch.Success) { throw "Could not parse NP benchmark: $($np.Output)" }
$npMedian = [double]::Parse($npMatch.Groups['median'].Value,
    [Globalization.CultureInfo]::InvariantCulture)
$npP95 = [double]::Parse($npMatch.Groups['p95'].Value,
    [Globalization.CultureInfo]::InvariantCulture)
$npMax = [double]::Parse($npMatch.Groups['max'].Value,
    [Globalization.CultureInfo]::InvariantCulture)
if ($npP95 -gt 50.0) { $failures.Add("NP p95 exceeded 50ms: $npP95") }
$scrollSmoke = if ($Background) {
    Invoke-BackgroundScrollSmoke
} else {
    Invoke-ForegroundScrollGate
}

for ($index = 0; $index -lt $TrackChanges; $index++) {
    Invoke-Probe 'next' | Out-Null
    Start-Sleep -Milliseconds 10
}
$process = Get-AppProcess
if ($null -eq $process -or -not $process.Responding) {
    $failures.Add('Player was not responsive after the track-change burst.')
}

Close-Listen
$listenOpenSamples.Add((Open-Listen))
Close-Listen
$listenOpenSamples.Add((Open-Listen))
for ($cycle = 2; $cycle -lt $ListenCycles; $cycle++) {
    Close-Listen
    $listenOpenSamples.Add((Open-Listen))
}

$process = Get-AppProcess
Start-Sleep -Seconds 2
$descendants = Get-Descendants $process.Id
$listenWorkingSet = [double]$process.WorkingSet64
foreach ($child in $descendants) {
    try {
        $listenWorkingSet += (Get-Process -Id $child.ProcessId).WorkingSet64
    } catch {}
}
$listenWorkingSetMb = $listenWorkingSet / 1MB
if ($listenWorkingSetMb -gt 550.0) {
    $failures.Add("Listen working set exceeded 550MB: $listenWorkingSetMb")
}

$forbidden = @($descendants | Where-Object {
    $forbiddenNames -contains $_.Name.ToLowerInvariant()
})
if ($forbidden.Count -gt 0) {
    $failures.Add('A forbidden runtime process was found: ' +
        (($forbidden | Select-Object -ExpandProperty Name) -join ', '))
}

$lifecycleFixture = Join-Path $repo `
    'artifacts\runtime-qa\nekorox-lifecycle-fixture.wav'
$lifecycleFixtureProof = Write-LifecycleWave $lifecycleFixture
Invoke-Probe 'drop-file' $lifecycleFixture | Out-Null
Start-Sleep -Milliseconds 150
$fixturePlayback = Invoke-Probe 'transport-state'
$lifecycleFixtureProof.playbackConfirmed =
    $fixturePlayback.Output -match 'playing=true'
if (-not $lifecycleFixtureProof.playbackConfirmed) {
    $failures.Add(
        'The deterministic lifecycle fixture did not start playback: ' +
        $fixturePlayback.Output)
}

$listenLifecycle = Invoke-ListenLifecycleQa $process.Id
$workerCancellationMs = $listenLifecycle.trackChangeCancellationMs
$workerCancellationExercised = $listenLifecycle.exercised

Close-Listen
Invoke-Probe 'stop' | Out-Null
Start-Sleep -Seconds 10
$process = Get-AppProcess
$performanceObservers = @(
    Get-Process -Name 'codex-computer-use' -ErrorAction SilentlyContinue |
        Select-Object Id, ProcessName, Path
)
if ($performanceObservers.Count -gt 0) {
    $failures.Add(
        'Idle CPU measurement was contaminated by an external UI Automation ' +
        'observer: ' +
        (($performanceObservers | ForEach-Object {
            "$($_.ProcessName)[$($_.Id)]"
        }) -join ', '))
}
$cpu = New-Object 'System.Collections.Generic.List[double]'
$workingSets = New-Object 'System.Collections.Generic.List[double]'
$previousCpu = $process.TotalProcessorTime.TotalMilliseconds
$previousTime = [Diagnostics.Stopwatch]::StartNew()
for ($sample = 0; $sample -lt $CpuSamples; $sample++) {
    Start-Sleep -Seconds 1
    $current = Get-Process -Id $process.Id
    $elapsed = $previousTime.Elapsed.TotalMilliseconds
    $previousTime.Restart()
    $cpuDelta = $current.TotalProcessorTime.TotalMilliseconds - $previousCpu
    $previousCpu = $current.TotalProcessorTime.TotalMilliseconds
    $cpu.Add(100.0 * $cpuDelta / $elapsed / [Environment]::ProcessorCount)
    $workingSets.Add($current.WorkingSet64 / 1MB)
}
$cpuMedian = Get-Percentile $cpu.ToArray() 0.5
$cpuP95 = Get-Percentile $cpu.ToArray() 0.95
$workingSetMax = ($workingSets | Measure-Object -Maximum).Maximum
if ($cpuMedian -gt 0.5) { $failures.Add("Idle CPU median exceeded 0.5%: $cpuMedian") }
if ($cpuP95 -gt 1.0) { $failures.Add("Idle CPU p95 exceeded 1.0%: $cpuP95") }
# Structural services intentionally keep one render device and four immutable
# analysis buffers warm. Enforce the release contract (no >10% regression)
# against the 160 MiB pre-hub baseline instead of treating that baseline as a
# hard byte ceiling.
if ($workingSetMax -gt 176.0) {
    $failures.Add("Idle working set exceeded 176MB (10% regression gate): $workingSetMax")
}

$process = Get-AppProcess
$finalDescendants = Get-Descendants $process.Id
$residualWorkers = @($finalDescendants | Where-Object Name -eq 'nekorox-analysis.exe')
if ($residualWorkers.Count -gt 0) {
    $failures.Add('An analysis worker remained after Listen closed and playback stopped.')
}

$startupP95 = Get-Percentile $startupSamples.ToArray() 0.95
$listenP95 = Get-Percentile $listenOpenSamples.ToArray() 0.95
if ($listenP95 -gt 750.0) {
    $failures.Add("Warm Listen p95 exceeded 750ms: $listenP95")
}

$manifest = [ordered]@{
    schema = 'nekorox.runtime-qa.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    passed = $failures.Count -eq 0
    executable = $app
    mode = if ($Background) { 'background' } else { 'normal' }
    iterations = [ordered]@{
        restarts = $RestartCycles
        trackChanges = $TrackChanges
        listenCycles = $ListenCycles
        cpuSamples = $CpuSamples
        lifecycleCandidates = $LifecycleCandidates
    }
    startup = [ordered]@{
        medianMs = Get-Percentile $startupSamples.ToArray() 0.5
        p95Ms = $startupP95
        maxMs = ($startupSamples | Measure-Object -Maximum).Maximum
    }
    np = [ordered]@{ medianMs = $npMedian; p95Ms = $npP95; maxMs = $npMax }
    scrollSmoke = $scrollSmoke
    listen = [ordered]@{
        medianOpenMs = Get-Percentile $listenOpenSamples.ToArray() 0.5
        p95OpenMs = $listenP95
        maxOpenMs = ($listenOpenSamples | Measure-Object -Maximum).Maximum
        workingSetMb = $listenWorkingSetMb
    }
    analysisCancellation = [ordered]@{
        exercised = $workerCancellationExercised
        milliseconds = $workerCancellationMs
    }
    coldAnalysisCache = if ($null -ne $coldAnalysisCacheProof) {
        $coldAnalysisCacheProof
    } else {
        [ordered]@{ requested = $false }
    }
    lifecycleFixture = $lifecycleFixtureProof
    listenLifecycle = $listenLifecycle
    audioSafety = [ordered]@{
        requested = [bool]$Background
        action = if ($Background) { 'mute-on' } else { $null }
        mechanism = if ($Background) {
            'native-transport-volume-floor'
        } else {
            $null
        }
        proof = if ($Background) { $audioSafetyProof } else { $null }
    }
    idle = [ordered]@{
        cpuMedianPercent = $cpuMedian
        cpuP95Percent = $cpuP95
        cpuSamplesPercent = @($cpu)
        workingSetMaxMb = $workingSetMax
        threadCount = $process.Threads.Count
        externalPerformanceObservers = @($performanceObservers)
    }
    forbiddenProcesses = @($forbidden | Select-Object Name, ProcessId, ExecutablePath)
    residualWorkers = @($residualWorkers | Select-Object Name, ProcessId, ExecutablePath)
    layout = [ordered]@{
        expected = [IO.Path]::GetFullPath($LayoutPath)
        initialProof = $initialLayoutProof
        restartProof = $restartLayoutProof
    }
    failures = @($failures)
}

$manifestDirectory = Split-Path -Parent $ManifestPath
New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $ManifestPath -Encoding UTF8

Write-Host "Runtime QA manifest: $ManifestPath"
Write-Host (("NP p95: {0:N2}ms; idle CPU median/p95: {1:N3}%/{2:N3}%; " +
    "idle/listen memory: {3:N1}MB/{4:N1}MB") -f
    $npP95, $cpuMedian, $cpuP95, $workingSetMax, $listenWorkingSetMb)
if ($failures.Count -gt 0) {
    throw ('Runtime QA failed: ' + ($failures -join '; '))
}
Write-Host 'NekoRoX runtime QA passed.'
