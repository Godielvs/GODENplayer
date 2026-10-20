param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$DevRoot = '',
    [string]$EvidenceRoot = '',
    [switch]$LeaveOpen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $DevRoot) {
    $DevRoot = Join-Path $repo 'artifacts\interaction-portable'
}
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $repo 'artifacts\visual-release'
}
$app = Join-Path ([IO.Path]::GetFullPath($DevRoot)) 'app\foobar2000.exe'
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
foreach ($required in @($app, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Release capture input is missing: $required"
    }
}
New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null

function Get-AppProcess {
    $match = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($match.Count -gt 1) { throw 'Multiple capture targets are running.' }
    if ($match.Count -eq 0) { return $null }
    try { return Get-Process -Id $match[0].ProcessId -ErrorAction Stop }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] { return $null }
}

function Wait-App([bool]$Present, [int]$TimeoutMs = 15000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-AppProcess
        if ($Present -and $null -ne $process -and
                $process.MainWindowHandle -ne 0) { return $process }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Capture target did not reach its expected process state."
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = & $probe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $($output -join "`n")"
    }
    return ($output | ForEach-Object { $_.ToString() }) -join "`n"
}

function Capture-NativeMenu([string]$Path) {
    $failures = New-Object 'System.Collections.Generic.List[string]'
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Invoke-Probe 'window-native-maximize' | Out-Null
            Start-Sleep -Milliseconds 120
            Invoke-Probe 'menu-capture' $Path | Out-Null
            if ((Test-Path -LiteralPath $Path) -and
                    (Get-Item -LiteralPath $Path).Length -ge 1024) {
                return
            }
            $failures.Add("attempt $attempt produced no usable bitmap")
        }
        catch {
            $failures.Add("attempt ${attempt}: $($_.Exception.Message)")
        }
        Start-Sleep -Milliseconds (120 * $attempt)
    }
    throw "Native menu capture failed after 3 attempts: $($failures -join '; ')"
}

function Capture-Main([string]$Name) {
    $path = Join-Path $EvidenceRoot "$Name.bmp"
    Invoke-Probe 'window-capture' $path | Out-Null
    return $path
}

function Capture-Listen([string]$Name) {
    $path = Join-Path $EvidenceRoot "$Name.bmp"
    Invoke-Probe 'listen-window-capture' $path | Out-Null
    return $path
}

function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return [BitConverter]::ToString(
                $sha.ComputeHash($stream)).Replace('-', '')
        }
        finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

function Measure-BmpCapture([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x4D42) {
            throw "Release capture is not a BMP: $Path"
        }
        $stream.Position = 10
        $pixelOffset = $reader.ReadUInt32()
        $stream.Position = 18
        $width = [Math]::Abs($reader.ReadInt32())
        $height = [Math]::Abs($reader.ReadInt32())
        [void]$reader.ReadUInt16()
        $bitsPerPixel = $reader.ReadUInt16()
        $compression = $reader.ReadUInt32()
        if ($width -lt 100 -or $height -lt 100 -or
                $bitsPerPixel -notin @(24, 32) -or
                $compression -notin @(0, 3)) {
            throw "Unsupported or implausible release bitmap: $Path"
        }
        $bytesPerPixel = [int]($bitsPerPixel / 8)
        $stride = [int](([Math]::Floor(
            (($width * $bitsPerPixel) + 31) / 32)) * 4)
        $colors = [Collections.Generic.HashSet[uint32]]::new()
        $minimumLuma = 255
        $maximumLuma = 0
        for ($sampleY = 0; $sampleY -lt 24; ++$sampleY) {
            $y = [Math]::Min($height - 1,
                [int][Math]::Floor($sampleY * ($height - 1) / 23.0))
            for ($sampleX = 0; $sampleX -lt 32; ++$sampleX) {
                $x = [Math]::Min($width - 1,
                    [int][Math]::Floor($sampleX * ($width - 1) / 31.0))
                $stream.Position = $pixelOffset + [int64]$y * $stride +
                    [int64]$x * $bytesPerPixel
                $blue = $reader.ReadByte()
                $green = $reader.ReadByte()
                $red = $reader.ReadByte()
                [void]$colors.Add([uint32](
                    $blue -bor ($green -shl 8) -bor ($red -shl 16)))
                $luma = [int][Math]::Round(
                    0.0722 * $blue + 0.7152 * $green + 0.2126 * $red)
                $minimumLuma = [Math]::Min($minimumLuma, $luma)
                $maximumLuma = [Math]::Max($maximumLuma, $luma)
            }
        }
        if ($colors.Count -lt 8 -or $maximumLuma - $minimumLuma -lt 12) {
            throw "Release capture is blank or effectively uniform: $Path"
        }
        return [ordered]@{
            width = $width
            height = $height
            bitsPerPixel = $bitsPerPixel
            sampledColors = $colors.Count
            sampledLumaRange = $maximumLuma - $minimumLuma
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$captures = New-Object 'System.Collections.Generic.List[string]'
try {
    if ($null -eq (Get-AppProcess)) { Start-Process -FilePath $app }
    Wait-App $true | Out-Null
    Invoke-Probe 'mute-on' | Out-Null
    Invoke-Probe 'window-native-maximize' | Out-Null
    Invoke-Probe 'lib' | Out-Null
    Invoke-Probe 'play-first-album' | Out-Null
    Start-Sleep -Milliseconds 1400

    foreach ($tab in @('lib', 'inf', 'bio', 'lyr', 'art')) {
        Invoke-Probe $tab | Out-Null
        Start-Sleep -Milliseconds 250
        $captures.Add((Capture-Main "main-1920-$tab"))
    }
    $captures.Add((Join-Path $EvidenceRoot 'main-native-menu.bmp'))
    Capture-NativeMenu $captures[$captures.Count - 1]

    Invoke-Probe 'lib' | Out-Null
    foreach ($size in @('1280x720', '960x640', '900x640')) {
        Invoke-Probe 'window-resize' $size | Out-Null
        Start-Sleep -Milliseconds 250
        $captures.Add((Capture-Main "main-$size-lib"))
    }
    Invoke-Probe 'window-native-maximize' | Out-Null

    Invoke-Probe 'listen' | Out-Null
    Start-Sleep -Milliseconds 2200
    foreach ($space in @(
            @{ action = 'listen-overview'; name = 'listen-overview' },
            @{ action = 'listen-live'; name = 'listen-live' },
            @{ action = 'listen-map'; name = 'listen-music-map' },
            @{ action = 'listen-portrait'; name = 'listen-signal-portrait' })) {
        Invoke-Probe $space.action | Out-Null
        Start-Sleep -Milliseconds 700
        $captures.Add((Capture-Listen $space.name))
    }
}
finally {
    try { Invoke-Probe 'listen-close' | Out-Null } catch {}
    if (-not $LeaveOpen) {
        try { & $app /exit | Out-Null } catch {}
        try { Wait-App $false | Out-Null } catch {}
    }
}

$files = @($captures | ForEach-Object {
    $item = Get-Item -LiteralPath $_
    $measurement = Measure-BmpCapture $item.FullName
    [ordered]@{
        path = $item.FullName
        bytes = $item.Length
        sha256 = Get-Sha256 $item.FullName
        visual = $measurement
    }
})
if ($files.Count -ne 13) {
    throw "Expected 13 release captures, found $($files.Count)."
}
foreach ($file in $files) {
    if ($file.bytes -lt 1024) {
        throw "Release capture is unexpectedly small: $($file.path)"
    }
}
$manifest = [ordered]@{
    schema = 'nekorox.visual-release.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    passed = $true
    app = $app
    playback = 'library track, muted output, realtime visuals active'
    captures = $files
}
$manifestPath = Join-Path $EvidenceRoot 'manifest.json'
$manifest | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "NekoRoX release captures: $($files.Count)"
Write-Host "Manifest: $manifestPath"
