param(
    [Parameter(Mandatory = $true)]
    [string]$DevRoot,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$MediaRoot,
    [ValidateRange(1, 10)]
    [int]$PrimaryRepetitions = 3,
    [ValidateRange(10, 180)]
    [int]$WindowSeconds = 60,
    [string]$ManifestPath = '',
    [switch]$SkipTransportToggle
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifactRoot = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
if (-not ($devRootFull + '\').StartsWith(
        $artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "R6 qualification is restricted to disposable artifacts: $devRootFull"
}
$app = Join-Path $devRootFull 'app\foobar2000.exe'
$component = Join-Path $devRootFull `
    'app\profile\user-components-x64\foo_nekorox\foo_nekorox.dll'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\r6\ambient-ab.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$track = rg --files $MediaRoot |
    Where-Object { $_ -match '\.(flac|mp3|m4a|aac|ogg|opus|wav|wma)$' } |
    Select-Object -First 1
foreach ($required in @($app, $component, $probe, $track)) {
    if (-not $required -or -not (Test-Path -LiteralPath $required)) {
        throw "R6 qualification input is missing: $required"
    }
}

function Invoke-Probe(
    [string]$Action,
    [string]$Argument = '',
    [switch]$Foreground,
    [switch]$AllowFailure
) {
    $arguments = @($app, $Action)
    if ($Argument) { $arguments += $Argument }
    if (-not $Foreground) { $arguments += '--background' }
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $probe @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $prior }
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "R6 qualification probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{ Success = $exitCode -eq 0; Output = $text }
}

function Stop-ExactProcess([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    Invoke-Probe 'app-close' -Foreground -AllowFailure | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit(5000) | Out-Null
    }
}

function Read-AmbientState {
    $text = (Invoke-Probe 'shell-album-ambient-state').Output
    $pattern = 'mode=(?<mode>\d+) fallback=(?<fallback>\d+) textureUpdates=(?<updates>\d+) featurePublications=(?<publications>\d+) resourceBytes=(?<bytes>\d+) gpuUs=(?<gpu>\d+) gpuP95Us=(?<p95>\d+) gpuMeasured=(?<measured>true|false) degraded=(?<degraded>true|false) featureMask=(?<mask>\d+) processedWindows=(?<windows>\d+).*?controllerState=(?<state>\d+) wantsPump=(?<pump>true|false).*?visualRevision=(?<visual>\d+) presentedRevision=(?<presented>\d+) physicalMotion=(?<motion>true|false) ingestAccepted=(?<accepted>true|false) playbackSerial=(?<serial>\d+)'
    if ($text -notmatch $pattern) {
        throw "Could not parse Album ambient state: $text"
    }
    return [pscustomobject]@{
        raw = $text
        mode = [int]$Matches.mode
        fallback = [int]$Matches.fallback
        textureUpdates = [uint64]$Matches.updates
        featurePublications = [uint64]$Matches.publications
        resourceBytes = [uint64]$Matches.bytes
        gpuMicroseconds = [double]$Matches.gpu
        gpuP95Microseconds = [double]$Matches.p95
        gpuMeasured = $Matches.measured -eq 'true'
        degraded = $Matches.degraded -eq 'true'
        featureMask = [uint32]$Matches.mask
        processedWindows = [uint64]$Matches.windows
        controllerState = [uint32]$Matches.state
        wantsPump = $Matches.pump -eq 'true'
        visualRevision = [uint64]$Matches.visual
        presentedRevision = [uint64]$Matches.presented
        physicalMotion = $Matches.motion -eq 'true'
        ingestAccepted = $Matches.accepted -eq 'true'
        playbackSerial = [uint64]$Matches.serial
    }
}

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) { return 0.0 }
    $rank = [Math]::Ceiling($Percentile * $ordered.Count)
    return [double]$ordered[[Math]::Max(0,
        [Math]::Min($ordered.Count - 1, $rank - 1))]
}

function Start-Scenario([hashtable]$Environment) {
    $existing = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object { $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase) })
    foreach ($item in $existing) {
        Stop-Process -Id $item.ProcessId -Force
    }
    $running = Join-Path $devRootFull 'app\profile\running'
    if (Test-Path -LiteralPath $running -PathType Leaf) {
        Remove-Item -LiteralPath $running -Force
    }
    $baseEnvironment = @{
        GODEN_FLAG_ALBUM_AMBIENT_A1 = '1'
        NEKOROX_QA_HOOKS = '1'
        NEKOROX_QA_PROFILE = '1'
        NEKOROX_QA_BACKGROUND = '1'
    }
    foreach ($pair in $Environment.GetEnumerator()) {
        $baseEnvironment[$pair.Key] = $pair.Value
    }
    foreach ($pair in $baseEnvironment.GetEnumerator()) {
        Set-Item -LiteralPath ("Env:" + $pair.Key) -Value $pair.Value
    }
    try {
        $process = Start-Process -FilePath $app `
            -WorkingDirectory (Split-Path -Parent $app) `
            -WindowStyle Hidden -PassThru
    }
    finally {
        foreach ($key in $baseEnvironment.Keys) {
            Remove-Item -LiteralPath ("Env:" + $key) `
                -ErrorAction SilentlyContinue
        }
    }
    for ($attempt = 0; $attempt -lt 240; ++$attempt) {
        if ((Invoke-Probe 'window-state' -AllowFailure).Success) {
            Invoke-Probe 'window-offscreen-show' | Out-Null
            $transport = (Invoke-Probe 'transport-state').Output
            if ($transport -match 'muted=true') {
                Invoke-Probe 'mute-toggle' | Out-Null
            }
            Invoke-Probe 'volume-set' '1' | Out-Null
            return $process
        }
        Start-Sleep -Milliseconds 25
    }
    Stop-ExactProcess $process
    throw 'R6 qualification candidate did not publish its window.'
}

function Set-AmbientMode([int]$Mode) {
    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'ambient-mode' ([string]$Mode) | Out-Null
    Invoke-Probe 'options-close' | Out-Null
    Invoke-Probe 'shell-album' | Out-Null
}

function Measure-Scenario(
    [string]$Name,
    [int]$Mode,
    [hashtable]$Environment,
    [int]$DurationSeconds,
    [bool]$OpenListen,
    [int]$Repetition
) {
    $process = $null
    try {
        $process = Start-Scenario $Environment
        Invoke-Probe 'drop-file' $track | Out-Null
        Start-Sleep -Milliseconds 800
        Invoke-Probe 'shell-album' | Out-Null
        Set-AmbientMode $Mode
        if ($OpenListen) {
            Invoke-Probe 'listen' -Foreground | Out-Null
            Invoke-Probe 'listen-focus-crt' | Out-Null
        }
        Start-Sleep -Seconds 2
        $initial = Read-AmbientState
        $process.Refresh()
        $previousCpu = $process.TotalProcessorTime.TotalMilliseconds
        $cpuCoreMsPerSecond = [Collections.Generic.List[double]]::new()
        $scroll = ''
        $paused = $false
        $resumed = $false
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
            $second = $watch.Elapsed.TotalSeconds
            if (-not $scroll -and $second -ge $DurationSeconds * 0.5) {
                $scroll = (Invoke-Probe 'benchmark-album-scroll-input' '80').Output
            }
            if (-not $SkipTransportToggle -and -not $paused -and
                    $second -ge $DurationSeconds * (2.0 / 3.0)) {
                Invoke-Probe 'play' | Out-Null
                $paused = $true
            }
            if (-not $SkipTransportToggle -and $paused -and -not $resumed -and
                    $second -ge $DurationSeconds * (5.0 / 6.0)) {
                Invoke-Probe 'play' | Out-Null
                $resumed = $true
            }
            Start-Sleep -Seconds 1
            $process.Refresh()
            if ($process.HasExited -or -not $process.Responding) {
                throw "$Name host became unhealthy."
            }
            $currentCpu = $process.TotalProcessorTime.TotalMilliseconds
            $cpuCoreMsPerSecond.Add($currentCpu - $previousCpu)
            $previousCpu = $currentCpu
        }
        $final = Read-AmbientState
        $events = (Invoke-Probe 'performance-events' '512' -AllowFailure).Output
        if ($OpenListen) { Invoke-Probe 'listen-close' | Out-Null }
        $scrollP95 = 0.0
        if ($scroll -match 'p95_ms=(?<p95>[0-9.]+)') {
            $scrollP95 = [double]::Parse(
                $Matches.p95, [Globalization.CultureInfo]::InvariantCulture)
        }
        return [pscustomobject]@{
            scenario = $Name
            repetition = $Repetition
            durationSeconds = $watch.Elapsed.TotalSeconds
            mode = $Mode
            listenOpen = $OpenListen
            environment = $Environment
            initial = $initial
            final = $final
            textureUpdatesDelta = $final.textureUpdates - $initial.textureUpdates
            featurePublicationsDelta =
                $final.featurePublications - $initial.featurePublications
            textureUpdatesPerSecond =
                ($final.textureUpdates - $initial.textureUpdates) /
                    $watch.Elapsed.TotalSeconds
            cpuCoreMsPerSecondP50 = Get-Percentile $cpuCoreMsPerSecond 0.50
            cpuCoreMsPerSecondP95 = Get-Percentile $cpuCoreMsPerSecond 0.95
            cpuCoreMsPerSecondP99 = Get-Percentile $cpuCoreMsPerSecond 0.99
            scrollInputP95Milliseconds = $scrollP95
            scrollRaw = $scroll
            performanceEventLines = @($events -split "`n").Count
            performanceEventsTail = @($events -split "`n" | Select-Object -Last 80)
        }
    }
    finally { Stop-ExactProcess $process }
}

$runs = [Collections.Generic.List[object]]::new()
for ($repetition = 1; $repetition -le $PrimaryRepetitions; ++$repetition) {
    $runs.Add((Measure-Scenario 'off' 0 @{} $WindowSeconds $false $repetition))
    $runs.Add((Measure-Scenario 'subtle-512' 1 @{} $WindowSeconds $false $repetition))
}
$runs.Add((Measure-Scenario 'worker-only' 1 @{
    NEKOROX_QA_ALBUM_AMBIENT_WORKER_ONLY = '1'
} $WindowSeconds $false 1))
$runs.Add((Measure-Scenario 'forced-320' 1 @{
    NEKOROX_QA_ALBUM_AMBIENT_FORCE_320 = '1'
} $WindowSeconds $false 1))
$runs.Add((Measure-Scenario 'listen-joint' 1 @{} $WindowSeconds $true 1))
$runs.Add((Measure-Scenario 'forced-60hz' 1 @{
    NEKOROX_QA_DISPLAY_REFRESH_NUMERATOR = '60'
    NEKOROX_QA_DISPLAY_REFRESH_DENOMINATOR = '1'
} $WindowSeconds $false 1))
$runs.Add((Measure-Scenario 'effect-failure' 1 @{
    NEKOROX_QA_ALBUM_AMBIENT_EFFECT_FAIL = '1'
} 15 $false 1))
$runs.Add((Measure-Scenario 'rollout-off' 1 @{
    GODEN_FLAG_ALBUM_AMBIENT_A1 = '0'
} 15 $false 1))

$offP95 = @($runs | Where-Object scenario -eq 'off' |
    ForEach-Object scrollInputP95Milliseconds)
$subtleP95 = @($runs | Where-Object scenario -eq 'subtle-512' |
    ForEach-Object scrollInputP95Milliseconds)
$offMedian = Get-Percentile $offP95 0.5
$subtleMedian = Get-Percentile $subtleP95 0.5
$allowedIncrease = [Math]::Max(1.0, $offMedian * 0.05)
$scrollPassed = $subtleMedian - $offMedian -le $allowedIncrease
$subtleRuns = @($runs | Where-Object scenario -eq 'subtle-512')
$offRuns = @($runs | Where-Object scenario -eq 'off')
$workerOnly = $runs | Where-Object scenario -eq 'worker-only' |
    Select-Object -First 1
$forced320 = $runs | Where-Object scenario -eq 'forced-320' |
    Select-Object -First 1
$listenJoint = $runs | Where-Object scenario -eq 'listen-joint' |
    Select-Object -First 1
$forced60 = $runs | Where-Object scenario -eq 'forced-60hz' |
    Select-Object -First 1
$effectFailure = $runs | Where-Object scenario -eq 'effect-failure' |
    Select-Object -First 1
$rolloutOff = $runs | Where-Object scenario -eq 'rollout-off' |
    Select-Object -First 1
# Album bands moved to bit 3 when the shared analysis hub gained the
# independent CompactScope/SpectralHistory/Dynamics leases.  Other visible
# surfaces may legitimately keep those bits active during this qualification,
# so assert ownership of AlbumBands instead of requiring the whole process
# mask to equal the old R6-era value.
$albumBandsMask = 1 -shl 3
$semanticPassed =
    @($offRuns | Where-Object {
        $_.final.fallback -ne 1 -or
        ($_.final.featureMask -band $albumBandsMask) -ne 0 -or
        $_.final.wantsPump }).Count -eq 0 -and
    @($subtleRuns | Where-Object {
        $_.final.fallback -ne 0 -or
        ($_.final.featureMask -band $albumBandsMask) -eq 0 -or
        -not $_.final.wantsPump -or
        -not $_.final.ingestAccepted -or
        $_.final.playbackSerial -eq 0 -or
        $_.textureUpdatesDelta -lt 5 -or
        $_.final.controllerState -notin @(1, 2) }).Count -eq 0 -and
    $workerOnly.textureUpdatesDelta -eq 0 -and
    $workerOnly.featurePublicationsDelta -gt 0 -and
    $workerOnly.final.ingestAccepted -and
    $workerOnly.final.playbackSerial -gt 0 -and
    $workerOnly.final.visualRevision -gt $workerOnly.final.presentedRevision -and
    ($workerOnly.final.featureMask -band $albumBandsMask) -ne 0 -and
    $workerOnly.final.wantsPump -and
    $forced320.final.degraded -and
    $forced320.textureUpdatesDelta -ge 5 -and
    $forced320.final.ingestAccepted -and
    $forced320.final.playbackSerial -gt 0 -and
    $forced320.final.resourceBytes -lt $subtleRuns[0].final.resourceBytes -and
    ($forced320.final.featureMask -band $albumBandsMask) -ne 0 -and
    $forced320.final.wantsPump -and
    ($listenJoint.final.featureMask -band $albumBandsMask) -ne 0 -and
    $listenJoint.textureUpdatesDelta -ge 5 -and
    $listenJoint.final.ingestAccepted -and
    $listenJoint.final.playbackSerial -gt 0 -and
    $listenJoint.final.wantsPump -and
    $forced60.textureUpdatesPerSecond -le 30.01 -and
    $forced60.textureUpdatesDelta -ge 5 -and
    $forced60.final.ingestAccepted -and
    $forced60.final.playbackSerial -gt 0 -and
    ($forced60.final.featureMask -band $albumBandsMask) -ne 0 -and
    $forced60.final.wantsPump -and
    $effectFailure.final.fallback -eq 6 -and
    ($effectFailure.final.featureMask -band $albumBandsMask) -eq 0 -and
    -not $effectFailure.final.wantsPump -and
    $rolloutOff.final.fallback -eq 2 -and
    ($rolloutOff.final.featureMask -band $albumBandsMask) -eq 0 -and
    -not $rolloutOff.final.wantsPump
$result = [ordered]@{
    schema = 'goden.r6.ambient-qualification.v1'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    status = if ($scrollPassed -and $semanticPassed -and
        @($subtleRuns | Where-Object {
            -not $_.final.gpuMeasured -or
            $_.final.gpuP95Microseconds -gt 500 -or
            $_.textureUpdatesPerSecond -gt 30.01 }).Count -eq 0) {
            'pass'
        } else { 'fail' }
    componentSha256 = (Get-FileHash -LiteralPath $component -Algorithm SHA256).Hash
    logicalProcessors = [Environment]::ProcessorCount
    primaryRepetitions = $PrimaryRepetitions
    windowSeconds = $WindowSeconds
    scrollComparison = [ordered]@{
        offMedianP95Milliseconds = $offMedian
        subtleMedianP95Milliseconds = $subtleMedian
        allowedIncreaseMilliseconds = $allowedIncrease
        passed = $scrollPassed
    }
    semanticStatePassed = $semanticPassed
    runs = $runs
}
New-Item -ItemType Directory -Force `
    -Path (Split-Path -Parent $manifestFull) | Out-Null
$result | ConvertTo-Json -Depth 12 |
    Set-Content -LiteralPath $manifestFull -Encoding UTF8
$result | ConvertTo-Json -Depth 5
if ($result.status -ne 'pass') {
    throw "R6 ambient qualification failed. See $manifestFull"
}
