param(
    [Parameter(Mandatory = $true)]
    [string]$DevRoot,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$MediaRoot,
    [ValidateRange(30.0, 240.0)]
    [double]$DurationMinutes = 30.0,
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifactRoot = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
if (-not ($devRootFull + '\').StartsWith(
        $artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "R6 soak is restricted to disposable artifacts: $devRootFull"
}
$app = Join-Path $devRootFull 'app\foobar2000.exe'
$component = Join-Path $devRootFull `
    'app\profile\user-components-x64\foo_nekorox\foo_nekorox.dll'
$worker = Join-Path $devRootFull `
    'app\profile\user-components-x64\foo_nekorox\nekorox-analysis.exe'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\r6\soak30-a1.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$evidenceRoot = Join-Path (Split-Path -Parent $manifestFull) 'soak30-evidence'
foreach ($required in @($app, $component, $worker, $probe, $MediaRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "R6 soak input is missing: $required"
    }
}

function Invoke-Probe(
    [string]$Action,
    [string]$Argument = '',
    [switch]$Foreground,
    [switch]$AllowFailure
) {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    if (-not $Foreground) { $arguments += '--background' }
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $probe @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $prior }
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "R6 soak probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{ Success = $exitCode -eq 0; Output = $text }
}

function Stop-Candidate([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    Invoke-Probe 'app-close' -Foreground -AllowFailure | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        # This process is the exact disposable executable launched above.
        # A failed run may lose its window before the probe can request exit;
        # terminate only that PID so the original gate failure is preserved.
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit(5000) | Out-Null
    }
}

function Set-AmbientMode([int]$Mode) {
    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'ambient-mode' ([string]$Mode) | Out-Null
    Invoke-Probe 'options-close' | Out-Null
    Invoke-Probe 'shell-album' | Out-Null
}

function Enable-LowVolumeAudio {
    $transport = (Invoke-Probe 'transport-state').Output
    if ($transport -match 'muted=true') {
        Invoke-Probe 'mute-toggle' | Out-Null
    }
    Invoke-Probe 'volume-set' '1' | Out-Null
}

function Get-TextSha256([string]$Value) {
    $stream = [IO.MemoryStream]::new(
        [Text.Encoding]::UTF8.GetBytes($Value))
    try {
        return (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash
    }
    finally { $stream.Dispose() }
}

function Read-AmbientState {
    $text = (Invoke-Probe 'shell-album-ambient-state').Output
    $pattern = 'mode=(?<mode>\d+) fallback=(?<fallback>\d+) textureUpdates=(?<updates>\d+) featurePublications=(?<publications>\d+) resourceBytes=(?<bytes>\d+) gpuUs=(?<gpu>\d+) gpuP95Us=(?<p95>\d+) gpuMeasured=(?<measured>true|false) degraded=(?<degraded>true|false)'
    if ($text -notmatch $pattern) {
        throw "Could not parse Album ambient state: $text"
    }
    return [pscustomobject]@{
        raw = $text
        mode = [int]$Matches.mode
        fallback = [int]$Matches.fallback
        textureUpdates = [uint64]$Matches.updates
        featurePublications = [uint64]$Matches.publications
        resourceBytes = [uint64]$Matches.bytes
        gpuMicroseconds = [double]$Matches.gpu
        gpuP95Microseconds = [double]$Matches.p95
        gpuMeasured = $Matches.measured -eq 'true'
        degraded = $Matches.degraded -eq 'true'
    }
}

function Add-Sample([string]$Label) {
    $process.Refresh()
    if ($process.HasExited -or -not $process.Responding) {
        throw "R6 soak host became unhealthy at $Label."
    }
    $ambient = Read-AmbientState
    $samples.Add([pscustomobject]@{
        label = $Label
        minute = [Math]::Round($watch.Elapsed.TotalMinutes, 3)
        cpuMilliseconds =
            $process.TotalProcessorTime.TotalMilliseconds - $cpuStart
        handles = $process.HandleCount
        threads = $process.Threads.Count
        workingSet = $process.WorkingSet64
        responding = $process.Responding
        ambient = $ambient
        frameClocks = (Invoke-Probe 'frame-clock-state').Output
        playback = (Invoke-Probe 'playback-state').Output
        transport = (Invoke-Probe 'transport-state').Output
    })
}

$audioExtensions = '\.(flac|mp3|m4a|aac|ogg|opus|wav|wma)$'
$files = @(rg --files $MediaRoot |
    Where-Object { $_ -match $audioExtensions } |
    Group-Object { Split-Path -Parent $_ } |
    Where-Object Count -ge 2 |
    Select-Object -First 20 |
    ForEach-Object { $_.Group | Select-Object -First 4 })
if ($files.Count -lt 40) {
    throw 'R6 soak needs at least 40 authorized tracks across varied folders.'
}
$files = @($files | Select-Object -First 60)

New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
Invoke-Probe 'app-close' -Foreground -AllowFailure | Out-Null
Start-Sleep -Milliseconds 300
$runningMarker = Join-Path $devRootFull 'app\profile\running'
if (Test-Path -LiteralPath $runningMarker -PathType Leaf) {
    Remove-Item -LiteralPath $runningMarker -Force
}
$env:GODEN_FLAG_ALBUM_AMBIENT_A1 = '1'
$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
$env:NEKOROX_QA_BACKGROUND = '1'
$process = $null
try {
    $process = Start-Process -FilePath $app `
        -WorkingDirectory (Split-Path -Parent $app) `
        -WindowStyle Hidden -PassThru
}
finally {
    Remove-Item Env:GODEN_FLAG_ALBUM_AMBIENT_A1, Env:NEKOROX_QA_HOOKS,
        Env:NEKOROX_QA_PROFILE, Env:NEKOROX_QA_BACKGROUND `
        -ErrorAction SilentlyContinue
}

$samples = [Collections.Generic.List[object]]::new()
$actions = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[object]]::new()
$watch = [Diagnostics.Stopwatch]::StartNew()
$startedUtc = [DateTime]::UtcNow
$trackIndex = 0
$menuCount = 0
$bookletCycles = 0
$listenCycles = 0
$modeChanges = 0
$tagCycles = 0
$historyCycles = 0
$pauseCycles = 0
$seekCycles = 0
$muteCycles = 0
try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        if ((Invoke-Probe 'window-state' -AllowFailure).Success) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 25
    }
    if (-not $ready) { throw 'R6 soak candidate did not publish its window.' }
    Invoke-Probe 'window-offscreen-show' | Out-Null
    Enable-LowVolumeAudio
    Invoke-Probe 'drop-file' $files[0] | Out-Null
    Start-Sleep -Milliseconds 700
    Invoke-Probe 'shell-album' | Out-Null
    Set-AmbientMode 1
    Start-Sleep -Seconds 2

    $process.Refresh()
    $handleStart = $process.HandleCount
    $threadStart = $process.Threads.Count
    $workingSetStart = $process.WorkingSet64
    $cpuStart = $process.TotalProcessorTime.TotalMilliseconds
    Add-Sample '0'

    $nextTrackAt = 8.0
    $nextMenuAt = 15.0
    $nextBookletAt = 35.0
    $nextListenAt = 55.0
    $nextTagAt = 210.0
    $nextHistoryAt = 300.0
    $nextMinuteSample = 60.0
    $special15Captured = $false
    $modeSchedule = @(
        [pscustomobject]@{ second = 480.0; mode = 2 },
        [pscustomobject]@{ second = 960.0; mode = 0 },
        [pscustomobject]@{ second = 1080.0; mode = 1 },
        [pscustomobject]@{ second = 1440.0; mode = 2 },
        [pscustomobject]@{ second = 1620.0; mode = 1 }
    )
    $modeIndex = 0

    while ($watch.Elapsed.TotalMinutes -lt $DurationMinutes) {
        $seconds = $watch.Elapsed.TotalSeconds
        if ($trackIndex -lt 40 -and $seconds -ge $nextTrackAt) {
            $file = $files[$trackIndex % $files.Count]
            Invoke-Probe 'drop-file' $file | Out-Null
            Start-Sleep -Milliseconds 180
            Invoke-Probe 'shell-album' | Out-Null
            $actions.Add([pscustomobject]@{
                kind = 'track'
                index = $trackIndex
                second = [Math]::Round($seconds, 1)
                fileToken = Get-TextSha256 ([IO.Path]::GetRelativePath(
                    $MediaRoot, $file))
                parentToken = Get-TextSha256 (
                    Split-Path -Leaf (Split-Path -Parent $file))
            })
            ++$trackIndex
            $nextTrackAt += 42.0
        }
        if ($menuCount -lt 20 -and $seconds -ge $nextMenuAt) {
            $path = Join-Path $evidenceRoot ("menu-{0:D2}.bmp" -f $menuCount)
            $menu = Invoke-Probe 'menu-capture' $path -Foreground -AllowFailure
            if (-not $menu.Success) {
                $failures.Add([pscustomobject]@{
                    kind = 'menu'; second = $seconds; output = $menu.Output })
            }
            ++$menuCount
            $nextMenuAt += 85.0
        }
        if ($bookletCycles -lt 10 -and $seconds -ge $nextBookletAt) {
            $open = Invoke-Probe 'shell-album-booklet-toggle' -AllowFailure
            Start-Sleep -Milliseconds 650
            $close = Invoke-Probe 'shell-album-booklet-toggle' -AllowFailure
            if (-not $open.Success -or -not $close.Success) {
                $failures.Add([pscustomobject]@{
                    kind = 'booklet'; second = $seconds
                    output = $open.Output + "`n" + $close.Output })
            }
            ++$bookletCycles
            $nextBookletAt += 170.0
        }
        if ($listenCycles -lt 10 -and $seconds -ge $nextListenAt) {
            Invoke-Probe 'listen' -Foreground | Out-Null
            Start-Sleep -Milliseconds 100
            $focus = @('listen-focus-scope', 'listen-focus-crt',
                'listen-focus-spectrum', 'listen-focus-spectrogram')[
                    $listenCycles % 4]
            Invoke-Probe $focus | Out-Null
            Start-Sleep -Milliseconds 120
            Invoke-Probe 'listen-close' | Out-Null
            Invoke-Probe 'shell-album' | Out-Null
            ++$listenCycles
            $nextListenAt += 170.0
        }
        if ($tagCycles -lt 5 -and $seconds -ge $nextTagAt) {
            Invoke-Probe 'tag-open' | Out-Null
            Invoke-Probe 'tag-scroll-cycle' | Out-Null
            Invoke-Probe 'tag-close' | Out-Null
            ++$tagCycles
            $nextTagAt += 330.0
        }
        if ($historyCycles -lt 4 -and $seconds -ge $nextHistoryAt) {
            Invoke-Probe 'shell-library' | Out-Null
            Invoke-Probe 'history' | Out-Null
            Invoke-Probe 'filter' | Out-Null
            Invoke-Probe 'shell-album' | Out-Null
            ++$historyCycles
            $nextHistoryAt += 360.0
        }
        if ($modeIndex -lt $modeSchedule.Count -and
                $seconds -ge $modeSchedule[$modeIndex].second) {
            Set-AmbientMode $modeSchedule[$modeIndex].mode
            $actions.Add([pscustomobject]@{
                kind = 'ambient-mode'
                mode = $modeSchedule[$modeIndex].mode
                second = [Math]::Round($seconds, 1) })
            ++$modeIndex
            ++$modeChanges
        }
        if ($pauseCycles -lt 3 -and $seconds -ge 240.0 + 420.0 * $pauseCycles) {
            Invoke-Probe 'play' | Out-Null
            Start-Sleep -Seconds 3
            Invoke-Probe 'play' | Out-Null
            ++$pauseCycles
        }
        if ($seekCycles -lt 3 -and $seconds -ge 360.0 + 420.0 * $seekCycles) {
            Invoke-Probe 'waveseek-seek-percent' ([string](25 + 20 * $seekCycles)) |
                Out-Null
            ++$seekCycles
        }
        if ($muteCycles -lt 3 -and $seconds -ge 420.0 + 420.0 * $muteCycles) {
            Invoke-Probe 'mute-toggle' | Out-Null
            Start-Sleep -Milliseconds 250
            Invoke-Probe 'mute-toggle' | Out-Null
            ++$muteCycles
        }
        if (-not $special15Captured -and $seconds -ge 900.0) {
            Add-Sample '15'
            $special15Captured = $true
        }
        if ($seconds -ge $nextMinuteSample) {
            Add-Sample ("minute-{0}" -f [int]($nextMinuteSample / 60.0))
            $nextMinuteSample += 60.0
        }
        Start-Sleep -Milliseconds 180
    }

    Set-AmbientMode 1
    Start-Sleep -Seconds 2
    Add-Sample '30'
    $process.Refresh()
    $finalAmbient = Read-AmbientState
    $duration = $watch.Elapsed.TotalMinutes
    $requiredCountsPassed = $trackIndex -ge 40 -and $menuCount -ge 20 -and
        $bookletCycles -ge 10 -and $listenCycles -ge 10
    $ambientPassed = $finalAmbient.fallback -eq 0 -and
        $finalAmbient.gpuMeasured -and
        $finalAmbient.gpuP95Microseconds -le 500.0 -and
        $finalAmbient.resourceBytes -le 67108864
    $result = [ordered]@{
        schema = 'goden.r6.ambient-soak30.v1'
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        startedUtc = $startedUtc.ToString('o')
        status = if ($requiredCountsPassed -and $ambientPassed -and
            $failures.Count -eq 0 -and $duration -ge 30.0) { 'pass' } else { 'fail' }
        durationMinutes = $duration
        sameProcess = $true
        processId = $process.Id
        logicalProcessors = [Environment]::ProcessorCount
        physicalMonitorCount = [System.Windows.Forms.Screen]::AllScreens.Count
        physicalMonitorExchangeCovered = $false
        physicalMonitorLimitation = 'No automated native-app monitor movement was available in this session; device-loss/epoch seams are covered separately.'
        componentSha256 = (Get-FileHash -LiteralPath $component -Algorithm SHA256).Hash
        workerSha256 = (Get-FileHash -LiteralPath $worker -Algorithm SHA256).Hash
        tracksExercised = $trackIndex
        menuInteractions = $menuCount
        bookletEntryExitCycles = $bookletCycles
        listenFocusCycles = $listenCycles
        tagCycles = $tagCycles
        historyCycles = $historyCycles
        modeChanges = $modeChanges
        pauseCycles = $pauseCycles
        seekCycles = $seekCycles
        muteCycles = $muteCycles
        handleStart = $handleStart
        handleEnd = $process.HandleCount
        threadStart = $threadStart
        threadEnd = $process.Threads.Count
        workingSetStart = $workingSetStart
        workingSetEnd = $process.WorkingSet64
        finalAmbient = $finalAmbient
        requiredCountsPassed = $requiredCountsPassed
        ambientBudgetPassed = $ambientPassed
        samples = $samples
        actions = $actions
        failures = $failures
    }
    New-Item -ItemType Directory -Force `
        -Path (Split-Path -Parent $manifestFull) | Out-Null
    $result | ConvertTo-Json -Depth 10 |
        Set-Content -LiteralPath $manifestFull -Encoding UTF8
    $result | ConvertTo-Json -Depth 4
    if ($result.status -ne 'pass') {
        throw "R6 ambient soak did not satisfy its gate. See $manifestFull"
    }
}
finally {
    Stop-Candidate $process
}
