param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    # A portable foobar2000 root that already carries the NekoRoX profile. The
    # test copies it, so the source install is never modified.
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$AppRoot,
    # An album folder with enough tracks to overflow the Album TRACKS viewport.
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FixtureAlbum,
    # Artist and title of the fixture album's first track. The test stages a
    # synthetic synced lyrics document for exactly this pair inside the copied
    # profile, so LYRICS is scrollable without touching the source install or
    # the music folder.
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FixtureArtist,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FixtureTitle,
    [int]$BurstNotches = 10,
    [int]$BurstStepMs = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Album-mode wheel integration contract. The unit suite in
# tests/workpack_album_scroll_tests.cpp pins the controller arithmetic; this
# script pins the real hardware-equivalent path with SendInput. Posting directly
# to the shell skips Windows focus routing and Columns UI's message hook, so it
# cannot reproduce the one-notch-at-a-time failure this gate protects against.
#
# Diagnostics used here are read-only: shell QA selectors 118..127 report both
# Album scroll controllers, and the probe action `shell-album-scroll-telemetry`
# reports them together with the layout the wheel must be aimed at.

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$component = Join-Path $repo "out\$Configuration\foo_nekorox.dll"
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"

foreach ($required in @($component, $probe, (Join-Path $AppRoot 'foobar2000.exe'), $FixtureAlbum)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Album scroll integration input is missing: $required"
    }
}

$qaRoot = Join-Path $repo 'artifacts\album-scroll-integration'
$qaApp = Join-Path $qaRoot 'app'
$app = Join-Path $qaApp 'foobar2000.exe'
$manifestPath = Join-Path $qaRoot 'manifest.json'

function Get-QaProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) { throw 'Multiple QA players are running.' }
    if ($matches.Count -eq 0) { return $null }
    return Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $probe @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previous }
    if ($exitCode -ne 0) {
        throw "Probe '$Action' failed ($exitCode): $($output -join "`n")"
    }
    return (($output | ForEach-Object { $_.ToString() }) -join "`n").Trim()
}

function Get-Telemetry([string]$Extra = '') {
    $line = Invoke-Probe 'shell-album-scroll-telemetry' $Extra
    $values = @{}
    foreach ($pair in ($line -split '\s+')) {
        if ($pair -match '^([A-Za-z]+)=(.*)$') { $values[$Matches[1]] = $Matches[2] }
    }
    foreach ($key in 'albumMode', 'trackCount', 'trackMaximum', 'lyricMaximum',
            'trackPosition', 'trackTarget', 'lyricPosition', 'lyricTarget') {
        if (-not $values.ContainsKey($key)) {
            throw "Album scroll telemetry is missing '$key': $line"
        }
    }
    return $values
}

function Invoke-WheelSchedule([int[]]$Deltas, [int]$StepMs, [string]$Aim) {
    $events = New-Object System.Collections.Generic.List[string]
    $tUs = 0
    foreach ($delta in $Deltas) {
        $events.Add("${tUs}:${delta}")
        $tUs += $StepMs * 1000
    }
    # The schedule ends on its last event so the caller observes the state the
    # burst produced, not a settled list.
    $encoded = "album-send-input|$tUs|$tUs|$($events -join ',')|$Aim"
    return Invoke-Probe 'workpack-wheel-driver' $encoded
}

$script:lastDriver = ''
function Assert-Close([double]$actual, [double]$expected, [double]$tolerance,
        [string]$message) {
    if ([Math]::Abs($actual - $expected) -gt $tolerance) {
        throw "$message (expected $expected +/- $tolerance, observed $actual) driver: $script:lastDriver"
    }
}

$checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$Name, [bool]$Passed, [string]$Detail) {
    $script:checks.Add([ordered]@{ name = $Name; passed = $Passed; detail = $Detail })
    Write-Host ("  [{0}] {1} - {2}" -f ($(if ($Passed) { 'PASS' } else { 'FAIL' }), $Name, $Detail))
    if (-not $Passed) { throw "Album scroll integration check failed: $Name - $Detail" }
}

if (Test-Path -LiteralPath $qaRoot) { Remove-Item -LiteralPath $qaRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $qaApp | Out-Null
& robocopy.exe $AppRoot $qaApp /E /COPY:DAT /DCOPY:T /R:1 /W:1 `
    /NFL /NDL /NJH /NJS /XF running nobadshutdown | Out-Null
if ($LASTEXITCODE -gt 7) { throw "Portable QA copy failed: robocopy $LASTEXITCODE" }
Copy-Item -LiteralPath $component -Destination (Join-Path $qaApp `
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll') -Force

# Stage a synced lyrics document for exactly the fixture track pair inside the
# copied profile. The local provider resolves "<profile>\lyrics-v2\<Artist> -
# <Title>.lrc", so LYRICS becomes scrollable without writing beside the audio
# files or into the source install.
$lyricsRoot = Join-Path $qaApp 'profile\lyrics-v2'
New-Item -ItemType Directory -Force -Path $lyricsRoot | Out-Null
$lyricLines = New-Object System.Collections.Generic.List[string]
$lyricLines.Add("[ar:$FixtureArtist]")
$lyricLines.Add("[ti:$FixtureTitle]")
for ($index = 0; $index -lt 120; $index++) {
    $ms = 3000 + $index * 2600
    $stamp = '{0:d2}:{1:d2}.{2:d2}' -f [int]($ms / 60000),
        [int](($ms % 60000) / 1000), [int](($ms % 1000) / 10)
    $lyricLines.Add("[$stamp] Album scroll fixture line " + ($index + 1))
}
$lyricLines | Set-Content -LiteralPath (Join-Path $lyricsRoot `
    "$FixtureArtist - $FixtureTitle.lrc") -Encoding UTF8

$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
try {
    Start-Process -FilePath $app -WindowStyle Minimized
    Start-Sleep -Milliseconds 4500
    Invoke-Probe 'window-offscreen-show' | Out-Null
    Start-Sleep -Milliseconds 300
    Invoke-Probe 'window-redraw' | Out-Null
    Invoke-Probe 'mute-on' | Out-Null
    # Drop the whole folder: the Album session enumerates the playing album, so
    # a single file would produce a one-track session and no scrollable list.
    Invoke-Probe 'drop-file' $FixtureAlbum | Out-Null
    Start-Sleep -Milliseconds 1500
    Invoke-Probe 'play' | Out-Null
    Start-Sleep -Milliseconds 1200
    Invoke-Probe 'shell-album' | Out-Null
    Start-Sleep -Milliseconds 1800

    # Keep only the foobar top level foreground. Deliberately do not focus the
    # shell: the product must maintain that invariant while the pointer remains
    # over TRACKS/LYRICS, just as it must for a real mouse spin.
    Invoke-Probe 'window-visible-focus' 'no-focus' | Out-Null
    Start-Sleep -Milliseconds 250

    $state = Get-Telemetry
    for ($attempt = 0; $attempt -lt 20 -and [int]$state['trackCount'] -lt 12; $attempt++) {
        Invoke-Probe 'shell-album' | Out-Null
        Start-Sleep -Milliseconds 600
        $state = Get-Telemetry
    }
    if ($state['albumMode'] -ne '1') { throw 'Album mode did not engage' }
    if ([int]$state['trackCount'] -lt 12) {
        throw "Fixture album is too small for a scrollable tracklist: $($state['trackCount']) tracks"
    }
    if ([double]$state['trackMaximum'] -le 0) {
        throw "Fixture album does not overflow the TRACKS viewport"
    }
    if ([double]$state['lyricMaximum'] -le 0) {
        throw "Fixture album exposed no scrollable lyrics document"
    }
    $trackMaximum = [double]$state['trackMaximum']
    $lyricMaximum = [double]$state['lyricMaximum']
    Write-Host ("album: tracks={0} trackMaximum={1} lyricMaximum={2}" -f `
        $state['trackCount'], $trackMaximum, $lyricMaximum)

    # 1. TRACKS: a one-direction burst is consumed in full and clamped exactly.
    [void](Get-Telemetry 'reset')
    Start-Sleep -Milliseconds 200
    $notches = [Math]::Min($BurstNotches, [int][Math]::Floor($trackMaximum / 56.0))
    $deltas = @(1..$notches | ForEach-Object { -120 })
    $script:lastDriver = Invoke-WheelSchedule $deltas $BurstStepMs '500:200'
    Start-Sleep -Milliseconds 1600
    $after = Get-Telemetry
    $expected = [Math]::Min($notches * 56.0, $trackMaximum)
    Assert-Close ([double]$after['trackTarget']) $expected 0.5 'TRACKS burst lost distance'
    Assert-Close ([double]$after['trackPosition']) $expected 0.5 'TRACKS burst did not present its target'
    Add-Check 'tracks-burst-consumed' $true "$notches notches -> $($after['trackPosition']) DIP"

    # 2. TRACKS: a burst past the end clamps on the true maximum, no overshoot.
    $deltas = @(1..40 | ForEach-Object { -120 })
    $script:lastDriver = Invoke-WheelSchedule $deltas $BurstStepMs '500:200'
    Start-Sleep -Milliseconds 1800
    $after = Get-Telemetry
    Assert-Close ([double]$after['trackPosition']) $trackMaximum 0.5 'TRACKS overshot its maximum'
    Assert-Close ([double]$after['trackTarget']) $trackMaximum 0.5 'TRACKS target escaped its maximum'
    Add-Check 'tracks-bound-clamped' $true "clamped at $trackMaximum DIP"

    # 3. LYRICS: same burst reaches the same accumulation and leaves TRACKS alone.
    [void](Get-Telemetry 'reset')
    Start-Sleep -Milliseconds 200
    $before = Get-Telemetry
    $lyricOrigin = [double]$before['lyricTarget']
    $deltas = @(1..$BurstNotches | ForEach-Object { -120 })
    $script:lastDriver = Invoke-WheelSchedule $deltas $BurstStepMs '900:200'
    Start-Sleep -Milliseconds 1600
    $after = Get-Telemetry
    Assert-Close ([double]$after['lyricTarget']) ($lyricOrigin + $BurstNotches * 56.0) 0.5 `
        'LYRICS burst lost distance'
    Assert-Close ([double]$after['lyricPosition']) ($lyricOrigin + $BurstNotches * 56.0) 0.5 `
        'LYRICS burst did not present its target'
    Assert-Close ([double]$after['trackPosition']) 0.0 0.5 'LYRICS input moved the TRACKS list'
    Add-Check 'lyrics-burst-consumed' $true "$BurstNotches notches -> $($after['lyricPosition']) DIP"

    # 4. Sub-notch deltas (high-resolution wheels and touchpads) accumulate in
    #    the same proportion as whole notches.
    [void](Get-Telemetry 'reset')
    Start-Sleep -Milliseconds 200
    $before = Get-Telemetry
    $lyricOrigin = [double]$before['lyricTarget']
    $deltas = @(1..($BurstNotches * 4) | ForEach-Object { -30 })
    $script:lastDriver = Invoke-WheelSchedule $deltas $BurstStepMs '900:200'
    Start-Sleep -Milliseconds 1600
    $after = Get-Telemetry
    Assert-Close ([double]$after['lyricTarget']) ($lyricOrigin + $BurstNotches * 56.0) 0.5 `
        "sub-notch LYRICS deltas lost distance from $lyricOrigin"
    Assert-Close ([double]$after['trackPosition']) 0.0 0.5 `
        'sub-notch LYRICS input moved the TRACKS list'
    Add-Check 'lyrics-subnotch-consumed' $true "4 x sub-notch == 1 notch"

    # 5. Direction reversal before settle. An approved reversal discards the
    #    not-yet-presented debt, so the burst is deliberately not symmetric:
    #    what must hold is that the reversal is accepted, moves the list back by
    #    roughly the notches it was given, stays in bounds, and leaves TRACKS
    #    alone.
    [void](Get-Telemetry 'reset')
    Start-Sleep -Milliseconds 200
    $trackBaseline = [double](Get-Telemetry)['trackPosition']
    $script:lastDriver = Invoke-WheelSchedule @(1..6 | ForEach-Object { -120 }) `
        $BurstStepMs '900:200'
    Start-Sleep -Milliseconds 1200
    $peak = [double](Get-Telemetry)['lyricPosition']
    $script:lastDriver = Invoke-WheelSchedule @(1..6 | ForEach-Object { 120 }) `
        $BurstStepMs '900:200'
    Start-Sleep -Milliseconds 1600
    $after = Get-Telemetry
    $final = [double]$after['lyricPosition']
    Add-Check 'reversal-responds' ($peak -gt 300.0 -and $final -ge 0.0 -and
        ($peak - $final) -ge 200.0) `
        "down peak=${peak} DIP, reversed to ${final} DIP"
    Assert-Close ([double]$after['trackPosition']) $trackBaseline 0.5 `
        'LYRICS reversal moved the TRACKS list'

    # 6. Library/Columns wheel dispatch must not regress against the shell's.
    Invoke-Probe 'shell-library' | Out-Null
    Start-Sleep -Milliseconds 1500
    $library = Invoke-Probe 'benchmark-scroll-input' '40'
    if ($library -notmatch 'p95_ms=([0-9.]+)') {
        throw "Could not parse the Library scroll benchmark: $library"
    }
    $libraryP95 = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
    Add-Check 'library-scroll-p95' ($libraryP95 -le 12.0) "p95=${libraryP95}ms (limit 12ms)"

    [ordered]@{
        schema = 'goden.album_scroll_integration.v1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        appSource = [IO.Path]::GetFullPath($AppRoot)
        fixtureAlbum = $FixtureAlbum
        burstNotches = $BurstNotches
        burstStepMs = $BurstStepMs
        trackCount = [int]$state['trackCount']
        trackMaximum = $trackMaximum
        lyricMaximum = $lyricMaximum
        libraryScrollBenchmark = $library
        checks = $checks
        status = 'pass'
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-Host 'Album scroll integration test: PASS'
    Write-Host "Manifest: $manifestPath"
}
finally {
    $ErrorActionPreference = 'Continue'
    if ($null -ne (Get-QaProcess)) { & $app /exit | Out-Null; Start-Sleep -Milliseconds 1500 }
    Remove-Item Env:NEKOROX_QA_HOOKS -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_PROFILE -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $qaApp) {
        for ($attempt = 0; $attempt -lt 20 -and (Test-Path -LiteralPath $qaApp); ++$attempt) {
            Remove-Item -LiteralPath $qaApp -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $qaApp) { Start-Sleep -Milliseconds 150 }
        }
    }
}
