param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [double]$Minutes = 30,
    [int]$CheckpointSeconds = 30,
    [string]$EvidenceRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $repo ('artifacts\c1\soak-' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
}
$evidenceFull = [IO.Path]::GetFullPath($EvidenceRoot)
$artifactPrefix = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')) + '\'
if (-not $evidenceFull.StartsWith(
        $artifactPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "C1 soak evidence must stay under artifacts: $evidenceFull"
}
if ($Minutes -le 0 -or $CheckpointSeconds -lt 2) {
    throw 'Minutes and CheckpointSeconds must be positive.'
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "C1 soak input is missing: $required"
    }
}
if (Test-Path -LiteralPath $evidenceFull) {
    throw "C1 soak evidence target already exists: $evidenceFull"
}
New-Item -ItemType Directory -Path $evidenceFull | Out-Null
$jsonl = Join-Path $evidenceFull 'checkpoints.jsonl'

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = (& $probe @arguments 2>&1 | ForEach-Object { $_.ToString() }) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $output"
    }
    return $output
}

function Get-TargetProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -ne 1) {
        throw "Expected one C1 soak process, found $($matches.Count)."
    }
    return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop
}

$initial = Get-TargetProcess
$initialPid = $initial.Id
$component = Join-Path (Split-Path -Parent $appFull) `
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll'
$componentHash = (Get-FileHash -LiteralPath $component -Algorithm SHA256).Hash
[void](Invoke-Probe 'shell-album')
$playback = Invoke-Probe 'playback-state'
if ($playback -match 'state=stopped') {
    [void](Invoke-Probe 'play-first-album')
}

$sizes = @('1280x720', '1600x900', '1920x1080')
$watch = [Diagnostics.Stopwatch]::StartNew()
$durationSeconds = $Minutes * 60.0
$checkpoint = 0
$tabChanges = 0
$resizes = 0
$bookletCycles = 0
$minimizeCycles = 0
$scrollBursts = 0
while ($watch.Elapsed.TotalSeconds -lt $durationSeconds) {
    $process = Get-TargetProcess
    if ($process.Id -ne $initialPid) {
        throw "C1 soak PID changed from $initialPid to $($process.Id)."
    }

    $scenario = $checkpoint % 4
    if ($scenario -eq 0) {
        [void](Invoke-Probe 'shell-curated-caustics')
        [void](Invoke-Probe 'shell-curated-music')
        [void](Invoke-Probe 'shell-curated-palette-original')
        $scenarioName = 'caustics-music-original-colors'
    } elseif ($scenario -eq 1) {
        [void](Invoke-Probe 'shell-curated-caustics')
        [void](Invoke-Probe 'shell-curated-original')
        $scenarioName = 'caustics-original'
    } elseif ($scenario -eq 2) {
        [void](Invoke-Probe 'shell-curated-wave')
        [void](Invoke-Probe 'shell-curated-music')
        [void](Invoke-Probe 'shell-curated-palette-album')
        $scenarioName = 'wave-music-album-colors'
    } else {
        [void](Invoke-Probe 'shell-curated-wave')
        [void](Invoke-Probe 'shell-curated-original')
        $scenarioName = 'wave-original'
    }

    [void](Invoke-Probe 'window-resize-transaction' $sizes[$checkpoint % $sizes.Count])
    ++$resizes
    if (($checkpoint % 3) -eq 0) {
        [void](Invoke-Probe 'shell-library')
        Start-Sleep -Milliseconds 250
        [void](Invoke-Probe 'shell-album')
        ++$tabChanges
    }
    if (($checkpoint % 3) -eq 1) {
        [void](Invoke-Probe 'shell-album-booklet-toggle')
        Start-Sleep -Milliseconds 500
        [void](Invoke-Probe 'shell-album-booklet-toggle')
        ++$bookletCycles
    }
    if (($checkpoint % 6) -eq 2) {
        [void](Invoke-Probe 'window-minimize')
        Start-Sleep -Milliseconds 500
        [void](Invoke-Probe 'window-background-show')
        [void](Invoke-Probe 'shell-album')
        ++$minimizeCycles
    }
    if (($checkpoint % 2) -eq 0) {
        [void](Invoke-Probe 'benchmark-curated-lyrics-scroll-input' '40')
        ++$scrollBursts
    }

    # Let the selected scenario dwell before sampling its performance state.
    # Sampling immediately after a mode switch reports the freshly reset counters
    # instead of the work performed during this checkpoint.
    $next = [Math]::Min($durationSeconds, ($checkpoint + 1) * $CheckpointSeconds)
    while ($watch.Elapsed.TotalSeconds -lt $next) {
        Start-Sleep -Milliseconds 500
        $process = Get-TargetProcess
        if ($process.Id -ne $initialPid) {
            throw "C1 soak PID changed while waiting."
        }
    }

    $playback = Invoke-Probe 'playback-state'
    if ($playback -match 'state=stopped') {
        [void](Invoke-Probe 'play-first-album')
        $playback = Invoke-Probe 'playback-state'
    }
    $state = Invoke-Probe 'shell-curated-state'
    $process.Refresh()
    $record = [ordered]@{
        checkpoint = $checkpoint
        elapsedSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
        utc = [DateTime]::UtcNow.ToString('o')
        pid = $process.Id
        scenario = $scenarioName
        playback = $playback
        curated = $state
        cpuSeconds = [Math]::Round($process.CPU, 6)
        workingSetBytes = $process.WorkingSet64
        privateBytes = $process.PrivateMemorySize64
        handles = $process.HandleCount
    }
    ($record | ConvertTo-Json -Compress) | Add-Content -LiteralPath $jsonl -Encoding UTF8
    Write-Host ("C1_SOAK checkpoint={0} elapsed={1:n1}s pid={2} scenario={3}" -f `
        $checkpoint, $watch.Elapsed.TotalSeconds, $process.Id, $scenarioName)
    ++$checkpoint
}

$final = Get-TargetProcess
[void](Invoke-Probe 'shell-album')
$finalState = Invoke-Probe 'shell-curated-state'
$manifest = [ordered]@{
    schema = 'goden.c1.same-pid-soak.v1'
    passed = $final.Id -eq $initialPid
    durationSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
    initialPid = $initialPid
    finalPid = $final.Id
    checkpoints = $checkpoint
    tabChanges = $tabChanges
    resizes = $resizes
    bookletCycles = $bookletCycles
    minimizeCycles = $minimizeCycles
    scrollBursts = $scrollBursts
    componentSha256 = $componentHash
    finalCuratedState = $finalState
    evidence = $jsonl
}
$manifestPath = Join-Path $evidenceFull 'manifest.json'
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "C1 same-PID soak: PASS"
Write-Host "Manifest: $manifestPath"
