param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)]
    [string]$DevRoot,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$FirstTrack,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$SecondTrack,
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
if (-not ($devRootFull + '\').StartsWith(
        $artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Album GlyphFlow QA is restricted to disposable artifacts: $devRootFull"
}

$app = Join-Path $devRootFull 'app\foobar2000.exe'
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $devRootFull 'album-glyphflow-transition.json'
}
foreach ($required in @($app, $probe, $FirstTrack, $SecondTrack)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Album GlyphFlow QA input is missing: $required"
    }
}

function Get-QaProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) { throw 'Multiple disposable QA players are running.' }
    if ($matches.Count -eq 0) { return $null }
    return Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue
}

function Wait-QaProcess([bool]$Present, [int]$TimeoutMs = 15000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-QaProcess
        if ($Present -and $null -ne $process) { return $process }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 40
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw 'Disposable QA player did not reach its expected process state.'
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $probe @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $prior
    }
    if ($exitCode -ne 0) {
        throw "Background probe '$Action' failed with exit code ${exitCode}: $($output -join "`n")"
    }
    return ($output | ForEach-Object { $_.ToString() }) -join "`n"
}

function Read-TransitionState {
    $raw = Invoke-Probe 'shell-album-transition-state'
    if ($raw -notmatch
            'active=(true|false)\s+presentedFrames=(\d+)\s+glyphFrames=(\d+)\s+maxFrameMs=([0-9.]+)\s+generation=(\d+)') {
        throw "Could not parse Album artwork transition state: $raw"
    }
    return [pscustomobject]@{
        Raw = $raw
        Active = $Matches[1] -eq 'true'
        PresentedFrames = [uint64]$Matches[2]
        GlyphFrames = [uint64]$Matches[3]
        MaximumFrameMilliseconds = [double]::Parse(
            $Matches[4], [Globalization.CultureInfo]::InvariantCulture)
        Generation = [uint64]$Matches[5]
    }
}

$startedHere = $false
$before = $null
$observed = $null
$settled = $null
$firstObservedAt = 0
$sustainedMotion = $false
$samples = [Collections.Generic.List[string]]::new()
try {
    if ($null -eq (Get-QaProcess)) {
        Start-Process -FilePath $app -WindowStyle Minimized
        Wait-QaProcess $true | Out-Null
        $startedHere = $true
        Start-Sleep -Milliseconds 900
    }
    Invoke-Probe 'window-offscreen-show' | Out-Null
    Invoke-Probe 'mute-on' | Out-Null
    Invoke-Probe 'drop-file' $FirstTrack | Out-Null
    Start-Sleep -Milliseconds 900
    Invoke-Probe 'shell-album' | Out-Null

    $readyWatch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $before = Read-TransitionState
        if ($before.Generation -gt 0) { break }
        Start-Sleep -Milliseconds 20
    } while ($readyWatch.ElapsedMilliseconds -lt 5000)
    if ($before.Generation -eq 0) {
        throw "Initial Album artwork did not become ready: $($before.Raw)"
    }

    Invoke-Probe 'drop-file' $SecondTrack | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Read-TransitionState
        $samples.Add(("{0} ms :: {1}" -f $watch.ElapsedMilliseconds, $state.Raw))
        if ($state.Generation -gt $before.Generation -and
                $state.PresentedFrames -gt 0 -and
                $state.GlyphFrames -gt 0) {
            if ($null -eq $observed) {
                $observed = $state
                $firstObservedAt = $watch.ElapsedMilliseconds
            }
            if ($state.Active -and
                    $watch.ElapsedMilliseconds - $firstObservedAt -ge 200 -and
                    $state.GlyphFrames -gt $observed.GlyphFrames) {
                $sustainedMotion = $true
            }
            if (-not $state.Active) { $settled = $state; break }
        }
        Start-Sleep -Milliseconds 8
    } while ($watch.ElapsedMilliseconds -lt 2000)

    if ($null -eq $observed) {
        throw "Album changed artwork without presenting GlyphFlow frames: $($samples -join '; ')"
    }
    if (-not $sustainedMotion -or $null -eq $settled -or
            $settled.GlyphFrames -le $observed.GlyphFrames) {
        throw "Album reconstruction ended early or failed to resolve: $($samples -join '; ')"
    }
    if ($settled.MaximumFrameMilliseconds -gt 16.0) {
        throw "Album GlyphFlow frame exceeded the 16 ms render budget: $($observed.Raw)"
    }

    [ordered]@{
        schema = 'goden.r3.album-glyphflow-transition.v1'
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        app = $app
        firstTrack = $FirstTrack
        secondTrack = $SecondTrack
        initialState = $before.Raw
        observedState = $observed.Raw
        settledState = $settled.Raw
        sustainedMotion = $sustainedMotion
        samples = $samples
        renderBudgetMilliseconds = 16.0
        sameRendererContract = 'ArtworkTransitionRenderer used by Listen and Album'
        status = 'pass'
    } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $ManifestPath -Encoding UTF8
}
finally {
    if ($startedHere -and $null -ne (Get-QaProcess)) {
        try { Invoke-Probe 'app-close' | Out-Null } catch {}
        try { Wait-QaProcess $false | Out-Null } catch {}
    }
}

Write-Host $observed.Raw
Write-Host 'Installed Album GlyphFlow transition: PASS'
Write-Host "Manifest: $ManifestPath"
