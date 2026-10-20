param(
    [switch]$CaptureHangDump,
    [Parameter(Mandatory = $true)]
    [int]$ProcessId,
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 30,
    [ValidateRange(25, 50)]
    [int]$PollMilliseconds = 25,
    [ValidateRange(100, 5000)]
    [int]$HeartbeatLimitMilliseconds = 250,
    [ValidateRange(1, 8192)]
    [int]$LatestEventCount = 256,
    [string]$UiProbeExecutable = '',
    [string]$AppExecutable = '',
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $OutputPath) {
    $OutputPath = Join-Path $repo (
        'artifacts\performance-ring-' + $ProcessId + '-' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.json')
}
$outputFull = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $outputFull)) | Out-Null
if (($UiProbeExecutable -and -not $AppExecutable) -or
        ($AppExecutable -and -not $UiProbeExecutable)) {
    throw 'UiProbeExecutable and AppExecutable must be supplied together.'
}
if ($UiProbeExecutable -and
        (-not (Test-Path -LiteralPath $UiProbeExecutable) -or
         -not (Test-Path -LiteralPath $AppExecutable))) {
    throw 'The UI probe or candidate executable does not exist.'
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public sealed class GodenPerformanceRingReader : IDisposable {
    const uint FileMapRead = 0x0004;
    const int HeaderBytes = 64;
    const int EventBytes = 64;
    const uint ExpectedMagic = 0x32524550;
    IntPtr mapping;
    IntPtr view;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr MapViewOfFile(IntPtr mapping, uint access,
        uint offsetHigh, uint offsetLow, UIntPtr bytes);
    [DllImport("kernel32.dll")]
    static extern bool UnmapViewOfFile(IntPtr address);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);

    public sealed class Event {
        public ulong sequence { get; set; }
        public ulong qpc { get; set; }
        public uint threadId { get; set; }
        public ushort kind { get; set; }
        public ushort surface { get; set; }
        public ulong argument0 { get; set; }
        public ulong argument1 { get; set; }
        public ulong argument2 { get; set; }
        public ulong argument3 { get; set; }
        public ulong durationQpc { get; set; }
    }

    public GodenPerformanceRingReader(int processId) {
        string name = "Local\\Goden.PerformanceEventRingV2." + processId;
        mapping = OpenFileMapping(FileMapRead, false, name);
        if (mapping == IntPtr.Zero)
            throw new InvalidOperationException("Performance ring is unavailable");
        view = MapViewOfFile(mapping, FileMapRead, 0, 0, UIntPtr.Zero);
        if (view == IntPtr.Zero) {
            CloseHandle(mapping);
            mapping = IntPtr.Zero;
            throw new InvalidOperationException("Performance ring mapping failed");
        }
        if (ReadUInt32(0) != ExpectedMagic || ReadUInt32(4) != 2 ||
                ReadUInt32(8) != EventBytes || ReadUInt32(12) != 8192)
            throw new InvalidOperationException("Performance ring ABI mismatch");
    }

    uint ReadUInt32(int offset) => unchecked((uint)Marshal.ReadInt32(view, offset));
    ulong ReadUInt64(int offset) => unchecked((ulong)Marshal.ReadInt64(view, offset));
    ulong ReadUInt64At(IntPtr address, int offset) =>
        unchecked((ulong)Marshal.ReadInt64(address, offset));

    public ulong Frequency => ReadUInt64(24);
    public ulong LatestSequence => ReadUInt64(32);
    public ulong HeartbeatQpc => ReadUInt64(40);
    public ulong StartupEpoch => ReadUInt64(48);

    public Event[] ReadLatest(int maximum) {
        ulong latest = LatestSequence;
        ulong first = latest > (ulong)maximum ? latest - (ulong)maximum + 1 : 1;
        var events = new List<Event>(maximum);
        for (ulong sequence = first; sequence <= latest && sequence != 0; ++sequence) {
            long slot = (long)(sequence % 8192UL);
            IntPtr record = IntPtr.Add(view, HeaderBytes + checked((int)slot * EventBytes));
            ulong before = ReadUInt64At(record, 0);
            if (before != sequence) continue;
            var item = new Event {
                sequence = sequence,
                qpc = ReadUInt64At(record, 8),
                threadId = unchecked((uint)Marshal.ReadInt32(record, 16)),
                kind = unchecked((ushort)Marshal.ReadInt16(record, 20)),
                surface = unchecked((ushort)Marshal.ReadInt16(record, 22)),
                argument0 = ReadUInt64At(record, 24),
                argument1 = ReadUInt64At(record, 32),
                argument2 = ReadUInt64At(record, 40),
                argument3 = ReadUInt64At(record, 48),
                durationQpc = ReadUInt64At(record, 56),
            };
            if (ReadUInt64At(record, 0) == before) events.Add(item);
        }
        return events.ToArray();
    }

    public void Dispose() {
        if (view != IntPtr.Zero) { UnmapViewOfFile(view); view = IntPtr.Zero; }
        if (mapping != IntPtr.Zero) { CloseHandle(mapping); mapping = IntPtr.Zero; }
    }
}
'@

$reader = $null
$openDeadline = [Diagnostics.Stopwatch]::StartNew()
while ($null -eq $reader -and $openDeadline.ElapsedMilliseconds -lt 10000) {
    try { $reader = [GodenPerformanceRingReader]::new($ProcessId) }
    catch [InvalidOperationException] { Start-Sleep -Milliseconds 50 }
}
if ($null -eq $reader) {
    throw "QA performance ring for PID $ProcessId did not appear. " +
        'Start the disposable app with NEKOROX_QA_HOOKS=1 and NEKOROX_QA_PROFILE=1.'
}

$gaps = [Collections.Generic.List[object]]::new()
$maximumGap = 0.0
$samples = 0
$lastObservedSequence = $reader.LatestSequence
$lostEventCount = [uint64]0
$spanStats = @{}
$stallEvents = @()
$stackProbeOutput = ''
$hangDumpStarted = $false
$timer = [Diagnostics.Stopwatch]::StartNew()
try {
    while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
        $heartbeat = $reader.HeartbeatQpc
        $frequency = [Math]::Max(1.0, [double]$reader.Frequency)
        $now = [Diagnostics.Stopwatch]::GetTimestamp()
        $gap = if ($heartbeat -eq 0) {
            [double]::PositiveInfinity
        } else {
            ([double]$now - [double]$heartbeat) * 1000.0 / $frequency
        }
        $maximumGap = [Math]::Max($maximumGap, $gap)
        if ($CaptureHangDump -and -not $hangDumpStarted -and $heartbeat -ne 0 -and
                $gap -ge 3000 -and $UiProbeExecutable -and $AppExecutable) {
            $hangDumpStarted = $true
            $dumpPath = [IO.Path]::ChangeExtension($outputFull, '.hang.dmp')
            $dumpLog = [IO.Path]::ChangeExtension($outputFull, '.hang.txt')
            # The probe verifies the exact candidate path and reads the target
            # externally; it never sends a synchronous request to the stalled UI.
            Start-Process -FilePath $UiProbeExecutable -ArgumentList @(
                ('"' + [IO.Path]::GetFullPath($AppExecutable) + '"'),
                'capture-hang-dump', ('"' + $dumpPath + '"'), '--background') `
                -WindowStyle Hidden -RedirectStandardOutput $dumpLog
            $componentRoot = Join-Path (Split-Path -Parent $AppExecutable) `
                'profile\user-components-x64\foo_nekorox'
            foreach ($name in @('foo_nekorox.dll','foo_nekorox.pdb')) {
                $source = Join-Path $componentRoot $name
                if (Test-Path -LiteralPath $source) {
                    Copy-Item -LiteralPath $source -Destination `
                        (Join-Path (Split-Path -Parent $outputFull) $name) -Force
                }
            }
        }
        if ($gap -gt $HeartbeatLimitMilliseconds) {
            if ($stallEvents.Count -eq 0) {
                $stallEvents = @($reader.ReadLatest(128) | ForEach-Object {
                    [ordered]@{
                        sequence = $_.sequence
                        qpc = $_.qpc
                        threadId = $_.threadId
                        kind = $_.kind
                        surface = $_.surface
                        argument0 = $_.argument0
                        argument1 = $_.argument1
                        durationQpc = $_.durationQpc
                    }
                })
                if ($UiProbeExecutable) {
                    $stackProbeOutput = [IO.Path]::ChangeExtension(
                        $outputFull, '.stacks.txt')
                    Start-Process -FilePath $UiProbeExecutable -ArgumentList @(
                        ('"' + [IO.Path]::GetFullPath($AppExecutable) + '"'),
                        'sample-ui-stacks', '2') -WindowStyle Hidden `
                        -RedirectStandardOutput $stackProbeOutput
                }
            }
            $gaps.Add([ordered]@{
                observedUtc = [DateTime]::UtcNow.ToString('o')
                gapMilliseconds = $gap
                latestSequence = $reader.LatestSequence
            })
        }
        $latest = $reader.LatestSequence
        if ($latest -gt $lastObservedSequence) {
            $available = $latest - $lastObservedSequence
            $take = [int][Math]::Min([double]$available, 1024.0)
            if ($available -gt $take) {
                $lostEventCount += $available - $take
            }
            foreach ($item in $reader.ReadLatest($take)) {
                if ($item.sequence -le $lastObservedSequence -or
                        $item.kind -ne 17) { continue }
                $key = "$($item.surface):$($item.argument0)"
                if (-not $spanStats.ContainsKey($key)) {
                    $spanStats[$key] = [ordered]@{
                        surface = [int]$item.surface
                        phase = [uint64]$item.argument0
                        count = [uint64]0
                        totalMilliseconds = 0.0
                        maximumMilliseconds = 0.0
                        over16ms = [uint64]0
                        over50ms = [uint64]0
                        over150ms = [uint64]0
                        largestDetail = [uint64]0
                    }
                }
                $row = $spanStats[$key]
                $milliseconds = [double]$item.durationQpc * 1000.0 /
                    $frequency
                $row['count'] = [uint64]$row['count'] + 1U
                $row['totalMilliseconds'] += $milliseconds
                $row['maximumMilliseconds'] = [Math]::Max(
                    $row['maximumMilliseconds'], $milliseconds)
                if ($milliseconds -gt 16.0) {
                    $row['over16ms'] = [uint64]$row['over16ms'] + 1U
                }
                if ($milliseconds -gt 50.0) {
                    $row['over50ms'] = [uint64]$row['over50ms'] + 1U
                }
                if ($milliseconds -gt 150.0) {
                    $row['over150ms'] = [uint64]$row['over150ms'] + 1U
                }
                $row['largestDetail'] = [Math]::Max(
                    $row['largestDetail'], $item.argument1)
            }
            $lastObservedSequence = $latest
        }
        ++$samples
        Start-Sleep -Milliseconds $PollMilliseconds
    }

    $kindNames = @('heartbeat','input','callback','post','wake','consume',
        'paint','endDraw','present','seek','pressure','failure','duration',
        'inputIncorporated','frameAttempt','frameResult','spanStart','spanEnd',
        'cacheReason','collectionMaterialization','readinessNotReady',
        'waitArmed','readySignaled','acceptedVisible','displayed')
    $surfaceNames = @('transport','waveSeek','headerMonitor','workspace',
        'spectrum','listen','listenCrt','daw','artwork','album','albumAnalysis','graphics','display')
    $events = @($reader.ReadLatest($LatestEventCount) | ForEach-Object {
        $kindName = if ($_.kind -lt $kindNames.Count) {
            $kindNames[$_.kind]
        } else {
            "unknown-$($_.kind)"
        }
        $surfaceName = if ($_.surface -lt $surfaceNames.Count) {
            $surfaceNames[$_.surface]
        } else {
            "unknown-$($_.surface)"
        }
        [ordered]@{
            sequence = $_.sequence
            qpc = $_.qpc
            threadId = $_.threadId
            kind = $kindName
            surface = $surfaceName
            argument0 = $_.argument0
            argument1 = $_.argument1
            argument2 = $_.argument2
            argument3 = $_.argument3
            durationQpc = $_.durationQpc
        }
    })
    $manifest = [ordered]@{
        schema = 'goden.performance-event-ring.v2'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        processId = $ProcessId
        startupEpoch = $reader.StartupEpoch
        durationSeconds = $timer.Elapsed.TotalSeconds
        pollMilliseconds = $PollMilliseconds
        heartbeatLimitMilliseconds = $HeartbeatLimitMilliseconds
        sampleCount = $samples
        maximumHeartbeatGapMilliseconds = $maximumGap
        lostEventCount = $lostEventCount
        stackProbeOutput = $stackProbeOutput
        hangDumpRequested = $hangDumpStarted
        firstStallEvents = $stallEvents
        spanStats = @($spanStats.Values | ForEach-Object {
            [pscustomobject]$_
        } | Sort-Object surface, phase)
        passed = $gaps.Count -eq 0
        gaps = @($gaps)
        events = $events
    }
    $manifest | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath $outputFull -Encoding UTF8
}
finally {
    $reader.Dispose()
}

if ($gaps.Count -ne 0) {
    throw "UI heartbeat exceeded ${HeartbeatLimitMilliseconds}ms. Evidence: $outputFull"
}
Write-Host "Performance event ring: PASS ($samples samples, max $([Math]::Round($maximumGap, 2)) ms)"
Write-Host "Evidence: $outputFull"
