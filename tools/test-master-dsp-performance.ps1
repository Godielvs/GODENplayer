param(
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [ValidateRange(1, 50)][int]$Runs = 10,
    [string]$ManifestPath = '',
    [string]$CandidateExecutable = '',
    [string]$BaselineExecutable = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $CandidateExecutable) {
    $CandidateExecutable = Join-Path $repo "out\$Configuration\master_dsp_integration_tests.exe"
}
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo ('artifacts\master-dsp-performance-' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N') + '.json')
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$logPath = [IO.Path]::ChangeExtension($manifestFull, '.log')
foreach ($path in @($manifestFull, $logPath)) {
    if (Test-Path -LiteralPath $path) {throw 'Refusing to overwrite an existing measurement or log.'}
}
$executables = [ordered]@{candidate=[IO.Path]::GetFullPath($CandidateExecutable)}
if ($BaselineExecutable) {$executables.baseline=[IO.Path]::GetFullPath($BaselineExecutable)}
$identities = [ordered]@{}
foreach ($role in $executables.Keys) {
    $path = $executables[$role]
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {throw "Missing $role DSP test executable."}
    $identities[$role] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
# Tiny OS counters sampled outside the measured executable. No polling, UI,
# priority/affinity changes, background benchmark, or performance-gate retry.
$contextAvailable = $true
try {
    if (-not ('GodenDspSystemContext' -as [type])) {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class GodenDspSystemContext {
    [StructLayout(LayoutKind.Sequential)] public struct Memory {
        public uint Length, Load;
        public ulong Total, Available, TotalPageFile, AvailablePageFile, TotalVirtual, AvailableVirtual, Extended;
    }
    [DllImport("kernel32.dll")] static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref Memory memory);
    public static ulong[] Read() {
        long idle, kernel, user;
        var memory = new Memory(); memory.Length = (uint)Marshal.SizeOf(typeof(Memory));
        if (!GetSystemTimes(out idle, out kernel, out user) || !GlobalMemoryStatusEx(ref memory))
            throw new InvalidOperationException("System context unavailable");
        return new ulong[] {(ulong)idle, (ulong)kernel, (ulong)user, memory.Total, memory.Available, memory.Load};
    }
}
"@
    }
} catch {$contextAvailable=$false}
function Get-Context {
    if (-not $contextAvailable) {return $null}
    try {return [GodenDspSystemContext]::Read()} catch {return $null}
}
function Get-Timing([string]$Text, [string]$Label) {
    $match=[regex]::Match($Text, '(?m)^'+[regex]::Escape($Label)+': ([0-9]+(?:\.[0-9]+)?)(?:[ \t]+ms\b|[ \t]*\r?$)')
    $number=0.0
    if ($match.Success -and [double]::TryParse($match.Groups[1].Value,
            [Globalization.NumberStyles]::AllowDecimalPoint,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and
            -not [double]::IsInfinity($number) -and -not [double]::IsNaN($number)) {return $number}
    return $null
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifestFull) | Out-Null
$samples = @()
$logs = [Collections.Generic.List[string]]::new()
$startedUtc = [DateTime]::UtcNow.ToString('o')
for ($round = 1; $round -le $Runs; ++$round) {
    # Alternate AB/BA to reduce order bias; both versions receive identical runs.
    $order = if (-not $BaselineExecutable) {@('candidate')} elseif ($round % 2) {
        @('baseline','candidate')
    } else {@('candidate','baseline')}
    foreach ($role in $order) {
        $before = Get-Context
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $exitCode = $null
        $text = ''
        $invocationError = $false
        try {
            $global:LASTEXITCODE = 0
            $output = & $executables[$role] 2>&1
            $exitCode = $LASTEXITCODE
            $text = $output -join "`n"
        } catch {
            $invocationError = $true
            $text = $_.ToString()
        }
        $watch.Stop()
        $after = Get-Context
        $logs.Add("round=$round role=$role exit=$exitCode`n$text`n")
        $common = Get-Timing $text 'Common audiophile chain p99'
        $creative = Get-Timing $text 'Creative 12-module chain p99'
        $context = $null
        if ($null -ne $before -and $null -ne $after) {
            $total = ([double]$after[1] - $before[1]) + ([double]$after[2] - $before[2])
            $idle = [double]$after[0] - $before[0]
            $cpu = if ($total -gt 0 -and $idle -ge 0 -and $idle -le $total) {100.0 * (1.0 - $idle / $total)} else {$null}
            $context = [ordered]@{
                cpuLoadPercentDuringInvocation = $cpu
                availablePhysicalMemoryBytesBefore = $before[4]
                availablePhysicalMemoryBytesAfter = $after[4]
                totalPhysicalMemoryBytes = $after[3]
                memoryLoadPercentBefore = $before[5]
                memoryLoadPercentAfter = $after[5]
            }
        }
        $parsed = $null -ne $common -and $null -ne $creative
        $samples += [pscustomobject]@{
            round=$round; role=$role; order=$samples.Count + 1
            exitCode=$exitCode; invocationError=$invocationError; parsed=$parsed
            commonP99Ms=$common; creativeP99Ms=$creative
            passed=($parsed -and -not $invocationError -and $exitCode -eq 0 -and $common -le 0.40 -and $creative -le 2.40)
            elapsedWallMilliseconds=$watch.Elapsed.TotalMilliseconds; systemContext=$context
        }
    }
}
$identityUnchanged=$true
foreach ($role in $executables.Keys) {
    if ((Get-FileHash -LiteralPath $executables[$role] -Algorithm SHA256).Hash.ToLowerInvariant() -ne $identities[$role]) {
        $identityUnchanged=$false
    }
}
$summary=[ordered]@{}
foreach ($role in $executables.Keys) {
    $values=@($samples | Where-Object {$_.role -eq $role -and $_.parsed})
    $summary[$role]=[ordered]@{
        samples=$values.Count
        commonMeanP99Ms=if ($values.Count) {($values.commonP99Ms | Measure-Object -Average).Average} else {$null}
        commonMaxP99Ms=if ($values.Count) {($values.commonP99Ms | Measure-Object -Maximum).Maximum} else {$null}
        creativeMeanP99Ms=if ($values.Count) {($values.creativeP99Ms | Measure-Object -Average).Average} else {$null}
        creativeMaxP99Ms=if ($values.Count) {($values.creativeP99Ms | Measure-Object -Maximum).Maximum} else {$null}
    }
}
$pairedDelta = $null
if ($BaselineExecutable -and $summary.baseline.samples -eq $Runs -and $summary.candidate.samples -eq $Runs) {
    $pairedDelta=[ordered]@{
        commonMeanP99Ms=$summary.candidate.commonMeanP99Ms - $summary.baseline.commonMeanP99Ms
        creativeMeanP99Ms=$summary.candidate.creativeMeanP99Ms - $summary.baseline.creativeMeanP99Ms
    }
}
$logs -join "`n" | Set-Content -LiteralPath $logPath -Encoding UTF8
$passed=$identityUnchanged -and @($samples | Where-Object {-not $_.passed}).Count -eq 0
$result = [ordered]@{
    schema='goden.master-dsp-performance.v2'; createdUtc=$startedUtc; completedUtc=[DateTime]::UtcNow.ToString('o')
    passed=$passed; runsPerVersion=$Runs; executableSha256=$identities; executableIdentityUnchanged=$identityUnchanged
    orderPolicy='alternating AB/BA; no retries'; gates=[ordered]@{commonP99Ms=0.40; creativeP99Ms=2.40}
    systemContextSource='GetSystemTimes (calling processor group) / GlobalMemoryStatusEx; sampled around each invocation'
    logicalProcessorsInProcess=[Environment]::ProcessorCount
    measurementConditionsControlled=$false; wholePlayerPerformanceMeasured=$false
    regressionConclusion='not established by this runner; compare matched binaries/audio/settings and inspect contemporaneous context'
    summary=$summary; pairedMeanDeltaMilliseconds=$pairedDelta; samples=$samples
    logSha256=(Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestFull -Encoding UTF8
Write-Host "DSP measurement saved ($($samples.Count) invocations). Manifest: $manifestFull"
if (-not $passed) {throw 'DSP qualification failed; all planned samples and failures were retained. No retry was performed.'}
Write-Host 'GODENplayer Master DSP timing gates: PASS (not whole-player performance approval).'
