param(
    [string]$App = "$env:LOCALAPPDATA\Programs\Goden\app\foobar2000.exe",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(2, 30)]
    [int]$Transitions = 8,
    [ValidateRange(700, 3000)]
    [int]$SampleMilliseconds = 900,
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $OutputPath) {
    $OutputPath = Join-Path $repo (
        'artifacts\theme-study\theme-transition-' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.json')
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = @(& $probe @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $($output -join "`n")"
    }
    ($output -join "`n").Trim()
}

function Get-TargetProcess {
    $match = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($match.Count -ne 1) { throw 'Expected exactly one target player.' }
    Get-Process -Id $match[0].ProcessId -ErrorAction Stop
}

function Measure-Window([int]$Milliseconds) {
    $process = Get-TargetProcess
    $beforeCpu = $process.TotalProcessorTime.TotalMilliseconds
    $beforeWorkingSet = $process.WorkingSet64
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $maximumWorkingSet = $beforeWorkingSet
    while ($clock.ElapsedMilliseconds -lt $Milliseconds) {
        Start-Sleep -Milliseconds 25
        $process.Refresh()
        $maximumWorkingSet = [Math]::Max(
            [int64]$maximumWorkingSet, [int64]$process.WorkingSet64)
    }
    $clock.Stop()
    $process.Refresh()
    [pscustomobject][ordered]@{
        elapsedMilliseconds = $clock.Elapsed.TotalMilliseconds
        processCpuMilliseconds =
            $process.TotalProcessorTime.TotalMilliseconds - $beforeCpu
        oneCorePercent = 100.0 *
            ($process.TotalProcessorTime.TotalMilliseconds - $beforeCpu) /
            $clock.Elapsed.TotalMilliseconds
        workingSetDeltaBytes = [int64]$process.WorkingSet64 -
            [int64]$beforeWorkingSet
        maximumWorkingSetBytes = [int64]$maximumWorkingSet
    }
}

foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Theme measurement input is missing: $required"
    }
}

$initialTransport = Invoke-Probe 'transport-state'
$initialWindow = Invoke-Probe 'window-state'
Invoke-Probe 'shell-library' | Out-Null
Invoke-Probe 'play' | Out-Null
Start-Sleep -Milliseconds 1200
$stable = @()
$transitionRows = @()
for ($index = 0; $index -lt $Transitions; ++$index) {
    $stable += Measure-Window $SampleMilliseconds
    Invoke-Probe 'next' | Out-Null
    $transition = Measure-Window $SampleMilliseconds
    $render = Invoke-Probe 'workspace-render-state'
    $frames = if ($render -match 'lastThemeFrames=(\d+)') {
        [int]$Matches[1]
    } else { -1 }
    $title = Invoke-Probe 'window-state'
    $transitionRows += [pscustomobject][ordered]@{
        index = $index
        title = $title
        workspaceRenderState = $render
        themePresentedFrames = $frames
        elapsedMilliseconds = $transition.elapsedMilliseconds
        processCpuMilliseconds = $transition.processCpuMilliseconds
        oneCorePercent = $transition.oneCorePercent
        workingSetDeltaBytes = $transition.workingSetDeltaBytes
        maximumWorkingSetBytes = $transition.maximumWorkingSetBytes
    }
}

for ($index = 0; $index -lt $Transitions; ++$index) {
    Invoke-Probe 'previous' | Out-Null
    Start-Sleep -Milliseconds 80
}

$stableCpu = [double](
    $stable.processCpuMilliseconds | Measure-Object -Average).Average
$transitionCpu = [double](
    $transitionRows.processCpuMilliseconds | Measure-Object -Average).Average
$frames = @($transitionRows.themePresentedFrames |
    Where-Object { $_ -ge 0 })
$manifest = [ordered]@{
    schema = 'goden.theme-transition-cost.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    displayRefreshRatesHz = @(Get-CimInstance Win32_VideoController |
        Where-Object CurrentRefreshRate |
        Select-Object -ExpandProperty CurrentRefreshRate)
    transitionDurationMilliseconds = 620
    sampleMilliseconds = $SampleMilliseconds
    transitions = $Transitions
    initialTransport = $initialTransport
    initialWindow = $initialWindow
    stable = $stable
    transitionRows = $transitionRows
    stableCpuAverageMilliseconds = $stableCpu
    transitionCpuAverageMilliseconds = $transitionCpu
    incrementalCpuAverageMilliseconds = $transitionCpu - $stableCpu
    themeFramesAverage = if ($frames.Count -gt 0) {
        [double]($frames | Measure-Object -Average).Average
    } else { -1.0 }
    themeFramesMinimum = if ($frames.Count -gt 0) {
        [int]($frames | Measure-Object -Minimum).Minimum
    } else { -1 }
    themeFramesMaximum = if ($frames.Count -gt 0) {
        [int]($frames | Measure-Object -Maximum).Maximum
    } else { -1 }
    projectedThemeFramesAt30Hz = 19
    finalWindow = Invoke-Probe 'window-state'
    finalTransport = Invoke-Probe 'transport-state'
}

$outputFull = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $outputFull)) | Out-Null
[IO.File]::WriteAllText(
    $outputFull,
    ($manifest | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 8
