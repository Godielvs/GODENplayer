param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(4, 60)]
    [int]$SampleSeconds = 10,
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo `
        'artifacts\performance-study\crt-idle.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$evidenceRoot = Split-Path -Parent $manifestFull
$beforePath = Join-Path $evidenceRoot 'crt-stopped-before.json'
$afterPath = Join-Path $evidenceRoot 'crt-stopped-after.json'

foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "CRT idle input is missing: $required"
    }
}
New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null

function Get-TargetProcess {
    $matches = @(Get-CimInstance Win32_Process `
        -Filter "Name='foobar2000.exe'" | Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) {
        throw 'Multiple target foobar2000 processes are running.'
    }
    if ($matches.Count -eq 0) { return $null }
    try { return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        return $null
    }
}

function Wait-App([bool]$Present, [int]$TimeoutMs = 20000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-TargetProcess
        if ($Present -and $null -ne $process -and
                $process.MainWindowHandle -ne 0) {
            return $process
        }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 75
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw 'CRT idle target did not reach its expected process state.'
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = @(& $probe @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $($output -join "`n")"
    }
    return ($output -join "`n").Trim()
}

function Get-Percentile([double[]]$Values, [double]$Amount) {
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Max(
        0, [Math]::Ceiling($Amount * $sorted.Count) - 1)
    return [double]$sorted[$index]
}

function Start-GpuSampler([int]$ProcessId, [int]$Seconds) {
    return Start-Job -ScriptBlock {
        param($TargetId, $Count)
        try {
            Get-Counter `
                "\GPU Engine(pid_${TargetId}_*)\Utilization Percentage" `
                -SampleInterval 1 `
                -MaxSamples $Count `
                -ErrorAction Stop |
                ForEach-Object {
                    $valid = @($_.CounterSamples | Where-Object {
                        [double]::IsFinite([double]$_.CookedValue) -and
                        [double]$_.CookedValue -ge 0.0
                    })
                    [pscustomobject]@{
                        total = [double](
                            $valid |
                                Measure-Object CookedValue -Sum).Sum
                        threeD = [double](
                            $valid |
                                Where-Object Path -Match 'engtype_3D' |
                                Measure-Object CookedValue -Sum).Sum
                    }
                }
        }
        catch {
            [pscustomobject]@{ error = $_.Exception.Message }
        }
    } -ArgumentList $ProcessId, $Seconds
}

try {
    if ($null -eq (Get-TargetProcess)) {
        Start-Process -FilePath $appFull
    }
    $process = Wait-App $true
    Invoke-Probe 'mute-on' | Out-Null
    Invoke-Probe 'window-native-maximize' | Out-Null
    Invoke-Probe 'lib' | Out-Null
    Start-Sleep -Seconds 2
    $temporary = Invoke-Probe 'temporary-workspace-state'
    if ($temporary -match 'active=true') {
        Invoke-Probe 'play-library-outside-temporary' | Out-Null
    } else {
        Invoke-Probe 'play-first-album' | Out-Null
    }
    if ((Invoke-Probe 'transport-state') -notmatch 'playing=true') {
        throw 'CRT idle fixture could not start verified library playback.'
    }
    Start-Sleep -Milliseconds 1500
    Invoke-Probe 'listen' | Out-Null
    Start-Sleep -Milliseconds 1800
    Invoke-Probe 'listen-live' | Out-Null
    Start-Sleep -Milliseconds 800
    Invoke-Probe 'listen-crt-stats' $beforePath | Out-Null
    $liveStats = Get-Content -LiteralPath $beforePath -Raw |
        ConvertFrom-Json
    if (-not $liveStats.active) {
        Invoke-Probe 'listen-mode-toggle' | Out-Null
        Start-Sleep -Milliseconds 500
    }

    Invoke-Probe 'stop' | Out-Null
    Start-Sleep -Milliseconds 3000
    $counterClock = [Diagnostics.Stopwatch]::StartNew()
    $beforeProbeStartTicks = $counterClock.ElapsedTicks
    Invoke-Probe 'listen-crt-stats' $beforePath | Out-Null
    $beforeProbeEndTicks = $counterClock.ElapsedTicks
    $before = Get-Content -LiteralPath $beforePath -Raw |
        ConvertFrom-Json
    if (-not $before.active) {
        throw 'CRT surface is inactive after stop.'
    }

    $process = Get-TargetProcess
    $logicalProcessors = [Environment]::ProcessorCount
    $cpuSamples = [Collections.Generic.List[double]]::new()
    $gpuJob = Start-GpuSampler $process.Id $SampleSeconds
    for ($sample = 0; $sample -lt $SampleSeconds; $sample++) {
        $process.Refresh()
        $cpuStart = $process.TotalProcessorTime
        $watch = [Diagnostics.Stopwatch]::StartNew()
        Start-Sleep -Milliseconds 1000
        $process.Refresh()
        $watch.Stop()
        $cpuDelta = ($process.TotalProcessorTime - $cpuStart).TotalSeconds
        $cpuSamples.Add(
            100.0 * $cpuDelta /
                $watch.Elapsed.TotalSeconds /
                $logicalProcessors)
    }

    Wait-Job -Job $gpuJob -Timeout ($SampleSeconds + 10) | Out-Null
    $gpuSamples = @(Receive-Job -Job $gpuJob)
    Remove-Job -Job $gpuJob -Force
    $validGpu = @($gpuSamples | Where-Object {
        $_.PSObject.Properties.Name -notcontains 'error'
    })

    $afterProbeStartTicks = $counterClock.ElapsedTicks
    Invoke-Probe 'listen-crt-stats' $afterPath | Out-Null
    $afterProbeEndTicks = $counterClock.ElapsedTicks
    $counterClock.Stop()
    $after = Get-Content -LiteralPath $afterPath -Raw |
        ConvertFrom-Json
    $counterIntervalSeconds =
        (($afterProbeStartTicks + $afterProbeEndTicks) -
            ($beforeProbeStartTicks + $beforeProbeEndTicks)) /
        (2.0 * [Diagnostics.Stopwatch]::Frequency)
    $presentedFramesDelta =
        [long]$after.presentedFrames - [long]$before.presentedFrames

    $manifest = [ordered]@{
        schema = 'nekorox.crt-idle.v2'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        app = $appFull
        processId = $process.Id
        stoppedSettleSeconds = 3
        sampleSeconds = $SampleSeconds
        counterIntervalSeconds = [double]$counterIntervalSeconds
        presentedFramesPerSecond =
            [double]$presentedFramesDelta / $counterIntervalSeconds
        activeBefore = [bool]$before.active
        activeAfter = [bool]$after.active
        cpu = [ordered]@{
            averagePercent = [double](
                $cpuSamples | Measure-Object -Average).Average
            p95Percent = Get-Percentile $cpuSamples.ToArray() 0.95
            maximumPercent = [double](
                $cpuSamples | Measure-Object -Maximum).Maximum
        }
        gpu3D = if ($validGpu.Count -gt 0) {
            [ordered]@{
                averagePercent = [double](
                    $validGpu | Measure-Object threeD -Average).Average
                p95Percent = Get-Percentile @($validGpu.threeD) 0.95
                maximumPercent = [double](
                    $validGpu | Measure-Object threeD -Maximum).Maximum
            }
        } else { $null }
        deltas = [ordered]@{
            presentedFrames = $presentedFramesDelta
            frameClockPulses =
                [long]$after.frameClockPulses - [long]$before.frameClockPulses
            submittedSegments =
                [long]$after.submittedSegments -
                [long]$before.submittedSegments
        }
        targetRefreshMilliHz = [long]$after.targetRefreshMilliHz
        outputSize = "$($after.outputWidth)x$($after.outputHeight)"
        renderSize = "$($after.renderWidth)x$($after.renderHeight)"
        rawStats = [ordered]@{
            before = $before
            after = $after
        }
    }
    [IO.File]::WriteAllText(
        $manifestFull,
        ($manifest | ConvertTo-Json -Depth 12),
        [Text.UTF8Encoding]::new($false))
    $manifest | ConvertTo-Json -Depth 6
}
finally {
    try { Invoke-Probe 'listen-close' | Out-Null } catch {}
    try { Invoke-Probe 'stop' | Out-Null } catch {}
    try { & $appFull /exit | Out-Null } catch {}
    try { Wait-App $false 15000 | Out-Null } catch {}
}
