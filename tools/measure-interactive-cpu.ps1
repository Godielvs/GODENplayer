param(
    [string]$App = "$env:LOCALAPPDATA\Programs\Goden\app\foobar2000.exe",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$SampleSeconds = 10,
    [ValidateSet('handler-message', 'send-input')]
    [string]$InputMode = 'handler-message',
    [string]$WheelScenario = 'wheel_16hz',
    [string]$WheelSchedulePath = '',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\cpu-study\interactive-cpu.json'
}
if (-not $WheelSchedulePath) {
    $WheelSchedulePath = Join-Path $repo 'tests\fixtures\wheel_schedules.json'
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Interactive CPU measurement input is missing: $required"
    }
}
if (-not (Test-Path -LiteralPath $WheelSchedulePath -PathType Leaf)) {
    throw "Wheel schedule fixture is missing: $WheelSchedulePath"
}
if ($SampleSeconds -lt 3) {
    throw 'SampleSeconds must be at least 3.'
}

$logicalProcessors = [Environment]::ProcessorCount

function Get-TargetProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) {
        throw 'Multiple target foobar2000 processes are running.'
    }
    if ($matches.Count -eq 0) { return $null }
    try {
        return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop
    }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        return $null
    }
}

function Invoke-Probe(
    [string]$Action,
    [string]$Argument = ''
) {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = @(& $probe @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $($output -join "`n")"
    }
    return ($output -join "`n").Trim()
}

function Get-Percentile(
    [double[]]$Values,
    [double]$Percentile
) {
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Max(
        0,
        [Math]::Ceiling($Percentile * $sorted.Count) - 1)
    return [double]$sorted[$index]
}

function Measure-CpuScenario(
    [string]$Name,
    [string]$DriverSchedule = ''
) {
    $process = Get-TargetProcess
    if ($null -eq $process) { throw 'Target foobar2000 is not running.' }

    $threadBefore = @{}
    foreach ($thread in $process.Threads) {
        $threadBefore[[int]$thread.Id] =
            [double]$thread.TotalProcessorTime.TotalMilliseconds
    }

    $samples = New-Object 'System.Collections.Generic.List[double]'
    $previousCpuMs = $process.TotalProcessorTime.TotalMilliseconds
    $totalCpuDeltaMs = 0.0
    $sampleClock = [Diagnostics.Stopwatch]::StartNew()
    $scenarioClock = [Diagnostics.Stopwatch]::StartNew()
    $driverProcess = $null
    $driverOutput = ''
    $driverError = ''
    $driverStdout = $null
    $driverStderr = $null
    if ($DriverSchedule) {
        $driverStdout = [IO.Path]::GetTempFileName()
        $driverStderr = [IO.Path]::GetTempFileName()
        $driverProcess = Start-Process -FilePath $probe -ArgumentList @(
            ('"' + $appFull + '"'),
            'workpack-wheel-driver',
            $DriverSchedule
        ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $driverStdout `
            -RedirectStandardError $driverStderr
    }
    while ($scenarioClock.Elapsed.TotalSeconds -lt $SampleSeconds) {
        Start-Sleep -Milliseconds 250
        $current = Get-Process -Id $process.Id -ErrorAction Stop
        $elapsedMs = $sampleClock.Elapsed.TotalMilliseconds
        $sampleClock.Restart()
        $cpuDeltaMs =
            $current.TotalProcessorTime.TotalMilliseconds - $previousCpuMs
        $totalCpuDeltaMs += $cpuDeltaMs
        $previousCpuMs = $current.TotalProcessorTime.TotalMilliseconds
        $samples.Add(
            100.0 * $cpuDeltaMs / $elapsedMs / $logicalProcessors)
    }
    $scenarioClock.Stop()
    if ($null -ne $driverProcess) {
        if (-not $driverProcess.WaitForExit(5000)) {
            $driverProcess.Kill()
            throw "Persistent input driver did not exit for '$Name'."
        }
        $driverOutput = ([IO.File]::ReadAllText($driverStdout)).Trim()
        $driverError = ([IO.File]::ReadAllText($driverStderr)).Trim()
        Remove-Item -LiteralPath $driverStdout, $driverStderr -Force
        if ($driverProcess.ExitCode -ne 0) {
            throw "Persistent input driver failed for '$Name': $driverError $driverOutput"
        }
    }

    $process = Get-Process -Id $process.Id -ErrorAction Stop
    $threadRows = @()
    foreach ($thread in $process.Threads) {
        $before = if ($threadBefore.ContainsKey([int]$thread.Id)) {
            $threadBefore[[int]$thread.Id]
        } else {
            0.0
        }
        $deltaMs =
            [double]$thread.TotalProcessorTime.TotalMilliseconds - $before
        if ($deltaMs -le 0.0) { continue }
        $threadRows += [pscustomobject]@{
            id = [int]$thread.Id
            cpuMs = $deltaMs
            totalCpuPercent = 100.0 * $deltaMs /
                $scenarioClock.Elapsed.TotalMilliseconds / $logicalProcessors
        }
    }

    return [pscustomobject][ordered]@{
        scenario = $Name
        seconds = [double]$scenarioClock.Elapsed.TotalSeconds
        cpuAveragePercent = [double](
            $samples | Measure-Object -Average).Average
        cpuMedianPercent = Get-Percentile $samples.ToArray() 0.5
        cpuP95Percent = Get-Percentile $samples.ToArray() 0.95
        cpuMaxPercent = [double](
            $samples | Measure-Object -Maximum).Maximum
        coreMillisecondsPerSecond = [double](
            $totalCpuDeltaMs / $scenarioClock.Elapsed.TotalSeconds)
        processCpuPercentOneCoreScale = [double](
            100.0 * $totalCpuDeltaMs /
                $scenarioClock.Elapsed.TotalMilliseconds)
        workingSetMb = [double]($process.WorkingSet64 / 1MB)
        threadCount = $process.Threads.Count
        inputDriver = if ($driverOutput) { $driverOutput } else { $null }
        hottestThreads = @($threadRows |
            Sort-Object cpuMs -Descending |
            Select-Object -First 12)
        frameClocksAfter = Invoke-Probe 'frame-clock-state'
    }
}

$fixture = Get-Content -LiteralPath $WheelSchedulePath -Raw |
    ConvertFrom-Json
if ($fixture.schema -ne 'goden.wheel_schedule.v1') {
    throw "Unsupported wheel schedule schema: $($fixture.schema)"
}
$selectedSchedule = @($fixture.scenarios |
    Where-Object id -EQ $WheelScenario)
if ($selectedSchedule.Count -ne 1) {
    throw "Wheel scenario '$WheelScenario' was not found uniquely."
}
$loopUs = [int64]$selectedSchedule[0].duration_ms * 1000
$totalUs = [int64]$SampleSeconds * 1000000
$encodedEvents = @($selectedSchedule[0].events | ForEach-Object {
    "$([int64]$_.t_us):$([int]$_.delta_wheel)"
}) -join ','
$encodedSchedule = "$InputMode|$totalUs|$loopUs|$encodedEvents"

$process = Get-TargetProcess
if ($null -eq $process) {
    Start-Process -FilePath $appFull
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $process = Get-TargetProcess
    } while (($null -eq $process -or $process.MainWindowHandle -eq 0) -and
        [DateTime]::UtcNow -lt $deadline)
}
if ($null -eq $process -or $process.MainWindowHandle -eq 0) {
    throw 'Target foobar2000 did not become interactive.'
}

$initialTransport = Invoke-Probe 'transport-state'
$initiallyMuted = $initialTransport -match 'muted=true'
$initialVolume = if ($initialTransport -match 'volume=([0-9]+)') {
    [int]$Matches[1]
} else {
    1023
}
$results = @()
try {
    Invoke-Probe 'window-native-maximize' | Out-Null
    Invoke-Probe 'lib' | Out-Null
    Invoke-Probe 'stop' | Out-Null
    Start-Sleep -Seconds 2
    $results += Measure-CpuScenario 'stopped-stable'

    if (-not $initiallyMuted) { Invoke-Probe 'mute-on' | Out-Null }
    Invoke-Probe 'play-first-album' | Out-Null
    Start-Sleep -Seconds 2
    $results += Measure-CpuScenario 'playback-stable'

    Invoke-Probe 'stop' | Out-Null
    Start-Sleep -Seconds 2
    $results += Measure-CpuScenario `
        'stopped-continuous-scroll' $encodedSchedule

    Invoke-Probe 'play-first-album' | Out-Null
    Start-Sleep -Seconds 2
    $results += Measure-CpuScenario `
        'playback-continuous-scroll' $encodedSchedule
}
finally {
    try { Invoke-Probe 'stop' | Out-Null } catch {}
    try { Invoke-Probe 'lib' | Out-Null } catch {}
    if (-not $initiallyMuted) {
        # mute-on lowers foobar's native volume to the mute floor rather than
        # preserving a separate toggle state. Restore the measured level; 99%
        # stays inside the volume hit rectangle while mapping to practical max.
        $restorePercent = [Math]::Min(99, [Math]::Max(1,
            [int][Math]::Round(100.0 * $initialVolume / 1023.0)))
        try { Invoke-Probe 'volume-set' "$restorePercent" | Out-Null } catch {}
    }
}

$finalTransport = Invoke-Probe 'transport-state'
$displayRates = @(Get-CimInstance Win32_VideoController |
    Where-Object CurrentRefreshRate |
    Select-Object -ExpandProperty CurrentRefreshRate)
$manifest = [ordered]@{
    schema = 'nekorox.interactive-cpu.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    logicalProcessors = $logicalProcessors
    displayRefreshRatesHz = $displayRates
    sampleSeconds = $SampleSeconds
    inputMode = $InputMode
    wheelScenario = $WheelScenario
    wheelSchedulePath = [IO.Path]::GetFullPath($WheelSchedulePath)
    initialTransport = $initialTransport
    finalTransport = $finalTransport
    results = $results
}

$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $manifestFull)) | Out-Null
[IO.File]::WriteAllText(
    $manifestFull,
    ($manifest | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false))

$manifest | ConvertTo-Json -Depth 8
