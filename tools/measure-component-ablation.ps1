param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$WarmupSeconds = 10,
    [int]$SampleSeconds = 20,
    [ValidateSet('Stopped', 'Playback')]
    [string]$Mode = 'Stopped',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
$appRoot = Split-Path -Parent $appFull
$components = Join-Path $appRoot 'profile\user-components-x64'
$disabled = Join-Path $appRoot 'disabled-components'
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\component-ablation.json'
}
foreach ($required in @($appFull, $probe, $components)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Ablation input is missing: $required"
    }
}
New-Item -ItemType Directory -Force -Path $disabled | Out-Null

function Get-TargetProcess {
    $match = Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        } | Select-Object -First 1
    if ($match) {
        try {
            return Get-Process -Id $match.ProcessId -ErrorAction Stop
        }
        catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
            return $null
        }
    }
    return $null
}

function Stop-Target {
    if ($null -eq (Get-TargetProcess)) { return }
    & $appFull /exit | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $process = Get-TargetProcess
    } while ($null -ne $process -and [DateTime]::UtcNow -lt $deadline)
    if ($null -ne $process) { throw 'Ablation portable did not close.' }
}

function Disable-Component([string]$Name) {
    $source = Join-Path $components $Name
    $destination = Join-Path $disabled $Name
    if (Test-Path -LiteralPath $destination) { return }
    if (Test-Path -LiteralPath $source) {
        Move-Item -LiteralPath $source -Destination $destination
    }
}

function Enable-Component([string]$Name) {
    $source = Join-Path $disabled $Name
    $destination = Join-Path $components $Name
    if (Test-Path -LiteralPath $source) {
        Move-Item -LiteralPath $source -Destination $destination
    }
}

function Percentile([double[]]$Values, [double]$Amount) {
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Ceiling($Amount * $sorted.Count) - 1
    return [double]$sorted[[Math]::Max(0, $index)]
}

function Assert-PlaybackRunning {
    $state = (& $probe $appFull transport-state | Out-String)
    if ($LASTEXITCODE -ne 0) { throw 'Ablation transport state query failed.' }
    if ($state -notmatch 'playing=true') {
        & $appFull /play | Out-Null
        Start-Sleep -Milliseconds 500
        $state = (& $probe $appFull transport-state | Out-String)
    }
    if ($state -notmatch 'playing=true') {
        throw "Ablation playback is not running: $($state.Trim())"
    }
}

function Start-BenchmarkPlayback {
    $temporary = (& $probe $appFull temporary-workspace-state | Out-String)
    if ($LASTEXITCODE -eq 0 -and $temporary -match 'active=true') {
        & $probe $appFull play-library-outside-temporary | Out-Null
    } else {
        & $probe $appFull play-first-album | Out-Null
    }
    if ($LASTEXITCODE -ne 0) { throw 'Ablation playback did not start.' }
    Assert-PlaybackRunning
}

function Measure-Scenario([string]$Name) {
    Start-Process -FilePath $appFull
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        $process = Get-TargetProcess
    } while (($null -eq $process -or $process.MainWindowHandle -eq 0) -and
        [DateTime]::UtcNow -lt $deadline)
    if ($null -eq $process) { throw 'Ablation portable failed to start.' }

    & $probe $appFull window-native-maximize | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Ablation UI did not become interactive.' }
    if ($Mode -eq 'Playback') {
        & $probe $appFull mute-on | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Ablation audio safety failed.' }
        Start-BenchmarkPlayback
    } else {
        & $probe $appFull stop | Out-Null
    }
    Start-Sleep -Seconds $WarmupSeconds
    if ($Mode -eq 'Playback') { Assert-PlaybackRunning }

    $samples = New-Object 'System.Collections.Generic.List[double]'
    $previousCpu = (Get-Process -Id $process.Id).TotalProcessorTime.TotalMilliseconds
    $clock = [Diagnostics.Stopwatch]::StartNew()
    for ($index = 0; $index -lt $SampleSeconds; $index++) {
        Start-Sleep -Seconds 1
        $current = Get-Process -Id $process.Id
        $elapsed = $clock.Elapsed.TotalMilliseconds
        $clock.Restart()
        $delta = $current.TotalProcessorTime.TotalMilliseconds - $previousCpu
        $previousCpu = $current.TotalProcessorTime.TotalMilliseconds
        $samples.Add(100.0 * $delta / $elapsed / [Environment]::ProcessorCount)
    }
    $result = [pscustomobject]@{
        scenario = $Name
        cpuMedianPercent = Percentile $samples.ToArray() 0.5
        cpuP95Percent = Percentile $samples.ToArray() 0.95
        cpuAveragePercent = ($samples | Measure-Object -Average).Average
        workingSetMb = $current.WorkingSet64 / 1MB
        threads = $current.Threads.Count
    }
    Write-Host (("{0}: median {1:N3}%, p95 {2:N3}%, average {3:N3}%, " +
        "working set {4:N1}MB") -f $result.scenario,
        $result.cpuMedianPercent, $result.cpuP95Percent,
        $result.cpuAveragePercent, $result.workingSetMb)
    Stop-Target
    return $result
}

$waveformAvailable =
    (Test-Path -LiteralPath (Join-Path $components 'foo_wave_minibar_mod')) -or
    (Test-Path -LiteralPath (Join-Path $disabled 'foo_wave_minibar_mod'))
$results = @()
try {
    Enable-Component 'foo_vis_spectrum_analyzer'
    Enable-Component 'foo_wave_minibar_mod'
    $results += Measure-Scenario 'full'

    Disable-Component 'foo_vis_spectrum_analyzer'
    $results += Measure-Scenario 'no-spectrum'

    if ($waveformAvailable) {
        Enable-Component 'foo_vis_spectrum_analyzer'
        Disable-Component 'foo_wave_minibar_mod'
        $results += Measure-Scenario 'no-waveform'

        Disable-Component 'foo_vis_spectrum_analyzer'
        $results += Measure-Scenario 'no-spectrum-no-waveform'
    }
}
finally {
    Stop-Target
    Enable-Component 'foo_vis_spectrum_analyzer'
    Enable-Component 'foo_wave_minibar_mod'
}

[ordered]@{
    schema = 'nekorox.component-ablation.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    mode = $Mode.ToLowerInvariant()
    warmupSeconds = $WarmupSeconds
    sampleSeconds = $SampleSeconds
    results = $results
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
Write-Host "Ablation manifest: $ManifestPath"
