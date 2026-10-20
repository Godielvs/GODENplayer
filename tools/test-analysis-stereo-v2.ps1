param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$worker = Join-Path $repo "out\$Configuration\nekorox-analysis.exe"
if (-not (Test-Path -LiteralPath $worker -PathType Leaf)) {
    throw "Analysis worker is missing: $worker"
}

function New-StereoFixture([string]$Path) {
    $rate = 12000
    $frames = $rate * 2
    $channels = 2
    $dataBytes = $frames * $channels * 2
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create)
    try {
        $writer = [IO.BinaryWriter]::new($stream)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([uint32]16)
        $writer.Write([uint16]1)
        $writer.Write([uint16]$channels)
        $writer.Write([uint32]$rate)
        $writer.Write([uint32]($rate * $channels * 2))
        $writer.Write([uint16]($channels * 2))
        $writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([uint32]$dataBytes)
        for ($frame = 0; $frame -lt $frames; ++$frame) {
            $time = $frame / [double]$rate
            $left = 0.62 * [Math]::Sin(2.0 * [Math]::PI * 220.0 * $time)
            $right = 0.44 * [Math]::Sin(
                2.0 * [Math]::PI * 220.0 * $time + 0.65) +
                0.16 * [Math]::Sin(2.0 * [Math]::PI * 330.0 * $time)
            $clampedLeft = [Math]::Min(1.0, [Math]::Max(-1.0, $left))
            $clampedRight = [Math]::Min(1.0, [Math]::Max(-1.0, $right))
            $writer.Write([int16][Math]::Round($clampedLeft * 32767.0))
            $writer.Write([int16][Math]::Round($clampedRight * 32767.0))
        }
        $writer.Flush()
    } finally {
        $stream.Dispose()
    }
}

function Invoke-Analysis([string]$Path) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $worker
    # ProcessStartInfo.ArgumentList is unavailable in Windows PowerShell 5.1.
    # The fixture path is generated locally and quoted for the Win32 parser.
    $start.Arguments = 'analyze 9247 "' + $Path.Replace('"', '\"') + '"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'Could not start analysis worker.' }
    $errorTask = $process.StandardError.ReadToEndAsync()
    $output = [IO.MemoryStream]::new()
    $process.StandardOutput.BaseStream.CopyTo($output)
    $process.WaitForExit()
    $errorText = $errorTask.Result
    if ($process.ExitCode -ne 0) {
        throw "Analysis worker failed ($($process.ExitCode)): $errorText"
    }
    $output.Position = 0
    $reader = [IO.BinaryReader]::new($output)
    $complete = $null
    while ($output.Position -lt $output.Length) {
        if ($output.Length - $output.Position -lt 20) {
            throw 'Analysis worker returned a truncated frame header.'
        }
        $magic = $reader.ReadUInt32()
        $version = $reader.ReadUInt16()
        $kind = $reader.ReadUInt16()
        $bytes = $reader.ReadUInt32()
        $request = $reader.ReadUInt64()
        if ($magic -ne 0x4F524B4E -or $version -ne 1 -or $request -ne 9247) {
            throw 'Analysis protocol V1 header changed unexpectedly.'
        }
        if ($bytes -gt $output.Length - $output.Position) {
            throw 'Analysis worker returned a truncated frame payload.'
        }
        $payload = $reader.ReadBytes($bytes)
        if ($kind -eq 7) {
            $complete = [Text.Encoding]::UTF8.GetString($payload) |
                ConvertFrom-Json
        }
    }
    if ($null -eq $complete) {
        throw 'Analysis worker did not publish a complete result.'
    }
    return $complete
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) (
    'goden-analysis-v2-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    $fixture = Join-Path $scratch 'stereo.wav'
    New-StereoFixture $fixture
    $result = Invoke-Analysis $fixture
    if ($result.godenV2.version -ne 2) {
        throw 'TrackAnalysisSummaryV2 version is missing.'
    }
    $stereo = $result.godenV2.stereo
    foreach ($field in @(
            'correlation', 'midEnergy', 'sideEnergy',
            'outOfPhaseFraction')) {
        if ($null -eq $stereo.$field) {
            throw "Stereo V2 field is missing: $field"
        }
    }
    if ($stereo.correlation -lt -1.0 -or $stereo.correlation -gt 1.0 -or
        $stereo.midEnergy -lt 0.0 -or $stereo.sideEnergy -lt 0.0 -or
        $stereo.outOfPhaseFraction -lt 0.0 -or
        $stereo.outOfPhaseFraction -gt 1.0) {
        throw 'Stereo V2 metrics left their fixed physical ranges.'
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force `
        -ErrorAction SilentlyContinue
}

Write-Host 'Goden analysis stereo V2: PASS'
