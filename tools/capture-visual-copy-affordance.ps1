param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$DevRoot = '',
    [string]$EvidenceRoot = '',
    [ValidateSet(96, 120, 144, 192)]
    [int]$Dpi = 96,
    [switch]$LeaveOpen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $DevRoot) { $DevRoot = Join-Path $repo 'artifacts\interaction-portable' }
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $repo 'artifacts\visual-copy-affordance'
}
$app = Join-Path ([IO.Path]::GetFullPath($DevRoot)) 'app\foobar2000.exe'
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
foreach ($required in @($app, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Visual affordance input is missing: $required"
    }
}
& (Join-Path $PSScriptRoot 'test-copy-lint.ps1') -RepoRoot $repo
[IO.Directory]::CreateDirectory($EvidenceRoot) | Out-Null

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = & $probe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "UI probe '$Action' failed: $($output -join "`n")"
    }
    return ($output | ForEach-Object ToString) -join "`n"
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

Add-Type -AssemblyName System.Drawing

function Test-VisualCapture([string]$Path, [int]$ExpectedWidthDip, [int]$ExpectedHeightDip) {
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -lt 1024) { throw "Invalid capture: $Path" }

    $bitmap = [Drawing.Bitmap]::new($item.FullName)
    try {
        if ($bitmap.Width -lt 64 -or $bitmap.Height -lt 64) {
            throw "Capture dimensions are invalid: $($bitmap.Width)x$($bitmap.Height)"
        }
        $expectedWidth = [Math]::Round($ExpectedWidthDip * $Dpi / 96.0)
        $expectedHeight = [Math]::Round($ExpectedHeightDip * $Dpi / 96.0)
        $widthTolerance = [Math]::Max(32, [Math]::Round($expectedWidth * 0.08))
        $heightTolerance = [Math]::Max(32, [Math]::Round($expectedHeight * 0.08))
        if ([Math]::Abs($bitmap.Width - $expectedWidth) -gt $widthTolerance -or
                [Math]::Abs($bitmap.Height - $expectedHeight) -gt $heightTolerance) {
            throw "Capture dimensions $($bitmap.Width)x$($bitmap.Height) do not match the ${ExpectedWidthDip}x${ExpectedHeightDip} DIP fixture at ${Dpi} DPI."
        }

        $minimum = 255
        $maximum = 0
        $colors = [Collections.Generic.HashSet[int]]::new()
        for ($sampleY = 0; $sampleY -lt 17; $sampleY++) {
            $y = [Math]::Min($bitmap.Height - 1,
                [Math]::Round(($bitmap.Height - 1) * $sampleY / 16.0))
            for ($sampleX = 0; $sampleX -lt 17; $sampleX++) {
                $x = [Math]::Min($bitmap.Width - 1,
                    [Math]::Round(($bitmap.Width - 1) * $sampleX / 16.0))
                $color = $bitmap.GetPixel($x, $y)
                $luma = [int][Math]::Round(
                    0.2126 * $color.R + 0.7152 * $color.G + 0.0722 * $color.B)
                $minimum = [Math]::Min($minimum, $luma)
                $maximum = [Math]::Max($maximum, $luma)
                [void]$colors.Add($color.ToArgb())
            }
        }
        if ($colors.Count -lt 3 -or ($maximum - $minimum) -lt 6) {
            throw "Capture is blank, uniform, or has insufficient contrast: $Path"
        }
        return [ordered]@{
            item = $item
            widthPixels = $bitmap.Width
            heightPixels = $bitmap.Height
            sampledColors = $colors.Count
            sampledLumaRange = $maximum - $minimum
        }
    }
    finally {
        $bitmap.Dispose()
    }
}

function Add-CaptureRecord(
    [string]$Path,
    [string]$Surface,
    [string]$State,
    [string]$Interaction,
    [int]$WidthDip,
    [int]$HeightDip,
    [string]$Fixture,
    [string]$Action,
    [string]$GoldenId
) {
    $validated = Test-VisualCapture $Path $WidthDip $HeightDip
    $item = $validated.item
    $captures.Add([ordered]@{
        dpi = $Dpi
        surface = $Surface
        state = $State
        interaction = $Interaction
        widthDip = $WidthDip
        heightDip = $HeightDip
        widthPixels = $validated.widthPixels
        heightPixels = $validated.heightPixels
        sampledColors = $validated.sampledColors
        sampledLumaRange = $validated.sampledLumaRange
        path = $item.FullName
        bytes = $item.Length
        sha256 = Get-Sha256 $item.FullName
        fixture = $Fixture
        action = $Action
        goldenId = $GoldenId
        pixelTolerance = 6
        maximumChangedPercent = 0.5
        maximumShiftDip = 1
    })
}

$captures = New-Object 'System.Collections.Generic.List[object]'
$process = $null
try {
    $process = Start-Process -FilePath $app -PassThru
    for ($attempt = 0; $attempt -lt 120; $attempt++) {
        $process.Refresh()
        if ($process.HasExited) { throw 'Visual affordance target exited during startup.' }
        if ($process.MainWindowHandle -ne 0 -and $process.Responding) { break }
        Start-Sleep -Milliseconds 50
    }
    Invoke-Probe 'lib' | Out-Null
    foreach ($width in @(900, 760, 620, 500)) {
        Invoke-Probe 'window-resize' "${width}x640" | Out-Null
        $state = Invoke-Probe 'window-state'
        for ($stable = 0; $stable -lt 40; $stable++) {
            Start-Sleep -Milliseconds 50
            $next = Invoke-Probe 'window-state'
            if ($next -eq $state) { break }
            $state = $next
        }
        $path = Join-Path $EvidenceRoot "main-${Dpi}dpi-${width}dip-normal.bmp"
        Invoke-Probe 'window-capture' $path | Out-Null
        Add-CaptureRecord $path 'main' 'normal' 'none' $width 640 `
            'library' "resize ${width}x640" "main.$Dpi.$width.normal"
    }

    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'presence-enable' | Out-Null
    foreach ($affordance in @('normal', 'hover', 'pressed')) {
        $path = Join-Path $EvidenceRoot "options-${Dpi}dpi-${affordance}.bmp"
        Invoke-Probe 'options-affordance-capture' "${affordance}|${path}" | Out-Null
        Add-CaptureRecord $path 'options' $affordance $affordance 620 680 `
            'presence-enabled' "listen button $affordance" `
            "options.$Dpi.$affordance"
    }
    Invoke-Probe 'presence-disable' | Out-Null
    $path = Join-Path $EvidenceRoot "options-${Dpi}dpi-disabled.bmp"
    Invoke-Probe 'options-affordance-capture' "disabled|${path}" | Out-Null
    Add-CaptureRecord $path 'options' 'disabled' 'disabled' 620 680 `
        'presence-disabled' 'disabled presence dependency' `
        "options.$Dpi.disabled"
    Invoke-Probe 'options-close' | Out-Null
}
finally {
    if (-not $LeaveOpen) {
        try { & $app /exit | Out-Null } catch {}
        if ($null -ne $process -and -not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force
        }
    }
}

$manifest = [ordered]@{
    schema = 'nekorox.visual-copy-affordance.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    requestedDpiMatrix = @(96, 120, 144, 192)
    capturedDpi = $Dpi
    widthsDip = @(900, 760, 620, 500)
    interactions = @('normal', 'hover', 'pressed', 'disabled')
    stabilization = 'two identical state polls; no fixed capture sleep'
    captures = @($captures)
}
$manifestPath = Join-Path $EvidenceRoot 'manifest.json'
$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Visual copy/affordance manifest: $manifestPath"
