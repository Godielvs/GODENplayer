param(
    [string]$LabRoot = '',
    [string]$OutRoot = ''
)

# C9 Gate W3 — real SendInput wheel / focus / capture / reversal campaign.
#
# Every scenario drives the low-level mouse hook through SendInput (the driver's
# `send-input` mode), which is the only mode that exercises focus routing and
# capture. The candidate's own opt-in counters distinguish callbacks, accepted
# posts, delivered events, cancelled events and post failures, and trackScroll
# position is read before/after so a delivery that does not move the list is
# visible rather than assumed.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $LabRoot) { $LabRoot = Join-Path $repo 'artifacts\c9-lab' }
$lab = [IO.Path]::GetFullPath($LabRoot).TrimEnd('\')
if (-not ($lab + '\').StartsWith($artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to run outside artifacts: $lab"
}
$exe = Join-Path $lab 'app\foobar2000.exe'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
$reader = Join-Path $repo 'artifacts\c9-qualification\c9_qa_read.exe'
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'artifacts\c9-qualification\W3' }
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
foreach ($required in @($exe, $probe, $reader)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "W3 input missing: $required" }
}

# Geometry derived from computeAlbumModeLayout at this window size (3 columns).
# TRACKS/LYRICS column centres in shell client DIP, from the live window tree.
$tracksPoint = '818:492'
$lyricsPoint = '1471:492'

function Get-C9Snapshot {
    param([string]$Tag)
    $raw = & $reader $exe 100 2>&1 | Out-String
    $values = @{}
    foreach ($line in ($raw -split "`r?`n")) {
        if ($line -match '^slot (\d+)=(-?\d+)$') { $values[[int]$Matches[1]] = [int64]$Matches[2] }
    }
    if ($values.Count -eq 0) { throw "C9 diagnostics snapshot failed at $Tag" }
    return $values
}

function Invoke-Scenario {
    param(
        [string]$Name,
        [string]$Surface,
        [string]$Schedule,
        [string]$Description
    )
    $before = Get-C9Snapshot -Tag "$Name-before"
    $point = if ($Surface -eq 'TRACKS') { $tracksPoint } else { $lyricsPoint }
    $encoded = "album-$Schedule|$point"
    $output = & $probe $exe workpack-wheel-driver $encoded 2>&1 | Out-String
    $mode = $LASTEXITCODE
    Start-Sleep -Milliseconds 350
    $after = Get-C9Snapshot -Tag "$Name-after"

    $deliveredDelta = $after[15] - $before[15]
    $acceptedDelta = $after[13] - $before[13]
    $callbackDelta = $after[12] - $before[12]
    $cancelDelta = $after[16] - $before[16]
    $postFailDelta = $after[14] - $before[14]
    $scrollDelta = $after[64] - $before[64]

    $driverLine = ($output -split "`r?`n" | Where-Object { $_ -match 'WORKPACK_WHEEL_DRIVER' } | Select-Object -First 1)
    $scheduled = 0; $driverDelivered = 0; $abort = 'none'
    if ($driverLine -match 'scheduled=(\d+)') { $scheduled = [int]$Matches[1] }
    if ($driverLine -match 'delivered=(\d+)') { $driverDelivered = [int]$Matches[1] }
    if ($driverLine -match 'abort=(\S+)') { $abort = $Matches[1] }

    [pscustomobject]@{
        scenario          = $Name
        surface           = $Surface
        description       = $Description
        exit              = $mode
        scheduled         = $scheduled
        driverDelivered   = $driverDelivered
        abort             = $abort
        hookCallbacks     = $callbackDelta
        hookAccepted      = $acceptedDelta
        hookDelivered     = $deliveredDelta
        hookCancelled     = $cancelDelta
        hookPostFailures  = $postFailDelta
        scrollPositionDelta = $scrollDelta
        scrollBefore      = $before[64]
        scrollAfter       = $after[64]
        lastWheelTick     = $after[17]
    }
}

$results = [System.Collections.Generic.List[object]]::new()

& $probe $exe shell-album | Out-Null
& $probe $exe window-visible-focus | Out-Null
Start-Sleep -Milliseconds 600

# TRACKS: one notch, then the equivalent in four sub-notches (delta 30 x4 for a
# 120 notch). Sub-notch must accumulate rather than be dropped.
$results.Add((Invoke-Scenario -Name 'tracks-1-notch' -Surface 'TRACKS' `
    -Schedule 'send-input|500|500|0:120' -Description 'single 120 notch down'))
$results.Add((Invoke-Scenario -Name 'tracks-4-subnotch' -Surface 'TRACKS' `
    -Schedule 'send-input|500|500|0:30,120:30,240:30,360:30' -Description 'four 30 sub-notches (equivalent to one notch)'))
$results.Add((Invoke-Scenario -Name 'tracks-reverse-in-burst' -Surface 'TRACKS' `
    -Schedule 'send-input|900|900|0:120,150:120,300:-120,450:120,600:-120' -Description 'burst with two reversals'))
$results.Add((Invoke-Scenario -Name 'tracks-sustained-burst' -Surface 'TRACKS' `
    -Schedule 'send-input|2000|200|0:120,40:120,80:120,120:120,160:120' -Description 'sustained fast burst (repeats 10x)'))

# LYRICS: the same contract on the other column.
$results.Add((Invoke-Scenario -Name 'lyrics-1-notch' -Surface 'LYRICS' `
    -Schedule 'send-input|500|500|0:120' -Description 'single 120 notch down'))
$results.Add((Invoke-Scenario -Name 'lyrics-4-subnotch' -Surface 'LYRICS' `
    -Schedule 'send-input|500|500|0:30,120:30,240:30,360:30' -Description 'four 30 sub-notches'))
$results.Add((Invoke-Scenario -Name 'lyrics-reverse-in-burst' -Surface 'LYRICS' `
    -Schedule 'send-input|900|900|0:120,150:120,300:-120,450:120,600:-120' -Description 'burst with reversals'))
$results.Add((Invoke-Scenario -Name 'lyrics-up-notch' -Surface 'LYRICS' `
    -Schedule 'send-input|500|500|0:-120' -Description 'single upward notch (direction integrity)'))

$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutRoot 'W3-wheel-scenarios.json') -Encoding utf8
$results | Format-Table -AutoSize | Out-String -Width 220
