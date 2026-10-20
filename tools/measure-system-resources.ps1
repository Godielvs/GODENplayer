param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(4, 120)]
    [int]$SampleSeconds = 10,
    [ValidateRange(1, 120)]
    [int]$WarmupSeconds = 3,
    [ValidateRange(1, 100)]
    [int]$ScrollEventsPerBurst = 24,
    [ValidateSet('Full', 'Visibility', 'Listen')]
    [string]$ScenarioGroup = 'Full',
    [string[]]$HiddenPanelClasses = @(),
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\performance-study\system-resources.json'
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "System resource input is missing: $required"
    }
}

if (-not ('NekoRoXResourceNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class NekoRoXResourceNative {
    private delegate bool EnumWindowProc(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    public struct IoCounters {
        public UInt64 ReadOperationCount;
        public UInt64 WriteOperationCount;
        public UInt64 OtherOperationCount;
        public UInt64 ReadTransferCount;
        public UInt64 WriteTransferCount;
        public UInt64 OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessMemoryCountersEx {
        public UInt32 cb;
        public UInt32 PageFaultCount;
        public UIntPtr PeakWorkingSetSize;
        public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage;
        public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage;
        public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessIoCounters(
        IntPtr process, out IoCounters counters);

    [DllImport("psapi.dll", SetLastError = true)]
    private static extern bool GetProcessMemoryInfo(
        IntPtr process,
        out ProcessMemoryCountersEx counters,
        UInt32 size);

    [DllImport("user32.dll")]
    private static extern UInt32 GetGuiResources(IntPtr process, UInt32 flags);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(
        EnumWindowProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(
        IntPtr parent, EnumWindowProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern UInt32 GetWindowThreadProcessId(
        IntPtr window, out UInt32 processId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(
        IntPtr window,
        System.Text.StringBuilder className,
        int maximum);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr window, int command);

    public sealed class Snapshot {
        public IoCounters Io;
        public UInt32 PageFaults;
        public UInt64 PrivateBytes;
        public UInt32 GdiObjects;
        public UInt32 UserObjects;
    }

    public static Snapshot Read(IntPtr process) {
        IoCounters io;
        if (!GetProcessIoCounters(process, out io)) {
            throw new System.ComponentModel.Win32Exception();
        }
        ProcessMemoryCountersEx memory = new ProcessMemoryCountersEx();
        memory.cb = (UInt32)Marshal.SizeOf<ProcessMemoryCountersEx>();
        if (!GetProcessMemoryInfo(process, out memory, memory.cb)) {
            throw new System.ComponentModel.Win32Exception();
        }
        return new Snapshot {
            Io = io,
            PageFaults = memory.PageFaultCount,
            PrivateBytes = memory.PrivateUsage.ToUInt64(),
            GdiObjects = GetGuiResources(process, 0),
            UserObjects = GetGuiResources(process, 1),
        };
    }

    public static int HideClasses(UInt32 processId, string[] classNames) {
        var wanted = new System.Collections.Generic.HashSet<string>(
            classNames, StringComparer.OrdinalIgnoreCase);
        int hidden = 0;
        EnumWindows((topLevel, topParameter) => {
            UInt32 owner;
            GetWindowThreadProcessId(topLevel, out owner);
            if (owner == processId) {
                EnumChildWindows(topLevel, (window, childParameter) => {
                    var name = new System.Text.StringBuilder(256);
                    if (GetClassName(window, name, name.Capacity) > 0 &&
                            wanted.Contains(name.ToString())) {
                        ShowWindow(window, 0);
                        ++hidden;
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
    try { return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] { return $null }
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

function Assert-PlaybackRunning {
    $state = Invoke-Probe 'transport-state'
    if ($state -notmatch 'playing=true') {
        & $appFull /play | Out-Null
        Start-Sleep -Milliseconds 500
        $state = Invoke-Probe 'transport-state'
    }
    if ($state -notmatch 'playing=true') {
        throw "Resource-study playback is not running: $state"
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

function Get-Stats([double[]]$Values) {
    if ($Values.Count -eq 0) { return $null }
    return [ordered]@{
        average = [double]($Values | Measure-Object -Average).Average
        median = Get-Percentile $Values 0.5
        p95 = Get-Percentile $Values 0.95
        maximum = [double]($Values | Measure-Object -Maximum).Maximum
    }
}

function Get-NativeSnapshot([Diagnostics.Process]$Process) {
    return [NekoRoXResourceNative]::Read($Process.Handle)
}

function Start-GpuSampler([int]$TargetId, [int]$Seconds) {
    return Start-Job -ScriptBlock {
        param($ProcessId, $Count)
        try {
            Get-Counter `
                "\GPU Engine(pid_${ProcessId}_*)\Utilization Percentage" `
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
                            $valid | Measure-Object CookedValue -Sum).Sum
                        threeD = [double](
                            $valid |
                                Where-Object Path -Match 'engtype_3D' |
                                Measure-Object CookedValue -Sum).Sum
                    }
                }
        }
        catch {
            [pscustomobject]@{ gpuSamplerError = $_.Exception.Message }
        }
    } -ArgumentList $TargetId, ([Math]::Max(2, $Seconds))
}

function Complete-GpuSampler($Job) {
    Wait-Job -Job $Job -Timeout 15 | Out-Null
    $output = @(Receive-Job -Job $Job -ErrorAction SilentlyContinue)
    Remove-Job -Job $Job -Force -ErrorAction SilentlyContinue
    $totals = New-Object 'System.Collections.Generic.List[double]'
    $threeD = New-Object 'System.Collections.Generic.List[double]'
    $errorText = ''
    foreach ($item in $output) {
        if ($item.PSObject.Properties.Name -contains 'gpuSamplerError') {
            $errorText = [string]$item.gpuSamplerError
            continue
        }
        if (-not ($item.PSObject.Properties.Name -contains 'total')) {
            continue
        }
        $totals.Add([double]$item.total)
        $threeD.Add([double]$item.threeD)
    }
    return [ordered]@{
        aggregateEnginePercent = Get-Stats $totals.ToArray()
        threeDEnginePercent = Get-Stats $threeD.ToArray()
        error = $errorText
    }
}

function Get-GpuMemory([int]$TargetId) {
    try {
        $samples = @(Get-Counter `
            "\GPU Process Memory(pid_${TargetId}_*)\*" `
            -ErrorAction Stop |
            Select-Object -ExpandProperty CounterSamples)
        $value = {
            param([string]$Pattern)
            [double](
                $samples |
                    Where-Object Path -Match $Pattern |
                    Measure-Object CookedValue -Sum).Sum / 1MB
        }
        return [ordered]@{
            dedicatedMb = & $value '\\dedicated usage$'
            sharedMb = & $value '\\shared usage$'
            committedMb = & $value '\\total committed$'
        }
    }
    catch {
        return [ordered]@{ error = $_.Exception.Message }
    }
}

function Measure-Scenario(
    [string]$Name,
    [scriptblock]$Driver = $null
) {
    $process = Get-TargetProcess
    if ($null -eq $process) { throw 'Target process is not running.' }
    $beforeNative = Get-NativeSnapshot $process
    $beforeUserMs = $process.UserProcessorTime.TotalMilliseconds
    $beforeKernelMs = $process.PrivilegedProcessorTime.TotalMilliseconds
    $beforeCpuMs = $process.TotalProcessorTime.TotalMilliseconds
    $threadBefore = @{}
    foreach ($thread in $process.Threads) {
        $threadBefore[[int]$thread.Id] =
            [double]$thread.TotalProcessorTime.TotalMilliseconds
    }

    $cpu = New-Object 'System.Collections.Generic.List[double]'
    $workingSet = New-Object 'System.Collections.Generic.List[double]'
    $privateBytes = New-Object 'System.Collections.Generic.List[double]'
    $handles = New-Object 'System.Collections.Generic.List[double]'
    $threads = New-Object 'System.Collections.Generic.List[double]'
    $gdi = New-Object 'System.Collections.Generic.List[double]'
    $user = New-Object 'System.Collections.Generic.List[double]'
    $gpuJob = Start-GpuSampler $process.Id ($SampleSeconds + 2)
    $sampleClock = [Diagnostics.Stopwatch]::StartNew()
    $scenarioClock = [Diagnostics.Stopwatch]::StartNew()
    $previousCpuMs = $beforeCpuMs
    while ($scenarioClock.Elapsed.TotalSeconds -lt $SampleSeconds) {
        if ($null -ne $Driver) { & $Driver }
        Start-Sleep -Milliseconds 500
        $current = Get-Process -Id $process.Id -ErrorAction Stop
        $native = Get-NativeSnapshot $current
        $elapsedMs = $sampleClock.Elapsed.TotalMilliseconds
        $sampleClock.Restart()
        $cpuMs = $current.TotalProcessorTime.TotalMilliseconds
        $cpu.Add(100.0 * ($cpuMs - $previousCpuMs) /
            $elapsedMs / $logicalProcessors)
        $previousCpuMs = $cpuMs
        $workingSet.Add([double]($current.WorkingSet64 / 1MB))
        $privateBytes.Add([double]($native.PrivateBytes / 1MB))
        $handles.Add([double]$current.HandleCount)
        $threads.Add([double]$current.Threads.Count)
        $gdi.Add([double]$native.GdiObjects)
        $user.Add([double]$native.UserObjects)
    }
    $scenarioClock.Stop()

    $process = Get-Process -Id $process.Id -ErrorAction Stop
    $afterNative = Get-NativeSnapshot $process
    $seconds = $scenarioClock.Elapsed.TotalSeconds
    $threadRows = @()
    foreach ($thread in $process.Threads) {
        $before = if ($threadBefore.ContainsKey([int]$thread.Id)) {
            $threadBefore[[int]$thread.Id]
        } else { 0.0 }
        $deltaMs =
            [double]$thread.TotalProcessorTime.TotalMilliseconds - $before
        if ($deltaMs -le 0.0) { continue }
        $threadRows += [pscustomobject][ordered]@{
            id = [int]$thread.Id
            cpuMs = $deltaMs
            processScaleCpuPercent =
                100.0 * $deltaMs / ($seconds * 1000.0) / $logicalProcessors
        }
    }

    return [pscustomobject][ordered]@{
        scenario = $Name
        seconds = [double]$seconds
        cpuPercent = Get-Stats $cpu.ToArray()
        userCpuPercent = 100.0 *
            ($process.UserProcessorTime.TotalMilliseconds - $beforeUserMs) /
            ($seconds * 1000.0) / $logicalProcessors
        kernelCpuPercent = 100.0 *
            ($process.PrivilegedProcessorTime.TotalMilliseconds - $beforeKernelMs) /
            ($seconds * 1000.0) / $logicalProcessors
        workingSetMb = Get-Stats $workingSet.ToArray()
        privateMb = Get-Stats $privateBytes.ToArray()
        handles = Get-Stats $handles.ToArray()
        threads = Get-Stats $threads.ToArray()
        gdiObjects = Get-Stats $gdi.ToArray()
        userObjects = Get-Stats $user.ToArray()
        ioPerSecond = [ordered]@{
            readBytes = [double](
                $afterNative.Io.ReadTransferCount -
                $beforeNative.Io.ReadTransferCount) / $seconds
            writeBytes = [double](
                $afterNative.Io.WriteTransferCount -
                $beforeNative.Io.WriteTransferCount) / $seconds
            otherBytes = [double](
                $afterNative.Io.OtherTransferCount -
                $beforeNative.Io.OtherTransferCount) / $seconds
            readOperations = [double](
                $afterNative.Io.ReadOperationCount -
                $beforeNative.Io.ReadOperationCount) / $seconds
            writeOperations = [double](
                $afterNative.Io.WriteOperationCount -
                $beforeNative.Io.WriteOperationCount) / $seconds
        }
        pageFaultsPerSecond = [double](
            $afterNative.PageFaults - $beforeNative.PageFaults) / $seconds
        gpu = Complete-GpuSampler $gpuJob
        gpuMemory = Get-GpuMemory $process.Id
        hottestThreads = @($threadRows |
            Sort-Object cpuMs -Descending |
            Select-Object -First 12)
        frameClocksAfter = Invoke-Probe 'frame-clock-state'
    }
}

$process = Get-TargetProcess
$startedByScript = $null -eq $process
if ($startedByScript) {
    Start-Process -FilePath $appFull
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
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
if ($initialTransport -notmatch 'muted=true') {
    throw 'The resource study requires an already-muted disposable profile.'
}

$results = @()
try {
    Invoke-Probe 'window-native-maximize' | Out-Null
    Invoke-Probe 'lib' | Out-Null
    Invoke-Probe 'stop' | Out-Null
    if ($ScenarioGroup -eq 'Full') {
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'stopped-foreground'
    }

    Start-BenchmarkPlayback
    Start-Sleep -Seconds $WarmupSeconds
    Assert-PlaybackRunning
    $hiddenPanels = if ($HiddenPanelClasses.Count -gt 0) {
        [NekoRoXResourceNative]::HideClasses(
            [uint32]$process.Id,
            $HiddenPanelClasses)
    } else { 0 }
    if ($hiddenPanels -ne $HiddenPanelClasses.Count) {
        throw "Resource study hid $hiddenPanels of $($HiddenPanelClasses.Count) requested panels."
    }
    if ($hiddenPanels -gt 0) { Start-Sleep -Seconds 1 }
    $results += Measure-Scenario 'playback-foreground'

    if ($ScenarioGroup -eq 'Listen') {
        Invoke-Probe 'listen' | Out-Null
        Invoke-Probe 'listen-overview' | Out-Null
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'listen-overview'

        Invoke-Probe 'listen-live' | Out-Null
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'listen-live'

        Invoke-Probe 'listen-map' | Out-Null
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'listen-map'

        Invoke-Probe 'listen-portrait' | Out-Null
        Start-Sleep -Seconds 4
        $results += Measure-Scenario 'listen-portrait'
        Invoke-Probe 'listen-close' | Out-Null
    } else {
        Invoke-Probe 'window-minimize' | Out-Null
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'playback-minimized'
    }

    if ($ScenarioGroup -eq 'Full') {
        Invoke-Probe 'window-native-maximize' | Out-Null
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'playback-restored'

        Invoke-Probe 'stop' | Out-Null
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'stopped-aggressive-scroll' {
            Invoke-Probe 'scroll-results' ([string]$ScrollEventsPerBurst) |
                Out-Null
        }

        Start-BenchmarkPlayback
        Start-Sleep -Seconds 2
        $results += Measure-Scenario 'playback-aggressive-scroll' {
            Invoke-Probe 'scroll-results' ([string]$ScrollEventsPerBurst) |
                Out-Null
        }
    }
}
finally {
    try { Invoke-Probe 'listen-close' | Out-Null } catch {}
    try { Invoke-Probe 'stop' | Out-Null } catch {}
    try { Invoke-Probe 'lib' | Out-Null } catch {}
    try { Invoke-Probe 'window-native-maximize' | Out-Null } catch {}
    if ($startedByScript) {
        try { & $appFull /exit | Out-Null } catch {}
    }
}

$manifest = [ordered]@{
    schema = 'nekorox.system-resources.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    logicalProcessors = $logicalProcessors
    displayRefreshRatesHz = @(Get-CimInstance Win32_VideoController |
        Where-Object CurrentRefreshRate |
        Select-Object -ExpandProperty CurrentRefreshRate)
    sampleSeconds = $SampleSeconds
    warmupSeconds = $WarmupSeconds
    scrollEventsPerBurst = $ScrollEventsPerBurst
    scenarioGroup = $ScenarioGroup.ToLowerInvariant()
    hiddenPanelClasses = $HiddenPanelClasses
    initialTransport = $initialTransport
    results = $results
}

$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $manifestFull)) | Out-Null
[IO.File]::WriteAllText(
    $manifestFull,
    ($manifest | ConvertTo-Json -Depth 12),
    [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 12
