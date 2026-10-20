param(
    [string]$LabRoot = '',
    [int]$Samples = 0,
    [int]$SampleMs = 150
)

# C9 Gate W5 (lighting measurement) — eight-region Clóvis response to the five
# original bench fixtures.
#
# This is a MEASUREMENT of the production light model as published by the shell
# (LightBands.master / level[8] / texture[8] through the opt-in C9 QA mirror).
# It is NOT a visual approval: appearance still requires a human watching the
# clips in motion, so this gate can only support or contradict specific
# measurable claims, never grant VISUAL approval.
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
$music = Join-Path $lab 'music'
$out = Join-Path $repo 'artifacts\c9-qualification\W5'
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach ($required in @($exe, $probe, $reader)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "W5 input missing: $required" }
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

function Get-Lights {
    $d = Get-Diag
    $levels = @(); $textures = @()
    for ($i = 0; $i -lt 8; $i++) {
        $levels += [double]$d[22 + $i] / 1000000.0
        $textures += [double]$d[30 + $i] / 1000000.0
    }
    return [pscustomobject]@{
        master   = [double]$d[20] / 1000000.0
        valid    = [int]$d[21]
        levels   = $levels
        textures = $textures
        raw      = $d
    }
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

$fixtures = @(
    @{ File = '01_absolute_level.wav';  Tagged = '01_absolute_level_tagged.flac';  Count = 165; Note = 'five 5 s segments, silence to loudest amplitude' },
    @{ File = '02_texture_constant.wav'; Tagged = '02_texture_constant_tagged.flac'; Count = 45; Note = 'constant tone, near-identical aggregate RMS to 03' },
    @{ File = '03_texture_modulated.wav'; Tagged = '03_texture_modulated_tagged.flac'; Count = 45; Note = 'modulated content, near-identical aggregate RMS to 02' },
    @{ File = '04_regions.wav';          Tagged = '04_regions_tagged.flac';          Count = 150; Note = 'energy walking through the spectrum' },
    @{ File = '05_antiphase.wav';        Tagged = '05_antiphase_tagged.flac';        Count = 45; Note = 'L/R antiphase; power must be preserved, not cancelled' }
)

# Dropping a file starts playback in foobar2000.  The old harness clicked Play
# afterwards and therefore paused every fixture.  It also left the currently
# selected Album visual unspecified, so a Collection preset could legitimately
# publish no A4/Clovis lights.  Make the runtime preconditions explicit.
foreach ($setup in @(
    @{ Action = 'window-visible-focus'; Argument = '' },
    @{ Action = 'options-open'; Argument = '' },
    @{ Action = 'motion-mode'; Argument = '0' },
    @{ Action = 'options-close'; Argument = '' },
    @{ Action = 'shell-album'; Argument = '' },
    @{ Action = 'shell-a4-select'; Argument = '' },
    @{ Action = 'shell-a4-material-clovis'; Argument = '' },
    @{ Action = 'volume-set'; Argument = '35' }
)) {
    $step = Invoke-Probe $setup.Action $setup.Argument
    if ($step.Exit -ne 0) { throw "W5 setup failed ($($setup.Action)): $($step.Text)" }
}

$results = [System.Collections.Generic.List[object]]::new()
foreach ($fixture in $fixtures) {
    $taggedPath = Join-Path $music $fixture.Tagged
    $path = if (Test-Path -LiteralPath $taggedPath) {
        $taggedPath
    } else {
        Join-Path $music $fixture.File
    }
    if (-not (Test-Path -LiteralPath $path)) { Write-Warning "missing fixture $path"; continue }
    $drop = Invoke-Probe 'drop-file' $path
    if ($drop.Exit -ne 0) { throw "Could not start $($fixture.File): $($drop.Text)" }
    Start-Sleep -Milliseconds 450

    # C9's visual demand is intentionally ineligible without artwork.  The
    # tagged, lossless qualification copies carry one disposable cover so this
    # gate exercises the production path rather than measuring all-zero idle
    # state.  WAV fallback remains useful for diagnosing a missing conversion,
    # but it is not eligible for a PASS.
    $ambient = Invoke-Probe 'shell-album-ambient-state'
    $eligible = $ambient.Exit -eq 0 -and
        $ambient.Text -match 'playing=true' -and
        $ambient.Text -match 'artwork=true' -and
        $ambient.Text -match 'featureMask=(?<mask>\d+)' -and
        (([int]$Matches.mask -band 24) -eq 24)

    # Discard the first samples: the analyzer needs a few hops to settle.
    $series = [System.Collections.Generic.List[object]]::new()
    $sampleCount = if ($Samples -gt 0) { $Samples } else { [int]$fixture.Count }
    for ($i = 0; $i -lt $sampleCount; $i++) {
        Start-Sleep -Milliseconds $SampleMs
        $l = Get-Lights
        $series.Add([pscustomobject]@{
            t          = $i
            master     = $l.master
            valid      = $l.valid
            levelSum   = ($l.levels | Measure-Object -Sum).Sum
            textureSum = ($l.textures | Measure-Object -Sum).Sum
            levels     = $l.levels
            textures   = $l.textures
        })
    }
    [void](Invoke-Probe 'stop')
    Start-Sleep -Milliseconds 400

    $masters = $series | ForEach-Object { $_.master }
    $textureSums = $series | ForEach-Object { $_.textureSum }
    $levelSums = $series | ForEach-Object { $_.levelSum }
    $validCount = ($series | Where-Object { $_.valid -ne 0 } | Measure-Object).Count

    # Mean energy per region, so a region-distribution claim can be checked.
    $regionMean = @()
    for ($r = 0; $r -lt 8; $r++) {
        $sum = 0.0
        foreach ($s in $series) { $sum += $s.levels[$r] }
        $regionMean += [math]::Round($sum / $series.Count, 6)
    }

    $results.Add([pscustomobject]@{
        fixture      = $fixture.File
        playedFile   = [IO.Path]::GetFileName($path)
        note         = $fixture.Note
        eligible     = $eligible
        ambient      = $ambient.Text
        samples      = $series.Count
        validSamples = $validCount
        masterMin    = ($masters | Measure-Object -Minimum).Minimum
        masterMax    = ($masters | Measure-Object -Maximum).Maximum
        masterMean   = [math]::Round(($masters | Measure-Object -Average).Average, 6)
        levelSumMin  = [math]::Round(($levelSums | Measure-Object -Minimum).Minimum, 6)
        levelSumMax  = [math]::Round(($levelSums | Measure-Object -Maximum).Maximum, 6)
        textureMin   = [math]::Round(($textureSums | Measure-Object -Minimum).Minimum, 6)
        textureMax   = [math]::Round(($textureSums | Measure-Object -Maximum).Maximum, 6)
        textureMean  = [math]::Round(($textureSums | Measure-Object -Average).Average, 6)
        regionMean   = $regionMean
        series       = $series
    })
    $r = $results[-1]
    Write-Host ("{0}: master[{1}..{2}] mean={3} levelSum[{4}..{5}] texture mean={6} max={7} valid={8}/{9}" -f `
        $r.fixture, $r.masterMin, $r.masterMax, $r.masterMean, $r.levelSumMin, $r.levelSumMax,
        $r.textureMean, $r.textureMax, $r.validSamples, $r.samples)
    Write-Host ("   region mean: {0}" -f ($r.regionMean -join ', '))
}

[ordered]@{
    schema     = 'goden.c9.w5-lighting.v1'
    scope      = 'MEASUREMENT of published LightBands; not a visual approval'
    sampleMs   = $SampleMs
    setup      = 'ALBUM + A4 + Clovis + volume 35; drop-file autoplays (no Play toggle)'
    fixtures   = $results
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'W5-lighting.json') -Encoding UTF8
Write-Host "Wrote $out\W5-lighting.json"
