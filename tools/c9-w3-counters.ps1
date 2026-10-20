param(
    [string]$LabRoot = '',
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FixtureAlbum,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FixtureArtist,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FixtureTitle,
    [int]$Notches = 6,
    [int]$StepMs = 10
)

# C9 Gate W3 (counters) — observable wheel transport on a genuinely scrollable
# list, using the C9 QA mirror (callbacks / accepted / delivered / cancelled /
# post failures) plus the presented scroll position.
#
# The C9 counters live in the shell process, so the fixture album and the synced
# lyrics document must be staged into the lab profile BEFORE the candidate
# launches: NEKOROX_QA_HOOKS/NEKOROX_QA_PROFILE are read once at boot.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $LabRoot) { $LabRoot = Join-Path $repo 'artifacts\c9-lab' }
$lab = [IO.Path]::GetFullPath($LabRoot).TrimEnd('\')
if (-not ($lab + '\').StartsWith($artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to run outside artifacts: $lab"
}
$app = Join-Path $lab 'app'
$exe = Join-Path $app 'foobar2000.exe'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
$reader = Join-Path $repo 'artifacts\c9-qualification\c9_qa_read.exe'
$out = Join-Path $repo 'artifacts\c9-qualification\W3'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($required in @($exe, $probe, $reader, $FixtureAlbum)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "W3 counter input missing: $required" }
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($exe, $Action)
    if ($Argument) { $arguments += $Argument }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $result = & $probe @arguments 2>&1; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    return [pscustomobject]@{ Exit = $code; Text = (($result | ForEach-Object { $_.ToString() }) -join "`n").Trim() }
}

function Get-Telemetry {
    $line = (Invoke-Probe 'shell-album-scroll-telemetry').Text
    $values = @{}
    foreach ($pair in ($line -split '\s+')) {
        if ($pair -match '^([A-Za-z]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
    }
    return $values
}

function Get-Diag {
    $raw = & $reader $exe 100 2>&1 | Out-String
    $values = @{}
    foreach ($line in ($raw -split "`r?`n")) {
        if ($line -match '^slot (\d+)=(-?\d+)$') { $values[[int]$Matches[1]] = [int64]$Matches[2] }
    }
    if ($values.Count -eq 0) { throw "C9 diagnostics unavailable: $raw" }
    return $values
}

function Invoke-Schedule([int[]]$Deltas, [string]$Aim) {
    $events = New-Object System.Collections.Generic.List[string]
    $t = 0
    foreach ($d in $Deltas) { $events.Add("${t}:${d}"); $t += $StepMs * 1000 }
    $encoded = "album-send-input|$t|$t|$($events -join ',')|$Aim"
    return Invoke-Probe 'workpack-wheel-driver' $encoded
}

# --- stage the fixture album + a long synced lyrics document into the lab ------
$lyricsRoot = Join-Path $app 'profile\lyrics-v2'
New-Item -ItemType Directory -Force -Path $lyricsRoot | Out-Null
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("[ar:$FixtureArtist]")
$lines.Add("[ti:$FixtureTitle]")
for ($i = 0; $i -lt 120; $i++) {
    $ms = 3000 + $i * 2600
    $stamp = '{0:d2}:{1:d2}.{2:d2}' -f [int]($ms / 60000), [int](($ms % 60000) / 1000), [int](($ms % 1000) / 10)
    $lines.Add("[$stamp] C9 wheel fixture line $($i + 1)")
}
$lines | Set-Content -LiteralPath (Join-Path $lyricsRoot "$FixtureArtist - $FixtureTitle.lrc") -Encoding UTF8

# --- launch the candidate with QA hooks ---------------------------------------
$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
$env:GODEN_FLAG_QA_TRACE_V2 = '1'
$env:GODEN_FLAG_TAG_IMPORT_V1 = '0'
Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like '*c9-lab*' } | ForEach-Object { Stop-Process -Id $_.Id -Force }
Start-Sleep -Seconds 3
Start-Process -FilePath $exe -WorkingDirectory $app -WindowStyle Minimized | Out-Null
Start-Sleep -Seconds 5
[void](Invoke-Probe 'window-offscreen-show')
Start-Sleep -Milliseconds 400
[void](Invoke-Probe 'window-resize' '1280x640')
[void](Invoke-Probe 'window-redraw')
[void](Invoke-Probe 'mute-on')
[void](Invoke-Probe 'drop-file' $FixtureAlbum)
Start-Sleep -Milliseconds 2000
# drop-file already starts playback. Toggling Play here paused the fixture and
# made the gate diverge from a real active Album session.
[void](Invoke-Probe 'shell-album')
Start-Sleep -Milliseconds 2000
[void](Invoke-Probe 'window-visible-focus' 'no-focus')
Start-Sleep -Milliseconds 400

$state = Get-Telemetry
for ($attempt = 0; $attempt -lt 20 -and [int]$state['trackCount'] -lt 12; $attempt++) {
    [void](Invoke-Probe 'shell-album')
    Start-Sleep -Milliseconds 600
    $state = Get-Telemetry
}
Write-Host ("fixture album: tracks={0} trackMaximum={1} lyricMaximum={2}" -f `
    $state['trackCount'], $state['trackMaximum'], $state['lyricMaximum'])
if ([int]$state['trackCount'] -lt 12) { throw "Fixture album too small: $($state['trackCount'])" }
if ([double]$state['trackMaximum'] -le 0) { throw 'TRACKS is not scrollable with this fixture' }

# Aim at the live geometry the product reports, not at guessed coordinates.
$tracksRect = ($state['tracks'] -split ',')
$lyricsRect = ($state['lyrics'] -split ',')
$tracksAim = '{0}:{1}' -f [int](([double]$tracksRect[0] + [double]$tracksRect[2]) / 2),
                            [int](([double]$tracksRect[1] + [double]$tracksRect[3]) / 2)
$lyricsAim = '{0}:{1}' -f [int](([double]$lyricsRect[0] + [double]$lyricsRect[2]) / 2),
                            [int](([double]$lyricsRect[1] + [double]$lyricsRect[3]) / 2)
Write-Host "aim: TRACKS=$tracksAim LYRICS=$lyricsAim"

$report = [System.Collections.Generic.List[object]]::new()

function Measure-Wheel {
    param([string]$Name, [string]$Surface, [string]$Aim, [int[]]$Deltas)
    # Foreground is a precondition of SendInput, not an outcome of the scroll
    # transport.  Reassert it immediately before every schedule so a single
    # transient focus steal cannot be misreported as a product failure.
    $focus = Invoke-Probe 'window-visible-focus' 'no-focus'
    if ($focus.Exit -ne 0) { throw "Could not establish foreground for ${Name}: $($focus.Text)" }
    $b = Get-Diag
    $t0 = Get-Telemetry
    $driver = Invoke-Schedule $Deltas $Aim
    Start-Sleep -Milliseconds 1400
    $a = Get-Diag
    $t1 = Get-Telemetry
    $driverLine = ($driver.Text -split "`r?`n" | Where-Object { $_ -match 'WORKPACK_WHEEL_DRIVER' } | Select-Object -First 1)
    $row = [ordered]@{
        scenario        = $Name
        surface         = $Surface
        aim             = $Aim
        notches         = $Deltas.Count
        driverExit      = $driver.Exit
        driver          = $driverLine
        foregroundSetup = $focus.Text
        hookCallbacks   = $a[12] - $b[12]
        hookAccepted    = $a[13] - $b[13]
        hookPostFailure = $a[14] - $b[14]
        hookDelivered   = $a[15] - $b[15]
        hookCancelled   = $a[16] - $b[16]
        trackPosDelta   = [double]$t1['trackPosition'] - [double]$t0['trackPosition']
        lyricPosDelta   = [double]$t1['lyricPosition'] - [double]$t0['lyricPosition']
        trackPosition   = [double]$t1['trackPosition']
        lyricPosition   = [double]$t1['lyricPosition']
        lightMasterPpm  = $a[20]
        lightValid      = $a[21]
    }
    $report.Add([pscustomobject]$row)
    $report[-1] | Format-List | Out-String -Width 200 | Write-Host
    return $row
}

# TRACKS: down burst, then the equivalent sub-notch burst.
[void](Invoke-Probe 'shell-album-scroll-telemetry' 'reset')
Start-Sleep -Milliseconds 250
[void](Measure-Wheel -Name 'tracks-notch-burst' -Surface 'TRACKS' -Aim $tracksAim `
    -Deltas (1..$Notches | ForEach-Object { -120 }))
[void](Invoke-Probe 'shell-album-scroll-telemetry' 'reset')
Start-Sleep -Milliseconds 250
[void](Measure-Wheel -Name 'tracks-subnotch-burst' -Surface 'TRACKS' -Aim $tracksAim `
    -Deltas (1..($Notches * 4) | ForEach-Object { -30 }))
[void](Invoke-Probe 'shell-album-scroll-telemetry' 'reset')
Start-Sleep -Milliseconds 250
[void](Measure-Wheel -Name 'tracks-reversal' -Surface 'TRACKS' -Aim $tracksAim `
    -Deltas (@(1..4 | ForEach-Object { -120 }) + @(1..3 | ForEach-Object { 120 })))

# LYRICS: same contract on the other column.
[void](Invoke-Probe 'shell-album-scroll-telemetry' 'reset')
Start-Sleep -Milliseconds 250
[void](Measure-Wheel -Name 'lyrics-notch-burst' -Surface 'LYRICS' -Aim $lyricsAim `
    -Deltas (1..$Notches | ForEach-Object { -120 }))
[void](Invoke-Probe 'shell-album-scroll-telemetry' 'reset')
Start-Sleep -Milliseconds 250
[void](Measure-Wheel -Name 'lyrics-subnotch-burst' -Surface 'LYRICS' -Aim $lyricsAim `
    -Deltas (1..($Notches * 4) | ForEach-Object { -30 }))
[void](Invoke-Probe 'shell-album-scroll-telemetry' 'reset')
Start-Sleep -Milliseconds 250
[void](Measure-Wheel -Name 'lyrics-reversal' -Surface 'LYRICS' -Aim $lyricsAim `
    -Deltas (@(1..4 | ForEach-Object { -120 }) + @(1..3 | ForEach-Object { 120 })))

$summary = [ordered]@{
    schema       = 'goden.c9.w3-wheel-counters.v1'
    trackCount   = [int]$state['trackCount']
    trackMaximum = $state['trackMaximum']
    lyricMaximum = $state['lyricMaximum']
    aimTracks    = $tracksAim
    aimLyrics    = $lyricsAim
    scenarios    = $report
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'W3-wheel-counters.json') -Encoding UTF8
Write-Host "Wrote $out\W3-wheel-counters.json"
Remove-Item Env:NEKOROX_QA_HOOKS -ErrorAction SilentlyContinue
Remove-Item Env:NEKOROX_QA_PROFILE -ErrorAction SilentlyContinue
