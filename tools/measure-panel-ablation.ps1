param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [int]$WarmupSeconds = 2,
    [int]$SampleSeconds = 6,
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
$logicalProcessors = [Environment]::ProcessorCount
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\panel-ablation.json'
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Panel ablation input is missing: $required"
    }
}

if (-not ('NekoRoXPanelAblationNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class NekoRoXPanelAblationNative {
    private delegate bool EnumWindowProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(
        IntPtr parent, EnumWindowProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window, out uint processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(
        IntPtr window, StringBuilder className, int maximum);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr window, int command);

    private const int Hide = 0;

    public static int HideClasses(uint processId, string[] classNames) {
        HashSet<string> wanted = new HashSet<string>(
            classNames, StringComparer.OrdinalIgnoreCase);
        int hidden = 0;
        EnumWindows((topLevel, topParameter) => {
            uint owner;
            GetWindowThreadProcessId(topLevel, out owner);
            if (owner == processId) {
                EnumChildWindows(topLevel, (window, childParameter) => {
                    StringBuilder name = new StringBuilder(256);
                    if (GetClassName(window, name, name.Capacity) > 0 &&
                            wanted.Contains(name.ToString())) {
                        ShowWindow(window, Hide);
                        hidden++;
                    }
                    return true;
                }, IntPtr.Zero);
            }
            return true;
        }, IntPtr.Zero);
        return hidden;
    }
}
'@
}

function Get-TargetProcess {
    $match = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($match.Count -eq 0) { return $null }
    if ($match.Count -gt 1) { throw 'Multiple panel-ablation targets are running.' }
    try { return Get-Process -Id $match[0].ProcessId -ErrorAction Stop }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] { return $null }
}

function Invoke-Probe([string]$Action) {
    $output = @(& $probe $appFull $Action 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $($output -join "`n")"
    }
    return ($output -join "`n").Trim()
}

function Stop-Target {
    if ($null -eq (Get-TargetProcess)) { return }
    & $appFull /exit | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $process = Get-TargetProcess
    } while ($null -ne $process -and [DateTime]::UtcNow -lt $deadline)
    if ($null -ne $process) { throw 'Panel-ablation target did not close.' }
}

function Assert-PlaybackRunning {
    $state = Invoke-Probe 'transport-state'
    if ($state -notmatch 'playing=true') {
        & $appFull /play | Out-Null
        Start-Sleep -Milliseconds 500
        $state = Invoke-Probe 'transport-state'
    }
    if ($state -notmatch 'playing=true') {
        throw "Panel-ablation playback is not running: $state"
    }
}

function Start-BenchmarkPlayback {
    $temporary = Invoke-Probe 'temporary-workspace-state'
    if ($temporary -match 'active=true') {
        Invoke-Probe 'play-library-outside-temporary' | Out-Null
    } else {
        Invoke-Probe 'play-first-album' | Out-Null
    }
    Assert-PlaybackRunning
}

function Get-Percentile([double[]]$Values, [double]$Amount) {
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Max(0, [Math]::Ceiling($Amount * $sorted.Count) - 1)
    return [double]$sorted[$index]
}

function Measure-Scenario(
    [string]$Name,
    [string[]]$HiddenClasses
) {
    Start-Process -FilePath $appFull
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $process = Get-TargetProcess
    } while (($null -eq $process -or $process.MainWindowHandle -eq 0) -and
        [DateTime]::UtcNow -lt $deadline)
    if ($null -eq $process -or $process.MainWindowHandle -eq 0) {
        throw 'Panel-ablation target did not become interactive.'
    }

    Invoke-Probe 'window-native-maximize' | Out-Null
    Invoke-Probe 'mute-on' | Out-Null
    Start-BenchmarkPlayback
    Start-Sleep -Seconds $WarmupSeconds
    Assert-PlaybackRunning

    $hidden = if ($HiddenClasses.Count -gt 0) {
        [NekoRoXPanelAblationNative]::HideClasses(
            [uint32]$process.Id,
            $HiddenClasses)
    } else {
        0
    }
    if ($hidden -ne $HiddenClasses.Count) {
        throw "Panel ablation '$Name' hid $hidden of $($HiddenClasses.Count) panels."
    }
    Start-Sleep -Seconds 1

    $samples = New-Object 'System.Collections.Generic.List[double]'
    $previousCpuMs =
        (Get-Process -Id $process.Id).TotalProcessorTime.TotalMilliseconds
    $clock = [Diagnostics.Stopwatch]::StartNew()
    for ($index = 0; $index -lt $SampleSeconds * 2; ++$index) {
        Start-Sleep -Milliseconds 500
        $current = Get-Process -Id $process.Id
        $elapsedMs = $clock.Elapsed.TotalMilliseconds
        $clock.Restart()
        $deltaMs =
            $current.TotalProcessorTime.TotalMilliseconds - $previousCpuMs
        $previousCpuMs = $current.TotalProcessorTime.TotalMilliseconds
        $samples.Add(
            100.0 * $deltaMs / $elapsedMs / $logicalProcessors)
    }

    $frameClocks = try { Invoke-Probe 'frame-clock-state' } catch { 'unavailable' }
    $result = [pscustomobject][ordered]@{
        scenario = $Name
        hiddenClasses = $HiddenClasses
        hiddenWindows = $hidden
        cpuAveragePercent = [double](
            $samples | Measure-Object -Average).Average
        cpuMedianPercent = Get-Percentile $samples.ToArray() 0.5
        cpuP95Percent = Get-Percentile $samples.ToArray() 0.95
        cpuMaxPercent = [double](
            $samples | Measure-Object -Maximum).Maximum
        workingSetMb = [double]($current.WorkingSet64 / 1MB)
        frameClocks = $frameClocks
    }
    Stop-Target
    return $result
}

$scenarios = @(
    [pscustomobject]@{ Name = 'full'; Classes = @() },
    [pscustomobject]@{ Name = 'without-header'; Classes = @('NekoRoXHeaderV3') },
    [pscustomobject]@{ Name = 'without-transport'; Classes = @('NekoRoXTransportV3') },
    [pscustomobject]@{ Name = 'without-waveseek'; Classes = @('NekoRoXWaveSeekV3') },
    [pscustomobject]@{
        Name = 'without-three-realtime-panels'
        Classes = @(
            'NekoRoXHeaderV3',
            'NekoRoXTransportV3',
            'NekoRoXWaveSeekV3')
    }
)

$results = @()
try {
    Stop-Target
    foreach ($scenario in $scenarios) {
        $results += Measure-Scenario $scenario.Name $scenario.Classes
    }
}
finally {
    Stop-Target
}

$manifest = [ordered]@{
    schema = 'nekorox.panel-ablation.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    logicalProcessors = $logicalProcessors
    warmupSeconds = $WarmupSeconds
    sampleSeconds = $SampleSeconds
    results = $results
}
$fullManifestPath = [IO.Path]::GetFullPath($ManifestPath)
[IO.Directory]::CreateDirectory(
    (Split-Path -Parent $fullManifestPath)) | Out-Null
[IO.File]::WriteAllText(
    $fullManifestPath,
    ($manifest | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false))

$manifest | ConvertTo-Json -Depth 8
