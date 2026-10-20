param(
    [string]$LabRoot = '',
    [ValidateRange(1, 240)]
    [int]$DurationMinutes = 45,
    [string]$ManifestPath = ''
)

# C9 W6 soak.  Runs only the disposable portable candidate and synthetic C9
# fixtures.  It deliberately never imports or writes the user's music library.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $LabRoot) { $LabRoot = Join-Path $repo 'artifacts\c9-lab' }
$lab = [IO.Path]::GetFullPath($LabRoot).TrimEnd('\')
if (-not ($lab + '\').StartsWith($artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "C9 soak must stay inside artifacts: $lab"
}
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\c9-qualification\W6-soak45.json'
}
$manifest = [IO.Path]::GetFullPath($ManifestPath)
$app = Join-Path $lab 'app\foobar2000.exe'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
$fixtures = @(Get-ChildItem -LiteralPath (Join-Path $lab 'music') `
    -Filter '*_tagged.flac' -File | Sort-Object Name)
foreach ($required in @($app, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing C9 soak input: $required"
    }
}
if ($fixtures.Count -lt 5) { throw 'C9 soak requires all five tagged fixtures.' }

function Invoke-Probe([string]$Action, [string]$Argument = '', [switch]$AllowFailure) {
    $args = @($app, $Action)
    if ($Argument) { $args += $Argument }
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & $probe @args 2>&1; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $prior }
    $text = (($output | ForEach-Object { $_.ToString() }) -join "`n").Trim()
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "Probe $Action failed (${code}): $text"
    }
    [pscustomobject]@{ success = $code -eq 0; output = $text }
}

function Add-Sample([string]$Label) {
    $process.Refresh()
    if ($process.HasExited -or -not $process.Responding) {
        throw "Candidate became unhealthy at $Label"
    }
    $samples.Add([pscustomobject]@{
        label = $Label
        elapsedSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 2)
        cpuMilliseconds = [Math]::Round($process.TotalProcessorTime.TotalMilliseconds, 2)
        handles = $process.HandleCount
        threads = $process.Threads.Count
        workingSetBytes = $process.WorkingSet64
        privateBytes = $process.PrivateMemorySize64
        responding = $process.Responding
        playback = (Invoke-Probe 'playback-state').output
        frameClock = (Invoke-Probe 'frame-clock-state').output
        a4 = (Invoke-Probe 'shell-a4-state').output
        diagnostics = (Invoke-Probe 'shell-a4-r12-diagnostics').output
        ambient = (Invoke-Probe 'shell-album-ambient-state').output
    })
    Write-Host "W6 sample ${Label}: handles=$($process.HandleCount) threads=$($process.Threads.Count) workingSet=$($process.WorkingSet64)"
}

$existing = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals(
        $app, [StringComparison]::OrdinalIgnoreCase) })
if ($existing.Count) { throw "C9 lab is already running (PID $($existing[0].ProcessId))." }
$runningMarker = Join-Path $lab 'app\profile\running'
if (Test-Path -LiteralPath $runningMarker -PathType Leaf) {
    Remove-Item -LiteralPath $runningMarker -Force
}

$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
$env:GODEN_FLAG_QA_TRACE_V2 = '1'
$env:GODEN_FLAG_TAG_IMPORT_V1 = '0'
$process = $null
try {
    $process = Start-Process -FilePath $app -WorkingDirectory (Split-Path $app) -PassThru
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS, Env:NEKOROX_QA_PROFILE,
        Env:GODEN_FLAG_QA_TRACE_V2, Env:GODEN_FLAG_TAG_IMPORT_V1 `
        -ErrorAction SilentlyContinue
}

$samples = [Collections.Generic.List[object]]::new()
$actions = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[object]]::new()
$startedUtc = [DateTime]::UtcNow
$watch = [Diagnostics.Stopwatch]::StartNew()
$gracefulExit = $false
try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 240; ++$attempt) {
        if ((Invoke-Probe 'window-state' -AllowFailure).success) { $ready = $true; break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw 'C9 candidate did not publish its window.' }
    Invoke-Probe 'window-offscreen-show' | Out-Null
    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'motion-mode' '0' | Out-Null
    Invoke-Probe 'options-close' | Out-Null
    Invoke-Probe 'volume-set' '1' | Out-Null
    Invoke-Probe 'shell-album' | Out-Null
    Invoke-Probe 'shell-a4-select' | Out-Null
    Invoke-Probe 'shell-a4-material-clovis' | Out-Null
    Invoke-Probe 'drop-file' $fixtures[0].FullName | Out-Null
    Start-Sleep -Seconds 2
    Add-Sample 'start'

    $nextMedia = 6.0
    $nextAction = 12.0
    $nextSample = 60.0
    $mediaIndex = 0
    $actionIndex = 0
    while ($watch.Elapsed.TotalMinutes -lt $DurationMinutes) {
        $seconds = $watch.Elapsed.TotalSeconds
        if ($seconds -ge $nextMedia) {
            $fixture = $fixtures[$mediaIndex % $fixtures.Count]
            Invoke-Probe 'drop-file' $fixture.FullName | Out-Null
            $actions.Add([pscustomobject]@{
                kind = 'media'
                index = $mediaIndex
                elapsedSeconds = [Math]::Round($seconds, 2)
                fixture = $fixture.Name
            })
            ++$mediaIndex
            $nextMedia += 6.0
        }
        if ($seconds -ge $nextAction) {
            switch ($actionIndex % 8) {
                0 { Invoke-Probe 'shell-a4-select' | Out-Null; Invoke-Probe 'shell-a4-material-clovis' | Out-Null }
                1 { Invoke-Probe 'window-resize' '1280x800' | Out-Null }
                2 { Invoke-Probe 'shell-library' | Out-Null; Invoke-Probe 'shell-album' | Out-Null }
                3 { Invoke-Probe 'shell-a4-material-legacy' | Out-Null }
                4 { Invoke-Probe 'listen' | Out-Null; Start-Sleep -Milliseconds 400; Invoke-Probe 'listen-close' | Out-Null; Invoke-Probe 'shell-album' | Out-Null }
                5 { Invoke-Probe 'window-resize' '1536x900' | Out-Null; Invoke-Probe 'shell-a4-material-clovis' | Out-Null }
                6 { Invoke-Probe 'window-minimize' | Out-Null; Start-Sleep -Milliseconds 500; Invoke-Probe 'window-background-show' | Out-Null; Invoke-Probe 'shell-album' | Out-Null }
                7 { Invoke-Probe 'window-resize' '1100x720' | Out-Null; Invoke-Probe 'play' | Out-Null; Start-Sleep -Milliseconds 700; Invoke-Probe 'play' | Out-Null }
            }
            $actions.Add([pscustomobject]@{
                kind = 'interaction'
                index = $actionIndex
                elapsedSeconds = [Math]::Round($seconds, 2)
                scenario = $actionIndex % 8
            })
            ++$actionIndex
            $nextAction += 12.0
        }
        if ($seconds -ge $nextSample) {
            Add-Sample ("minute-{0}" -f [int]($nextSample / 60.0))
            $nextSample += 60.0
        }
        Start-Sleep -Milliseconds 200
    }
    Add-Sample 'end'
}
catch {
    $failures.Add([pscustomobject]@{
        elapsedSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 2)
        message = $_.Exception.Message
    })
}
finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Invoke-Probe 'app-close' -AllowFailure | Out-Null
            $gracefulExit = $process.WaitForExit(15000)
            if (-not $gracefulExit) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                $process.WaitForExit(5000) | Out-Null
            }
        } else { $gracefulExit = $true }
    }
}

$duration = $watch.Elapsed.TotalMinutes
$result = [pscustomobject][ordered]@{
    schema = 'goden.c9.soak45.v1'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    startedUtc = $startedUtc.ToString('o')
    status = if ($duration -ge $DurationMinutes -and $failures.Count -eq 0 -and $gracefulExit) { 'pass' } else { 'fail' }
    requestedMinutes = $DurationMinutes
    durationMinutes = [Math]::Round($duration, 3)
    processId = if ($process) { $process.Id } else { 0 }
    sameProcess = $true
    componentSha256 = (Get-FileHash -LiteralPath (Join-Path $lab 'app\profile\user-components-x64\foo_nekorox\foo_nekorox.dll') -Algorithm SHA256).Hash
    syntheticFixturesOnly = $true
    tagImportEnabled = $false
    actionCount = $actions.Count
    sampleCount = $samples.Count
    gracefulExit = $gracefulExit
    samples = $samples
    actions = $actions
    failures = $failures
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifest) | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest -Encoding utf8
$result | Select-Object status,durationMinutes,processId,actionCount,sampleCount,gracefulExit | Format-List
if ($result.status -ne 'pass') { throw "C9 W6 soak failed; see $manifest" }
