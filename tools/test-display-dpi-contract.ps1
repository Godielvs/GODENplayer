param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$CandidateExe = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$patch = Join-Path $repo 'third_party/columns-ui/0001-goden-pmv2-window-context.patch'
$patchText = Get-Content -LiteralPath $patch -Raw
foreach ($token in @('DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2', 'SetThreadDpiAwarenessContext', 'ScopedWindowDpi', 'WM_DPICHANGED', 'Goden.Dpi.HostHandled', 'Goden.Dpi.ColumnsHandled')) {
    if (!$patchText.Contains($token)) { throw "Columns UI DPI patch contract is missing: $token" }
}
$chromeSource = Get-Content -LiteralPath (
    Join-Path $repo 'src\component\chrome.cpp') -Raw
foreach ($requiredText in @(
    'case WM_ENTERSIZEMOVE:',
    'case WM_EXITSIZEMOVE:',
    'g_interactiveGeometryChange = true;',
    'if (g_interactiveGeometryChange || !g_initialRelayoutPending',
    'ordinary system-setting broadcasts do not',
    'case WM_WINDOWPOSCHANGING:',
    'case WM_SIZING:',
    'case WM_MOVING:',
    'commitInteractiveGeometry(window);',
    'preserveInteractiveGeometry(',
    'preserveCommittedGeometry(',
    'restoreCommittedGeometryAfterChange(',
    'g_restoringCommittedGeometry',
    'SWP_NOSENDCHANGING',
    'g_interactiveGeometryLatest',
    'g_initialRelayoutEpoch',
    'invalidateInitialRelayout();',
    '!sameGeometry(proposed, g_interactiveGeometryStart)',
    '(finalPosition->flags & kSwpStateChanged) != 0'
)) {
    if (-not $chromeSource.Contains($requiredText)) {
        throw "Interactive window geometry can still be overwritten: $requiredText"
    }
}
if ($chromeSource.Contains('GetTickCount64() + 2000') -or
        $chromeSource.Contains('SWP_FRAMECHANGED | kSwpStateChanged')) {
    throw 'Interactive geometry protection is still time-limited or bypassed by frame-only relayout.'
}
if ($chromeSource.Contains('case WM_CANCELMODE:')) {
    throw 'WM_CANCELMODE still discards an otherwise valid resize transaction.'
}
$exitBlock = [regex]::Match($chromeSource,
    'case WM_EXITSIZEMOVE:\s*\{(?s:.*?)case WM_WINDOWPOSCHANGING:').Value
if ([string]::IsNullOrWhiteSpace($exitBlock) -or
        $exitBlock.IndexOf('DefSubclassProc', [StringComparison]::Ordinal) -lt 0 -or
        $exitBlock.IndexOf('commitInteractiveGeometry', [StringComparison]::Ordinal) -lt 0 -or
        $exitBlock.IndexOf('DefSubclassProc', [StringComparison]::Ordinal) -gt
            $exitBlock.IndexOf('commitInteractiveGeometry', [StringComparison]::Ordinal)) {
    throw 'WM_EXITSIZEMOVE still commits before downstream host processing.'
}

function Test-InteractiveGeometryGuard(
    [int[]]$Start, [int[]]$Latest, [int[]]$Incoming,
    [int[]]$Downstream, [bool]$StateChanged = $false) {
    if ($StateChanged) { return $Downstream }
    $equal = {
        param([int[]]$Left, [int[]]$Right)
        return (($Left[0] -eq $Right[0]) -and
            ($Left[1] -eq $Right[1]) -and
            ($Left[2] -eq $Right[2]) -and
            ($Left[3] -eq $Right[3]))
    }
    $authoritative = $Incoming
    if (& $equal $authoritative $Start) { $authoritative = $Latest }
    if ((& $equal $authoritative $Start) -or
            -not (& $equal $Downstream $Start)) {
        return $Downstream
    }
    return $authoritative
}

$startGeometry = @(100, 100, 900, 700)
$liveGeometry = @(100, 100, 1220, 820)
foreach ($case in @(
    @{ Name = 'same-message rollback'; Incoming = $liveGeometry; State = $false },
    @{ Name = 'separate stale placement'; Incoming = $startGeometry; State = $false },
    @{ Name = 'frame-changed stale placement'; Incoming = $startGeometry; State = $false },
    @{ Name = 'second stale placement'; Incoming = $startGeometry; State = $false }
)) {
    $actual = Test-InteractiveGeometryGuard $startGeometry $liveGeometry `
        $case.Incoming $startGeometry $case.State
    if (($actual -join ',') -ne ($liveGeometry -join ',')) {
        throw "Interactive rollback model failed: $($case.Name)"
    }
}
$returnedToStart = Test-InteractiveGeometryGuard $startGeometry $startGeometry `
    $startGeometry $startGeometry $false
if (($returnedToStart -join ',') -ne ($startGeometry -join ',')) {
    throw 'A genuine user drag back to the original rectangle was blocked.'
}
$stateTransition = Test-InteractiveGeometryGuard $startGeometry $liveGeometry `
    $liveGeometry $startGeometry $true
if (($stateTransition -join ',') -ne ($startGeometry -join ',')) {
    throw 'A genuine system state transition was blocked.'
}

function Convert-Dip([int]$Dip, [int]$Dpi) {
    return [int][Math]::Floor(($Dip * $Dpi + 48) / 96)
}

$matrix = @(
    @{ Dpi = 96;  WorkW = 1920; WorkH = 1040; ExpectedW = 720;  ExpectedH = 620 },
    # Non-standard custom scales deliberately cover the old notebook failure.
    @{ Dpi = 110; WorkW = 1620; WorkH = 980;  ExpectedW = 825;  ExpectedH = 710 },
    @{ Dpi = 120; WorkW = 1920; WorkH = 1040; ExpectedW = 900;  ExpectedH = 775 },
    @{ Dpi = 134; WorkW = 1620; WorkH = 980;  ExpectedW = 1005; ExpectedH = 865 },
    # Former notebook-shaped case: 16:10-ish 1620 px panel at 150%.
    @{ Dpi = 144; WorkW = 1620; WorkH = 980;  ExpectedW = 1080; ExpectedH = 930 },
    @{ Dpi = 168; WorkW = 1620; WorkH = 980;  ExpectedW = 1260; ExpectedH = 980 },
    # The work area is authoritative when the nominal DIP minimum cannot fit.
    @{ Dpi = 192; WorkW = 1920; WorkH = 1040; ExpectedW = 1440; ExpectedH = 1040 },
    @{ Dpi = 240; WorkW = 3840; WorkH = 2060; ExpectedW = 1800; ExpectedH = 1550 }
)
foreach ($case in $matrix) {
    $width = [Math]::Min((Convert-Dip 720 $case.Dpi), $case.WorkW)
    $height = [Math]::Min((Convert-Dip 620 $case.Dpi), $case.WorkH)
    if ($width -ne $case.ExpectedW -or $height -ne $case.ExpectedH) {
        throw "Safe-minimum matrix failed at DPI $($case.Dpi): ${width}x${height}"
    }
}

function Get-FittedRootBounds(
    [int]$Dpi, [int]$WorkX, [int]$WorkY, [int]$WorkW, [int]$WorkH,
    [int]$X, [int]$Y, [int]$Width, [int]$Height) {
    $minimumWidth = [Math]::Min((Convert-Dip 720 $Dpi), $WorkW)
    $minimumHeight = [Math]::Min((Convert-Dip 620 $Dpi), $WorkH)
    $fittedWidth = [Math]::Max($minimumWidth, [Math]::Min($Width, $WorkW))
    $fittedHeight = [Math]::Max($minimumHeight, [Math]::Min($Height, $WorkH))
    $fittedX = [Math]::Max($WorkX,
        [Math]::Min($X, $WorkX + $WorkW - $fittedWidth))
    $fittedY = [Math]::Max($WorkY,
        [Math]::Min($Y, $WorkY + $WorkH - $fittedHeight))
    return [ordered]@{
        X = $fittedX; Y = $fittedY
        Width = $fittedWidth; Height = $fittedHeight
    }
}

$restoredBoundsMatrix = @(
    [ordered]@{
        Name = '96dpi-profile-on-150pct-16x10'
        Actual = Get-FittedRootBounds 144 0 0 1620 980 -400 20 820 700
        Expected = [ordered]@{ X = 0; Y = 20; Width = 1080; Height = 930 }
    },
    [ordered]@{
        Name = 'oversized-profile-on-custom-140pct'
        Actual = Get-FittedRootBounds 134 0 0 1620 980 80 -120 1900 1200
        Expected = [ordered]@{ X = 0; Y = 0; Width = 1620; Height = 980 }
    },
    [ordered]@{
        Name = 'negative-origin-secondary-monitor'
        Actual = Get-FittedRootBounds 120 -1920 0 1920 1040 -2500 70 700 500
        Expected = [ordered]@{ X = -1920; Y = 70; Width = 900; Height = 775 }
    }
)
foreach ($case in $restoredBoundsMatrix) {
    foreach ($field in @('X', 'Y', 'Width', 'Height')) {
        if ($case.Actual[$field] -ne $case.Expected[$field]) {
            throw "Restored-window fit failed ($($case.Name), $field): " +
                "$($case.Actual[$field]) != $($case.Expected[$field])"
        }
    }
}

$qaRoot = Join-Path $repo ('artifacts\display-dpi-contract-' +
    [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $qaRoot | Out-Null
$runtime = $null
if ($CandidateExe) {
    $candidate = [IO.Path]::GetFullPath($CandidateExe)
    $probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
    $output = & $probe $candidate window-dpi-state --background 2>&1
    if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch 'pmv2=1') {
        throw "Running candidate is not Per-Monitor aware: $output"
    }
    $runtime = $output -join "`n"
}

$evidence = [ordered]@{
    schema = 'goden.display-dpi-contract.v1'
    passed = $true
    columnsPatchSha256 = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash
    matrix = $matrix
    restoredBoundsMatrix = $restoredBoundsMatrix
    runtime = $runtime
}
$evidence | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
    Join-Path $qaRoot 'result.json') -Encoding UTF8
Write-Host 'GODENplayer display/DPI contract: PASS'
