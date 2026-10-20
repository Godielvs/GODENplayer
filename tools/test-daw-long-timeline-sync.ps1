param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [Parameter(Mandatory = $true)]
    [string]$FixtureFolder,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$fixtureFull = [IO.Path]::GetFullPath($FixtureFolder)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\daw-long-timeline-sync.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
foreach ($required in @($appFull, $fixtureFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "DAW long-timeline QA input is missing: $required"
    }
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $manifestFull)) | Out-Null

function Invoke-Probe([string]$Action, [string]$Argument = '', [switch]$AllowFailure) {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $output = & $probe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "DAW long-timeline probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{ Success = $exitCode -eq 0; Output = $text }
}

function Position-Milliseconds {
    $state = Invoke-Probe 'daw-position'
    if ($state.Output -notmatch 'DAW_POSITION ms=(\d+)') {
        throw "Could not parse DAW position: $($state.Output)"
    }
    return [int64]$Matches[1]
}

function Wait-Audio([int]$Expected, [int]$TimeoutMs = 90000) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'daw-state' -AllowFailure
        if ($state.Success -and $state.Output -match
                'tracks=(\d+)\s+ready=(\d+)\s+playing=(\d+)\s+audio=(\d+)\s+failed=(\d+)') {
            if ([int]$Matches[5] -ne 0) {
                throw "A real DAW stem failed to decode: $($state.Output)"
            }
            if ([int]$Matches[1] -eq $Expected -and
                    [int]$Matches[4] -eq $Expected) {
                return $timer.Elapsed.TotalMilliseconds
            }
        }
        Start-Sleep -Milliseconds 50
    } while ($timer.ElapsedMilliseconds -lt $TimeoutMs)
    throw "DAW did not decode $Expected stems in ${TimeoutMs}ms. Last: $($state.Output)"
}

function Wait-Lifecycle([string]$Expected, [int]$TimeoutMs = 5000) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'daw-lifecycle' -AllowFailure
        if ($state.Success -and $state.Output -match 'state=(\w+)' -and
                $Matches[1] -eq $Expected) { return }
        Start-Sleep -Milliseconds 20
    } while ($timer.ElapsedMilliseconds -lt $TimeoutMs)
    throw "DAW did not enter '$Expected'. Last: $($state.Output)"
}

function Stop-Candidate([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    & $appFull /exit | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        Stop-Process -Id $Process.Id -Force
        throw 'DAW long-timeline candidate required forced termination.'
    }
}

$existing = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq 'foobar2000.exe' -and $_.ExecutablePath -and
    [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
        $appFull, [StringComparison]::OrdinalIgnoreCase) })
if ($existing.Count -gt 1) { throw 'Multiple DAW candidates are running.' }
if ($existing.Count -eq 1) {
    & $appFull /exit | Out-Null
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        if (-not (Get-Process -Id $existing[0].ProcessId -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 25
    }
}

$process = $null
try {
    $env:NEKOROX_QA_HOOKS = '1'
    $env:NEKOROX_QA_PROFILE = '1'
    $env:NEKOROX_QA_BACKGROUND = '1'
    $process = Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
    Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
    for ($attempt = 0; $attempt -lt 240; ++$attempt) {
        $window = Invoke-Probe 'window-state' -AllowFailure
        if ($window.Success) { break }
        Start-Sleep -Milliseconds 25
    }
    if (-not $window.Success) { throw 'DAW candidate did not start.' }
    Invoke-Probe 'mute-on' | Out-Null
    Invoke-Probe 'daw-open' | Out-Null
    Invoke-Probe 'daw-drop' $fixtureFull | Out-Null
    $expected = @(Get-ChildItem -LiteralPath $fixtureFull -File |
        Where-Object { $_.Extension -match '^\.(flac|wav|mp3|aif|aiff|ogg|opus|m4a)$' }).Count
    if ($expected -eq 0) { throw 'Fixture folder contains no audio stems.' }
    $decodeMs = Wait-Audio $expected

    $samples = [Collections.Generic.List[object]]::new()
    foreach ($anchor in @(120000, 240000)) {
        Invoke-Probe 'daw-seek' ([string]$anchor) | Out-Null
        Invoke-Probe 'daw-play-toggle' | Out-Null
        Wait-Lifecycle 'playing'
        Start-Sleep -Milliseconds 120
        $first = Position-Milliseconds
        $clock = [Diagnostics.Stopwatch]::StartNew()
        Start-Sleep -Milliseconds 3000
        $last = Position-Milliseconds
        $clock.Stop()
        $positionDelta = $last - $first
        $wallDelta = $clock.Elapsed.TotalMilliseconds
        $drift = [Math]::Abs($positionDelta - $wallDelta)
        if ($drift -gt 60.0) {
            throw "DAW drift at ${anchor}ms was $([Math]::Round($drift, 2))ms."
        }
        $samples.Add([ordered]@{
            anchorMilliseconds = $anchor
            firstPositionMilliseconds = $first
            lastPositionMilliseconds = $last
            wallMilliseconds = [Math]::Round($wallDelta, 3)
            positionMilliseconds = $positionDelta
            absoluteDriftMilliseconds = [Math]::Round($drift, 3)
        })
        Invoke-Probe 'daw-play-toggle' | Out-Null
        Wait-Lifecycle 'paused'
    }
    Invoke-Probe 'daw-close' | Out-Null

    [ordered]@{
        schema = 'goden.daw-long-timeline-sync.v1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        app = $appFull
        fixtureFolder = $fixtureFull
        stems = $expected
        decodeMilliseconds = [Math]::Round($decodeMs, 2)
        passed = $true
        samples = @($samples)
    } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $manifestFull -Encoding UTF8
    Write-Host "GODENplayer DAW long-timeline sync: PASS ($expected stems)"
    Write-Host "Evidence: $manifestFull"
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
    Stop-Candidate $process
}
